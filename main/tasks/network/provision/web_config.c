#include "web_config.h"
#include "captive_dns.h"
#include "provision_page.h"
#include "network/stream_url.h"
#include "tasks/network/webrtc/webrtc_camera.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "network/wifi_manager.h"
#include "config/device_config.h"

static const char *TAG = "WEB_CONFIG";

static esp_err_t captive_probe_handler(httpd_req_t *req)
{
    char location[64];
    snprintf(location, sizeof(location), "http://%s/", WIFI_MANAGER_AP_IP);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "", 0);
}

typedef enum { FIELD_MISSING, FIELD_VALID, FIELD_INVALID } form_field_t;

static form_field_t form_get_value(const char *body, const char *key, char *value, size_t capacity)
{
    size_t key_len = strlen(key);
    for (const char *p = body; *p; ) {
        const char *end = p + strcspn(p, "&");
        if ((size_t)(end - p) > key_len && p[key_len] == '=' && !memcmp(p, key, key_len)) {
            size_t len = 0;
            for (p += key_len + 1; p < end; ++p) {
                char ch = *p;
                if (ch == '%') {
                    if (end - p < 3 || !isxdigit((unsigned char)p[1]) ||
                        !isxdigit((unsigned char)p[2])) {
                        return FIELD_INVALID;
                    }
                    char hex[] = {p[1], p[2], 0};
                    ch = (char)strtoul(hex, NULL, 16);
                    p += 2;
                } else if (ch == '+') {
                    ch = ' ';
                }
                if (!ch || len + 1 >= capacity) {
                    return FIELD_INVALID;
                }
                value[len++] = ch;
            }
            value[len] = '\0';
            return FIELD_VALID;
        }
        p = *end ? end + 1 : end;
    }
    return FIELD_MISSING;
}

static esp_err_t wifi_page_handler(httpd_req_t *req)
{
    char html[4096];
    esp_ip4_addr_t ip = wifi_manager_get_ip();
    device_config_t saved;
    if (device_config_load(&saved) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "读取视频配置失败");
    }

    int len = snprintf(
        html,
        sizeof(html),
        provision_page_format,
        wifi_manager_get_mode_name(),
        device_config_get()->mode,
        IP2STR(&ip),
        !strcmp(saved.mode, "http") ? " selected" : "",
        !strcmp(saved.mode, "microros") ? " selected" : "",
        !strcmp(saved.mode, "rtsp") ? " selected" : "",
        !strcmp(saved.mode, "webrtc") ? " selected" : "",
        !strcmp(saved.mode, "rtsp") ? "" : " hidden",
        IP2STR(&ip),
        device_mode("rtsp") ? "" : "<br>保存视频设置并重启后可播放。",
        !strcmp(saved.mode, "webrtc") ? "" : " hidden",
        device_mode("webrtc") ? "<a href=\"/webrtc\">观看 WebRTC 视频</a>"
                              : "保存视频设置并重启后，打开 /webrtc 观看。",
        !strcmp(saved.mode, "microros") ? "" : " hidden disabled",
        saved.agent_ip,
        (unsigned long)saved.agent_port,
        saved.topic,
        (unsigned)(sizeof(saved.topic) - 1),
        (unsigned long)saved.domain_id,
        WIFI_MANAGER_AP_IP
    );

    if (len <= 0 || len >= sizeof(html)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "页面内容过大");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_video_form(const char *body, bool set_mode)
{
    device_config_t saved;
    esp_err_t err = device_config_load(&saved);
    if (err != ESP_OK) {
        return err;
    }
    if (set_mode && form_get_value(body, "mode", saved.mode, sizeof(saved.mode)) != FIELD_VALID) {
        return ESP_ERR_INVALID_ARG;
    }

    // Switching away from micro-ROS preserves its saved settings, including for older forms.
    if (strcmp(saved.mode, "microros") != 0) {
        return set_mode ? device_config_save(&saved) : ESP_OK;
    }
    // A missing topic keeps the saved value for clients predating the topic field.
    if (set_mode && form_get_value(body, "topic", saved.topic, sizeof(saved.topic)) == FIELD_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }

    char port_text[6];
    form_field_t ip = form_get_value(body, "agent_ip", saved.agent_ip, sizeof(saved.agent_ip));
    form_field_t port = form_get_value(body, "agent_port", port_text, sizeof(port_text));
    if (ip != FIELD_MISSING || port != FIELD_MISSING) {
        if (ip != FIELD_VALID || port != FIELD_VALID || !*port_text ||
            strspn(port_text, "0123456789") != strlen(port_text)) {
            return ESP_ERR_INVALID_ARG;
        }
        saved.agent_port = strtoul(port_text, NULL, 10);
    } else if (!set_mode) {
        return ESP_OK;
    }
    return device_config_save(&saved);
}

static esp_err_t save_wifi_form(const char *body)
{
    char ssid[33], password[65], enable[4];
    form_field_t enabled = form_get_value(body, "-wf_enable", enable, sizeof(enable));
    if ((enabled != FIELD_MISSING && (enabled != FIELD_VALID || strcmp(enable, "1"))) ||
        form_get_value(body, "-wf_ssid", ssid, sizeof(ssid)) != FIELD_VALID || !*ssid ||
        form_get_value(body, "-wf_key", password, sizeof(password)) != FIELD_VALID ||
        (*password && strlen(password) < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Older provisioning clients may include the Agent settings with their credentials.
    esp_err_t err = save_video_form(body, false);
    return err == ESP_OK ? wifi_manager_save_sta(ssid, password) : err;
}

static esp_err_t wifi_param_handler(httpd_req_t *req)
{
    char body[512];
    int received = 0;

    if (req->content_len <= 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "表单长度无效");
    }

    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char command[32];
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (form_get_value(body, "cmd", command, sizeof(command)) == FIELD_VALID) {
        if (!strcmp(command, "setvideomode")) {
            err = save_video_form(body, true);
        } else if (!strcmp(command, "setwirelessattr")) {
            err = save_wifi_form(body);
        }
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "配置参数无效");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存配置失败");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "保存成功，设备即将重启\n");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handshake_handler(httpd_req_t *req)
{
    const device_config_t *config = device_config_get();
    char json[1024];
    char resolved_topic[164];
    device_config_resolved_topic(config, resolved_topic, sizeof(resolved_topic));
    esp_ip4_addr_t ip = wifi_manager_get_ip();
    const char *device_name = wifi_manager_get_device_name();
    char stream_url[64];
    stream_url_get(stream_url, sizeof(stream_url));

    int len = snprintf(
        json,
        sizeof(json),
        "{"
        "\"ok\":true,"
        "\"name\":\"%s\","
        "\"ip\":\"" IPSTR "\","
        "\"mode\":\"%s\","
        "\"config_url\":\"http://" IPSTR "/\","
        "\"stream_url\":\"%s\","
        "\"video_mode\":\"%s\","
        "\"ros_topic\":\"%s\","
        "\"ros_namespace\":\"%s\","
        "\"ros_resolved_topic\":\"%s\","
        "\"ros_type\":\"sensor_msgs/msg/CompressedImage\","
        "\"ros_frame_id\":\"%s\","
        "\"ros_domain_id\":%d,"
        "\"agent_ip\":\"%s\","
        "\"agent_port\":%u"
        "}\n",
        device_name,
        IP2STR(&ip),
        wifi_manager_get_mode_name(),
        IP2STR(&ip),
        stream_url,
        config->mode,
        config->topic,
        config->ros_namespace,
        resolved_topic,
        config->frame_id,
        (int)config->domain_id,
        config->agent_ip,
        (unsigned)config->agent_port
    );

    if (len <= 0 || len >= sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Handshake response too large");
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

httpd_handle_t start_config_webserver(void)
{
    httpd_handle_t server = NULL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", ret);
        return NULL;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = wifi_page_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t wifi_uri = {
        .uri = "/web/wifi.html",
        .method = HTTP_GET,
        .handler = wifi_page_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t param_uri = {
        .uri = "/web/cgi-bin/his3510/param.cgi",
        .method = HTTP_POST,
        .handler = wifi_param_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t handshake_uri = {
        .uri = "/handshake",
        .method = HTTP_GET,
        .handler = handshake_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_handshake_uri = {
        .uri = "/api/handshake",
        .method = HTTP_GET,
        .handler = handshake_handler,
        .user_ctx = NULL,
    };

    // Captive-portal probe URLs used by Android, iOS, Windows and Linux.
    // Redirecting them to the root page makes the Wi-Fi settings page open
    // automatically after joining the provisioning AP.
    static const char *const probe_paths[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt",
        "/ncsi.txt", "/success.txt", "/canonical.html", "/fwlink",
        "/library/test/success.html", "/mobile/status.php", "/redirect",
        "/check_network_status.txt",
    };
    for (size_t i = 0; i < sizeof(probe_paths) / sizeof(probe_paths[0]); ++i) {
        httpd_uri_t probe_uri = {
            .uri = probe_paths[i],
            .method = HTTP_GET,
            .handler = captive_probe_handler,
            .user_ctx = NULL,
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &probe_uri));
    }

    if (device_mode("webrtc")) ESP_ERROR_CHECK(webrtc_camera_register(server));
    captive_dns_start();

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &param_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &handshake_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api_handshake_uri));

    ESP_LOGI(TAG, "Wi-Fi config webserver started, handshake API enabled");
    return server;
}
