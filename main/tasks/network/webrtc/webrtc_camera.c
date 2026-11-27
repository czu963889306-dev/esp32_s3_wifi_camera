#include "webrtc_camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config/device_config.h"
#include "platform/camera_init.h"
#include "esp_camera.h"
#include "esp_h264_enc_single_sw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_peer_default.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define MAX_FPS 10
#define MAX_SDP 12288
#define RTC_IDLE BIT0
#define RTC_ANSWER BIT1
#define RTC_STOP BIT2

static const char *TAG = "WEBRTC";
extern const char player_html[] asm("_binary_webrtc_html_start");

// HTTP handlers run serially. Only the peer task touches the peer and encoder.
// The next offer can reuse the session only after the previous task signals IDLE.
static EventGroupHandle_t events;
static char *offer;
static char *answer;
static char session_id[16];

typedef struct {
    esp_peer_handle_t peer;
    esp_h264_enc_handle_t encoder;
    esp_h264_enc_out_frame_t output;
    int64_t capture_wait_us;
    int64_t encode_us;
    unsigned encoded_frames;
    unsigned sent_frames;
    bool connected;
    bool failed;
} video_session_t;

static int on_state(esp_peer_state_t state, void *ctx)
{
    video_session_t *s = ctx;
    ESP_LOGI(TAG, "Peer state %d", (int)state);
    if (state == ESP_PEER_STATE_CONNECTED)
        s->connected = true;
    if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CONNECT_FAILED)
        s->failed = true;
    return 0;
}

static int on_message(esp_peer_msg_t *msg, void *ctx)
{
    video_session_t *s = ctx;
    if (msg->type != ESP_PEER_MSG_TYPE_SDP || answer)
        return 0;
    if (msg->size > 0 && msg->size <= MAX_SDP)
        answer = strndup((const char *)msg->data, msg->size);
    if (!answer) {
        s->failed = true;
        return -1;
    }
    xEventGroupSetBits(events, RTC_ANSWER);
    return 0;
}

static bool encoder_start(video_session_t *s, unsigned fps)
{
    const resolution_info_t *res = &resolution[CAMERA_WEBRTC_FRAME_SIZE];
    esp_h264_enc_cfg_sw_t cfg = {
        .res = {.width = res->width, .height = res->height},
        .pic_type = ESP_H264_RAW_FMT_YUYV, .fps = fps, .gop = fps,
        .rc = {.bitrate = 300000, .qp_min = 20, .qp_max = 40},
    };
    s->output.raw_data.len = res->width * res->height * 3 / 2;
    s->output.raw_data.buffer = heap_caps_malloc(s->output.raw_data.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s->output.raw_data.buffer &&
           esp_h264_enc_sw_new(&cfg, &s->encoder) == ESP_H264_ERR_OK &&
           esp_h264_enc_open(s->encoder) == ESP_H264_ERR_OK;
}

static bool send_frame(video_session_t *s)
{
    const resolution_info_t *res = &resolution[CAMERA_WEBRTC_FRAME_SIZE];
    int64_t start = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    int64_t acquired = esp_timer_get_time();
    s->capture_wait_us += acquired - start;
    if (!fb)
        return false;
    esp_h264_err_t err = ESP_H264_ERR_ARG;
    if (fb->format == PIXFORMAT_YUV422 && fb->width == res->width && fb->height == res->height &&
        fb->len == res->width * res->height * 2) {
        esp_h264_enc_in_frame_t input = {
            .raw_data = {.buffer = fb->buf, .len = fb->len},
            .pts = (uint32_t)(fb->timestamp.tv_sec * 1000ULL + fb->timestamp.tv_usec / 1000),
        };
        err = esp_h264_enc_process(s->encoder, &input, &s->output);
    }
    s->encode_us += esp_timer_get_time() - acquired;
    esp_camera_fb_return(fb);
    if (err != ESP_H264_ERR_OK)
        return false;
    if (!s->output.length)
        return true;
    ++s->encoded_frames;
    esp_peer_video_frame_t frame = {
        .pts = s->output.pts, .data = s->output.raw_data.buffer, .size = s->output.length,
    };
    int ret = esp_peer_send_video(s->peer, &frame);
    if (ret == ESP_PEER_ERR_NONE)
        ++s->sent_frames;
    return ret == ESP_PEER_ERR_NONE || ret == ESP_PEER_ERR_WOULD_BLOCK;
}

static void stream_task(void *arg)
{
    video_session_t s = {0};
    const resolution_info_t *res = &resolution[CAMERA_WEBRTC_FRAME_SIZE];
    unsigned fps = device_config_get()->fps;
    if (fps > MAX_FPS)
        fps = MAX_FPS;

    esp_peer_default_cfg_t defaults = {
        .agent_recv_timeout = 10, .ice_use_lite_mode = true,
        .rtp_cfg = {.send_pool_size = 96 * 1024, .send_queue_num = 96},
    };
    esp_peer_cfg_t cfg = {
        .role = ESP_PEER_ROLE_CONTROLLED, .no_auto_reconnect = true,
        .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .video_info = {.codec = ESP_PEER_VIDEO_CODEC_H264, .width = res->width, .height = res->height, .fps = fps},
        .on_state = on_state, .on_msg = on_message, .ctx = &s,
        .extra_cfg = &defaults, .extra_size = sizeof(defaults),
    };
    esp_peer_msg_t remote = {.type = ESP_PEER_MSG_TYPE_SDP, .data = (uint8_t *)offer, .size = strlen(offer)};
    int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s.peer);
    if (!ret)
        ret = esp_peer_new_connection(s.peer);
    if (!ret)
        ret = esp_peer_send_msg(s.peer, &remote);
    int64_t started = esp_timer_get_time(), next_frame = 0;
    const int64_t period = 1000000 / fps;
    while (!ret && !s.failed && !(xEventGroupGetBits(events) & RTC_STOP)) {
        ret = esp_peer_main_loop(s.peer);
        int64_t now = esp_timer_get_time();
        if (!s.connected && now - started > 20000000)
            break;
        if (s.connected && now >= next_frame) {
            if (!s.encoder && !encoder_start(&s, fps)) {
                ESP_LOGE(TAG, "Cannot start H.264 encoder");
                break;
            }
            if (!send_frame(&s)) {
                ESP_LOGW(TAG, "Failed to encode or send frame");
                break;
            }
            // Keep a steady cadence despite the tick and variable encoding time.
            if (!next_frame || now - next_frame >= period)
                next_frame = now;
            next_frame += period;
        }
        vTaskDelay(1);
    }
    if (s.encoded_frames) {
        ESP_LOGI(TAG, "%u encoded, %u sent, average capture wait %lld us, encode %lld us",
                 s.encoded_frames, s.sent_frames, s.capture_wait_us / s.encoded_frames,
                 s.encode_us / s.encoded_frames);
    }
    if (s.encoder) {
        esp_h264_enc_close(s.encoder);
        esp_h264_enc_del(s.encoder);
    }
    free(s.output.raw_data.buffer);
    if (s.peer)
        esp_peer_close(s.peer);
    free(offer);
    offer = NULL;
    ESP_LOGI(TAG, "Session ended (%d)", ret);
    xEventGroupSetBits(events, RTC_IDLE);
    vTaskDelete(NULL);
}

static esp_err_t player(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, player_html);
}

// Browsers can mask host ICE candidates with mDNS. For same-LAN HTTP
// signaling the socket already identifies the browser's reachable interface.
static void resolve_browser_hosts(httpd_req_t *req, char *sdp)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0)
        return;
    char ip[INET_ADDRSTRLEN];
    const void *ipv4 = NULL;
    if (addr.ss_family == AF_INET)
        ipv4 = &((struct sockaddr_in *)&addr)->sin_addr;
#if LWIP_IPV6
    else if (addr.ss_family == AF_INET6) {
        struct in6_addr *v6 = &((struct sockaddr_in6 *)&addr)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(v6))
            ipv4 = &v6->s6_addr[12];
    }
#endif
    if (!ipv4 || !inet_ntop(AF_INET, ipv4, ip, sizeof(ip)))
        return;

    const size_t ip_len = strlen(ip);
    for (char *line = sdp; *line; ) {
        char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t)(end - line) : strlen(line);
        int start = 0, stop = 0;
        char kind[8] = {0};
        if (!strncmp(line, "a=candidate:", 12)) {
            // Bound the scan to this ICE candidate, excluding subsequent SDP lines.
            if (end) *end = 0;
            sscanf(line, "a=candidate:%*s %*u %*s %*u %n%*s%n %*u typ %7s", &start, &stop, kind);
            if (end) *end = '\n';
        }
        if (stop - start > 6 && !memcmp(line + stop - 6, ".local", 6) && !strcmp(kind, "host")) {
            size_t host_len = stop - start;
            if (ip_len <= host_len || strlen(sdp) + ip_len - host_len < MAX_SDP) {
                memmove(line + start + ip_len, line + stop, strlen(line + stop) + 1);
                memcpy(line + start, ip, ip_len);
                line_len = line_len - host_len + ip_len;
            }
        }
        line += line_len;
        if (*line) ++line;
    }
}

static esp_err_t negotiate(httpd_req_t *req)
{
    if (!(xEventGroupGetBits(events) & RTC_IDLE)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "相机正在为另一位观看者传输视频");
    }
    if (!req->content_len || req->content_len >= MAX_SDP)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SDP size");
    char *data = malloc(MAX_SDP + 1);
    if (!data)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, data + got, req->content_len - got);
        if (n <= 0) {
            free(data);
            return ESP_FAIL;
        }
        got += n;
    }
    data[got] = 0;
    if (strlen(data) != got || strncmp(data, "v=0", 3) || !strstr(data, "m=video ") ||
        !strstr(data, "H264/90000") || !strstr(data, "a=fingerprint:")) {
        free(data);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "需要 H.264 WebRTC 视频 offer");
    }
    resolve_browser_hosts(req, data);
    free(answer);
    answer = NULL;
    offer = data;
    snprintf(session_id, sizeof(session_id), "%08lx", (unsigned long)esp_random());
    xEventGroupClearBits(events, RTC_IDLE | RTC_STOP | RTC_ANSWER);
    if (xTaskCreate(stream_task, "webrtc", 24576, NULL, 5, NULL) != pdPASS) {
        free(offer);
        offer = NULL;
        xEventGroupSetBits(events, RTC_IDLE);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot start WebRTC");
    }
    EventBits_t result = xEventGroupWaitBits(events, RTC_ANSWER | RTC_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(7000));
    if (!(result & RTC_ANSWER) || (result & RTC_IDLE)) {
        xEventGroupSetBits(events, RTC_STOP);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WebRTC negotiation failed");
    }
    httpd_resp_set_type(req, "application/sdp");
    httpd_resp_set_hdr(req, "X-Session", session_id);
    esp_err_t err = httpd_resp_sendstr(req, answer);
    if (err != ESP_OK)
        xEventGroupSetBits(events, RTC_STOP);
    return err;
}

static esp_err_t stop(httpd_req_t *req)
{
    char query[32], id[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", id, sizeof(id)) == ESP_OK && !strcmp(id, session_id))
        xEventGroupSetBits(events, RTC_STOP);
    return httpd_resp_sendstr(req, "");
}

esp_err_t webrtc_camera_register(httpd_handle_t server)
{
    events = xEventGroupCreate();
    if (!events)
        return ESP_ERR_NO_MEM;
    xEventGroupSetBits(events, RTC_IDLE);
    const httpd_uri_t routes[] = {
        {.uri = "/webrtc", .method = HTTP_GET, .handler = player},
        {.uri = "/webrtc/offer", .method = HTTP_POST, .handler = negotiate},
        {.uri = "/webrtc/stop", .method = HTTP_POST, .handler = stop},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK)
            return err;
    }
    return ESP_OK;
}
