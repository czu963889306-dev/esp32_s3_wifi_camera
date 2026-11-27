// Minimal single-client RTSP/1.0 server, RTP/JPEG (RFC 2435) interleaved over TCP.
#include "rtsp_server.h"

#include "config/device_config.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "rtsp_jpeg.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef RTSP_PORT
#define RTSP_PORT 8554
#endif
static const char *TAG = "RTSP";
static bool send_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        int n = send(fd, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= n;
    }
    return true;
}
static bool reply(int fd, int seq, int code, const char *reason, const char *extra,
                  const char *body) {
    char h[1024];
    size_t size = body ? strlen(body) : 0;
    int n = snprintf(h, sizeof(h), "RTSP/1.0 %d %s\r\nCSeq: %d\r\n%sContent-Length: %u\r\n\r\n",
                     code, reason, seq, extra ? extra : "", (unsigned)size);
    return n > 0 && n < sizeof(h) && send_all(fd, h, n) && (!size || send_all(fd, body, size));
}
static bool header(const char *req, const char *name, char *out, size_t cap) {
    size_t len = strlen(name);
    const char *p = strstr(req, "\r\n");
    while (p && p[2]) {
        p += 2;
        const char *end = strstr(p, "\r\n");
        if (!end)
            return false;
        if (!strncasecmp(p, name, len) && p[len] == ':') {
            p += len + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t n = end - p;
            if (n >= cap)
                return false;
            memcpy(out, p, n);
            out[n] = 0;
            return true;
        }
        p = end;
    }
    return false;
}
static bool number(const char *s, unsigned *value) {
    if (*s < '0' || *s > '9')
        return false;
    errno = 0;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v > 2147483647UL)
        return false;
    *value = v;
    return true;
}
static bool send_frame(int fd, uint8_t channel, uint16_t *sequence, uint32_t ssrc) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
        return true;
    jpeg_scan_t j;
    if (fb->format != PIXFORMAT_JPEG || !rtsp_jpeg_parse(fb->buf, fb->len, &j)) {
        ESP_LOGW(TAG, "Unsupported JPEG layout");
        esp_camera_fb_return(fb);
        return true;
    }
    uint32_t stamp =
        (uint32_t)(((int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec) * 9 / 100);
    bool ok = true;
    for (size_t offset = 0; offset < j.len && ok;) {
        uint8_t packet[1400];
        size_t n = j.len - offset;
        if (n > 1200)
            n = 1200;
        size_t h = 24; // 4-byte TCP framing, RTP header, JPEG payload header.
        packet[0] = '$';
        packet[1] = channel;
        packet[4] = 0x80;
        packet[5] = 26 | ((offset + n == j.len) ? 0x80 : 0);
        uint16_t seq = (*sequence)++;
        packet[6] = seq >> 8;
        packet[7] = seq;
        for (int k = 0; k < 4; k++) {
            packet[8 + k] = stamp >> (24 - k * 8);
            packet[12 + k] = ssrc >> (24 - k * 8);
        }
        packet[16] = 0;
        packet[17] = offset >> 16;
        packet[18] = offset >> 8;
        packet[19] = offset;
        packet[20] = j.type;
        packet[21] = 255;
        packet[22] = j.width / 8;
        packet[23] = j.height / 8;
        if (!offset) {
            packet[h++] = 0;
            packet[h++] = 0;
            packet[h++] = 0;
            packet[h++] = 128;
            memcpy(packet + h, j.quant, 128);
            h += 128;
        }
        memcpy(packet + h, j.scan + offset, n);
        size_t length = h + n - 4;
        packet[2] = length >> 8;
        packet[3] = length;
        ok = send_all(fd, packet, h + n);
        offset += n;
    }
    esp_camera_fb_return(fb);
    return ok;
}
static void client_loop(int fd) {
    char buf[4096];
    size_t used = 0;
    bool setup = false, playing = false, alive = true;
    uint8_t channel = 0;
    uint16_t sequence = esp_random();
    uint32_t ssrc = esp_random(), session = esp_random();
    int64_t next = 0, last = esp_timer_get_time();
    while (alive && esp_timer_get_time() - last < 60000000) {
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(fd, &readset);
        struct timeval wait = {.tv_usec = 10000};
        int ready = select(fd + 1, &readset, NULL, NULL, &wait);
        if (ready < 0)
            break;
        if (ready > 0) {
            int n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
            if (n <= 0)
                break;
            used += n;
            buf[used] = 0;
            while (used && alive) {
                // Consume interleaved RTCP packets, including split TCP reads.
                if ((uint8_t)buf[0] == '$') {
                    if (used < 4)
                        break;
                    size_t size = 4 + ((uint8_t)buf[2] << 8) + (uint8_t)buf[3];
                    if (size >= sizeof(buf)) {
                        alive = false;
                        break;
                    }
                    if (used < size)
                        break;
                    memmove(buf, buf + size, used - size);
                    used -= size;
                    buf[used] = 0;
                    last = esp_timer_get_time();
                    continue;
                }
                char *end = strstr(buf, "\r\n\r\n");
                if (!end)
                    break;
                size_t size = end - buf + 4;
                char req[4096];
                memcpy(req, buf, size);
                req[size] = 0;
#ifdef RTSP_HOST_TEST
                fprintf(stderr, "REQUEST: %s", req);
#endif
                char value[256];
                unsigned seq = 0, bodylen = 0;
                if (!header(req, "CSeq", value, sizeof(value)) || !number(value, &seq)) {
                    alive = reply(fd, 0, 400, "Bad Request", NULL, NULL);
                    used = 0;
                    break;
                }
                if (header(req, "Content-Length", value, sizeof(value)) &&
                    (!number(value, &bodylen) || bodylen > 1024)) {
                    alive = false;
                    break;
                }
                if (size + bodylen >= sizeof(buf)) {
                    alive = false;
                    break;
                }
                if (used < size + bodylen)
                    break;
                size += bodylen;
                memmove(buf, buf + size, used - size);
                used -= size;
                buf[used] = 0;
                last = esp_timer_get_time();
                char method[24], url[256], version[16];
                if (sscanf(req, "%23s %255s %15s", method, url, version) != 3 ||
                    strcmp(version, "RTSP/1.0")) {
                    alive = reply(fd, seq, 400, "Bad Request", NULL, NULL);
                    continue;
                }
                char session_hdr[80];
                snprintf(session_hdr, sizeof(session_hdr), "Session: %08" PRIx32 ";timeout=60\r\n",
                         session);
                if (!strcmp(method, "OPTIONS"))
                    alive = reply(fd, seq, 200, "OK",
                                  "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, GET_PARAMETER, "
                                  "TEARDOWN\r\n",
                                  NULL);
                else if (!strcmp(method, "DESCRIBE")) {
                    const char *sdp = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=ESP32 Camera\r\nc=IN IP4 "
                                      "0.0.0.0\r\nt=0 0\r\na=control:*\r\nm=video 0 RTP/AVP "
                                      "26\r\na=rtpmap:26 JPEG/90000\r\na=control:trackID=0\r\n";
                    char extra[384];
                    snprintf(extra, sizeof(extra),
                             "Content-Type: application/sdp\r\nContent-Base: %s%s\r\n", url,
                             url[strlen(url) - 1] == '/' ? "" : "/");
                    alive = reply(fd, seq, 200, "OK", extra, sdp);
                } else if (!strcmp(method, "SETUP")) {
                    unsigned first = 0, second = 1;
                    char tail;
                    if (!header(req, "Transport", value, sizeof(value)) ||
                        strncmp(value, "RTP/AVP/TCP", 11) || strstr(value, "multicast")) {
                        alive = reply(fd, seq, 461, "Unsupported Transport", NULL, NULL);
                        continue;
                    }
                    char *p = strstr(value, "interleaved=");
                    if (!p || sscanf(p + 12, "%u-%u%c", &first, &second, &tail) < 2 ||
                        first > 254 || second != first + 1) {
                        alive = reply(fd, seq, 461, "Unsupported Transport", NULL, NULL);
                        continue;
                    }
                    channel = first;
                    setup = true;
                    char extra[256];
                    snprintf(extra, sizeof(extra),
                             "%sTransport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08" PRIx32
                             "\r\n",
                             session_hdr, first, second, ssrc);
                    alive = reply(fd, seq, 200, "OK", extra, NULL);
                } else {
                    char expected[16];
                    snprintf(expected, sizeof(expected), "%08" PRIx32, session);
                    if (!setup || !header(req, "Session", value, sizeof(value)) ||
                        strncmp(value, expected, 8) || (value[8] && value[8] != ';')) {
                        alive = reply(fd, seq, 454, "Session Not Found", NULL, NULL);
                        continue;
                    }
                    if (!strcmp(method, "PLAY")) {
                        playing = true;
                        alive = reply(fd, seq, 200, "OK", session_hdr, NULL);
                    } else if (!strcmp(method, "PAUSE")) {
                        playing = false;
                        alive = reply(fd, seq, 200, "OK", session_hdr, NULL);
                    } else if (!strcmp(method, "GET_PARAMETER"))
                        alive = reply(fd, seq, 200, "OK", session_hdr, NULL);
                    else if (!strcmp(method, "TEARDOWN")) {
                        reply(fd, seq, 200, "OK", session_hdr, NULL);
                        alive = false;
                    } else
                        alive = reply(fd, seq, 405, "Method Not Allowed", NULL, NULL);
                }
            }
            if (used == sizeof(buf) - 1)
                break;
        }
        int64_t now = esp_timer_get_time();
        if (alive && playing && now >= next) {
            next = now + 1000000 / device_config_get()->fps;
            alive = send_frame(fd, channel, &sequence, ssrc);
        }
        vTaskDelay(1);
    }
}
static void server_task(void *arg) {
    int listener = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        struct timeval timeout = {.tv_sec = 2};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        client_loop(fd);
        close(fd);
    }
}
esp_err_t rtsp_server_start(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return ESP_FAIL;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(RTSP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 1)) {
        close(fd);
        return ESP_FAIL;
    }
    if (xTaskCreate(server_task, "rtsp", 16384, (void *)(intptr_t)fd, 4, NULL) != pdPASS) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RTSP ready: rtsp://<device-ip>:8554/stream (TCP, one client)");
    return ESP_OK;
}
