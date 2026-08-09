#include "web_mjpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"

#include "img_converters.h"

static const char *TAG = "WEB_MJPEG";

#define STREAM_BOUNDARY "frame"

static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY;

static const char *STREAM_BOUNDARY_LINE =
    "\r\n--" STREAM_BOUNDARY "\r\n";

static const char *STREAM_PART_HEADER =
    "Content-Type: image/jpeg\r\n\r\n";

typedef struct {
    httpd_req_t *req;
    size_t len;
    bool ok;
} jpg_chunking_t;

#define STREAM_JPEG_QUALITY 20

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len)
{
    jpg_chunking_t *j = (jpg_chunking_t *)arg;

    if (!index) {
        j->len = 0;
        j->ok = true;
    }

    if (!j->ok) {
        return 0;
    }

    if (len == 0) {
        return 0;
    }

    if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
        j->ok = false;
        return 0;
    }

    j->len += len;
    return len;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    ESP_LOGI(TAG, "MJPEG client connected");

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();

        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed during stream");
            return ESP_FAIL;
        }

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY_LINE, strlen(STREAM_BOUNDARY_LINE));
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        res = httpd_resp_send_chunk(req, STREAM_PART_HEADER, strlen(STREAM_PART_HEADER));
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        jpg_chunking_t jchunk = {
            .req = req,
            .len = 0,
            .ok = false,
        };

        if (!frame2jpg_cb(fb, STREAM_JPEG_QUALITY, jpg_encode_stream, &jchunk) || !jchunk.ok) {
            ESP_LOGE(TAG, "Streaming frame JPEG conversion failed");
            esp_camera_fb_return(fb);
            return ESP_FAIL;
        }

        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            break;
        }
    }

    ESP_LOGW(TAG, "MJPEG client disconnected");
    return res;
}

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 4;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", ret);
        return NULL;
    }

    httpd_uri_t root_stream_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_stream_uri));

    ESP_LOGI(TAG, "HTTP MJPEG stream started: http://<ESP32_IP>:%d/", config.server_port);
    return server;
}
