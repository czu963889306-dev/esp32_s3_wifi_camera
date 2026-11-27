"""Run the firmware RTSP implementation with a host camera fixture and FFmpeg."""
import pathlib
import subprocess
import tempfile
import time
import socket
import unittest
import cv2
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]

class RtspTests(unittest.TestCase):
    def test_tcp_frames_and_protocol(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp)
            headers = {
                'esp_err.h': '#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM -2\ntypedef int esp_err_t;\n',
                'esp_camera.h': '#include <stdint.h>\n#include <stddef.h>\n#include <sys/time.h>\n#define PIXFORMAT_JPEG 1\ntypedef struct {int format; uint8_t *buf; size_t len; struct timeval timestamp;} camera_fb_t;\ncamera_fb_t *esp_camera_fb_get(void);\nvoid esp_camera_fb_return(camera_fb_t *fb);\n',
                'esp_timer.h': '#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
                'esp_random.h': '#include <stdint.h>\nuint32_t esp_random(void);\n',
                'esp_log.h': '#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n',
                'freertos/FreeRTOS.h': '#define pdPASS 1\n#define pdMS_TO_TICKS(ms) (ms)\n',
                'freertos/task.h': 'int xTaskCreate(void (*fn)(void*),const char*,int,void*,int,void*);\nvoid vTaskDelay(int);\n',
                'lwip/sockets.h': '#include <sys/socket.h>\n#include <netinet/in.h>\n#include <arpa/inet.h>\n#include <unistd.h>\n',
            }
            for name, content in headers.items():
                target = p / name
                target.parent.mkdir(exist_ok=True, parents=True)
                target.write_text(content)
            ok, jpeg = cv2.imencode('.jpg', np.random.default_rng(7).integers(0, 256, (480, 640, 3), dtype=np.uint8), [cv2.IMWRITE_JPEG_QUALITY, 50])
            self.assertTrue(ok)
            (p/'frame.jpg').write_bytes(jpeg.tobytes())
            (p/'host.c').write_text(r'''
#include "config/device_config.h"
#include "esp_camera.h"
#include "tasks/network/rtsp/rtsp_jpeg.h"
#include "tasks/network/rtsp/rtsp_server.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
static device_config_t cfg={.fps=25};
const device_config_t *device_config_get(void){return &cfg;}
int64_t esp_timer_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
uint32_t esp_random(void){return rand();}
static camera_fb_t fb;
camera_fb_t *esp_camera_fb_get(void){int64_t t=esp_timer_get_time();fb.timestamp.tv_sec=t/1000000;fb.timestamp.tv_usec=t%1000000;return &fb;}
void esp_camera_fb_return(camera_fb_t *f){(void)f;}
void vTaskDelay(int ms){usleep(ms*1000);}
static void (*task)(void*);static void *arg;
static void *run(void *ignored){(void)ignored;task(arg);return NULL;}
int xTaskCreate(void (*f)(void*),const char*n,int s,void*a,int pr,void*h){(void)n;(void)s;(void)pr;(void)h;task=f;arg=a;pthread_t t;return pthread_create(&t,NULL,run,NULL)==0;}
int main(int argc,char **argv){(void)argc;FILE *f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);fb.len=ftell(f);rewind(f);fb.buf=malloc(fb.len);fread(fb.buf,1,fb.len,f);fclose(f);fb.format=1;
 jpeg_scan_t j;if(!rtsp_jpeg_parse(fb.buf,fb.len,&j))return 2;
 for(size_t n=0;n<fb.len;n++)if(rtsp_jpeg_parse(fb.buf,n,&j))return 3;
 if(rtsp_server_start()!=0)return 4;for(;;)pause();}
''')
            subprocess.run(['cc','-DRTSP_HOST_TEST','-DRTSP_PORT=18554','-g','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'main'),str(p/'host.c'),str(ROOT/'main/tasks/network/rtsp/rtsp_server.c'),str(ROOT/'main/tasks/network/rtsp/rtsp_jpeg.c'),'-pthread','-o',str(p/'server')], check=True)
            server = subprocess.Popen([str(p/'server'), str(p/'frame.jpg')], stderr=subprocess.PIPE)
            try:
                for _ in range(100):
                    self.assertIsNone(server.poll())
                    try:
                        with socket.create_connection(('127.0.0.1',18554),timeout=0.1): pass
                        break
                    except OSError: time.sleep(0.1)
                else: self.fail("RTSP fixture did not start")
                self.assertIsNone(server.poll())
                # A request split across reads must produce one complete response.
                with socket.create_connection(('127.0.0.1',18554),timeout=3) as s:
                    s.sendall(b'OPTIONS rtsp://127.0.0.1:18554/stream RTSP/1.0\r\nCSeq: ')
                    time.sleep(0.03)
                    s.sendall(b'1\r\n\r\n')
                    self.assertIn(b'200 OK',s.recv(4096))
                    s.sendall(b'SETUP rtsp://127.0.0.1:18554/stream/trackID=0 RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP;unicast;client_port=6000-6001\r\n\r\n')
                    self.assertIn(b'461 Unsupported Transport',s.recv(4096))
                # Decode actual RTP/JPEG packets emitted by the firmware source.
                result = subprocess.run(['ffmpeg','-nostdin','-v','error','-rtsp_transport','tcp','-i','rtsp://127.0.0.1:18554/stream','-frames:v','3','-f','rawvideo','-pix_fmt','rgb24',str(p/'decoded.rgb')],capture_output=True,timeout=20)
                self.assertEqual(result.returncode,0,result.stderr.decode())
                self.assertEqual((p/'decoded.rgb').stat().st_size,3*640*480*3)
                # Frames must decode to the same pixels as the original JPEG.
                decoded = np.frombuffer((p/'decoded.rgb').read_bytes(), dtype=np.uint8).reshape(3,480,640,3)
                subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(p/'frame.jpg'),'-frames:v','1','-f','rawvideo','-pix_fmt','rgb24',str(p/'expected.rgb')],check=True,capture_output=True,timeout=10)
                expected = np.frombuffer((p/'expected.rgb').read_bytes(),dtype=np.uint8).reshape(480,640,3)
                np.testing.assert_array_equal(decoded[0],expected)
                self.assertIsNone(server.poll())
            finally:
                server.terminate()
                _, errors = server.communicate(timeout=3)
                self.assertNotIn(b'ERROR: AddressSanitizer',errors)
                self.assertNotIn(b'runtime error:',errors)

if __name__ == '__main__': unittest.main()
