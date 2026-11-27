"""Exercise firmware config validation/persistence against an in-memory NVS."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ConfigTests(unittest.TestCase):
    def test_save_reload_and_reject_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp);(p/'lwip').mkdir()
            (p/'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_NVS_NOT_FOUND 2\n')
            (p/'lwip/sockets.h').write_text('#include <sys/socket.h>\n#include <arpa/inet.h>\n')
            (p/'sdkconfig.h').write_text('''#define CONFIG_CAMERA_ROS_AGENT_IP "192.168.31.69"
#define CONFIG_CAMERA_ROS_TOPIC "/camera/image_raw/compressed"
#define CONFIG_CAMERA_ROS_FRAME_ID "camera_optical_frame"
#define CONFIG_CAMERA_ROS_AGENT_PORT 8888
#define CONFIG_CAMERA_ROS_FPS 25
#define CONFIG_CAMERA_ROS_DOMAIN_ID 0
''')
            (p/'nvs.h').write_text('''#include <stddef.h>
#include <stdint.h>
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
int nvs_open(const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_get_str(nvs_handle_t,const char*,char*,size_t*);
int nvs_get_u16(nvs_handle_t,const char*,uint16_t*);
int nvs_commit(nvs_handle_t);
''')
            (p/'test.c').write_text(r'''
#include "config/device_config.h"
#include "nvs.h"
#include <assert.h>
#include <string.h>
static device_config_t saved;static int exists;static size_t saved_size=sizeof(saved);
int nvs_open(const char*n,int mode,nvs_handle_t*h){(void)mode;*h=1;return strcmp(n,"camera_cfg")?2:0;}
void nvs_close(int h){(void)h;}
int nvs_get_blob(int h,const char*k,void*out,size_t*n){(void)h;(void)k;if(!exists)return 2;assert(*n>=saved_size);memcpy(out,&saved,saved_size);*n=saved_size;return 0;}
int nvs_set_blob(int h,const char*k,const void*in,size_t n){(void)h;(void)k;assert(n==sizeof(saved));memcpy(&saved,in,n);saved_size=n;exists=1;return 0;}
int nvs_get_str(int h,const char*k,char*v,size_t*n){(void)h;(void)k;(void)v;(void)n;return 2;}
int nvs_get_u16(int h,const char*k,uint16_t*v){(void)h;(void)k;(void)v;return 2;}
int nvs_commit(int h){(void)h;return 0;}
int main(void){assert(device_config_init()==0);assert(device_mode("http"));
device_config_t c=*device_config_get();assert(c.fps==25);assert(c.ros_namespace[0]==0);assert(!strcmp(c.agent_ip,"192.168.31.69"));
strcpy(c.mode,"microros");
assert(device_config_save(&c)==0);assert(device_mode("http"));
assert(device_config_init()==0);assert(device_mode("microros"));
strcpy(c.mode,"http");
assert(device_config_save(&c)==0);assert(device_mode("microros"));
assert(device_config_init()==0);assert(device_mode("http"));
strcpy(c.mode,"webrtc");assert(device_config_save(&c)==0);assert(device_config_init()==0);assert(device_mode("webrtc"));
strcpy(c.mode,"rtsp");c.fps=15;assert(device_config_save(&c)==0);assert(device_mode("webrtc"));
device_config_t read;assert(device_config_load(&read)==0);assert(!strcmp(read.mode,"rtsp")&&read.fps==15);
assert(device_config_init()==0);assert(device_mode("rtsp"));
c.fps=0;assert(device_config_save(&c)==1);c.fps=31;assert(device_config_save(&c)==1);c.fps=25;
c.quality=64;assert(device_config_save(&c)==1);c.quality=12;
strcpy(c.mode,"invalid");assert(device_config_save(&c)==1);strcpy(c.mode,"http");
strcpy(c.agent_ip,"999.1.1.1");assert(device_config_save(&c)==1);strcpy(c.agent_ip,"192.168.31.69");
const char *bad_topics[] = {"", "/", "/bad//name", "/1bad", "front/1cam", "front/", "bad name", "bad-name", "a.b"};
for (size_t i = 0; i < sizeof(bad_topics) / sizeof(bad_topics[0]); ++i) {
    strcpy(c.topic, bad_topics[i]);
    assert(device_config_save(&c) == ESP_ERR_INVALID_ARG);
}
strcpy(c.topic, "_cam2/Image_0");
assert(device_config_save(&c) == ESP_OK);
strcpy(c.topic,"/camera/image_raw/compressed");assert(device_config_save(&c)==0);
assert(device_config_load(&read)==0);assert(!strcmp(read.mode,"http"));
char topic[164];
strcpy(c.ros_namespace,"/robot1/front");strcpy(c.topic,"camera/image_raw/compressed");c.agent_port=9999;
assert(device_config_save(&c)==0);assert(device_config_load(&read)==0);
assert(read.agent_port==9999 && !strcmp(read.ros_namespace,"/robot1/front"));
assert(device_config_resolved_topic(&read,topic,sizeof(topic)));assert(!strcmp(topic,"/robot1/front/camera/image_raw/compressed"));
strcpy(c.topic,"/absolute/image");assert(device_config_resolved_topic(&c,topic,sizeof(topic)));assert(!strcmp(topic,"/absolute/image"));
strcpy(c.ros_namespace,"/");strcpy(c.topic,"image");assert(device_config_resolved_topic(&c,topic,sizeof(topic)));assert(!strcmp(topic,"/image"));
assert(!device_config_resolved_topic(&c,topic,3));
c.ros_namespace[0]=0;assert(device_config_save(&c)==0);assert(device_config_load(&read)==0);assert(read.ros_namespace[0]==0);
assert(device_config_resolved_topic(&read,topic,sizeof(topic)));assert(!strcmp(topic,"/image"));

const char *invalid[]={"robot1","/robot//camera","/robot/","/1robot","/bad-name"};
for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){strcpy(c.ros_namespace,invalid[i]);assert(device_config_save(&c)==1);}
memset(c.ros_namespace,'x',sizeof(c.ros_namespace));assert(device_config_save(&c)==1);
strcpy(c.ros_namespace,"/robot");assert(device_config_save(&c)==0);
// Simulate the exact old blob prefix, rather than saving with the new writer.
saved.version=1;saved_size=offsetof(device_config_t,ros_namespace);
assert(device_config_load(&read)==0);assert(read.version==2);assert(!strcmp(read.ros_namespace,""));
assert(read.agent_port==9999 && !strcmp(read.topic,"image"));
assert(device_config_save(&read)==0);assert(saved_size==sizeof(saved));assert(device_config_init()==0);
assert(!strcmp(device_config_get()->ros_namespace,""));
memset(c.mode,'x',sizeof(c.mode));assert(device_config_save(&c)==1);return 0;}
''')
            subprocess.run(['cc','-Wall','-Wextra','-Werror','-I'+str(p),'-I'+str(ROOT/'main'),str(p/'test.c'),str(ROOT/'main/config/device_config.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
