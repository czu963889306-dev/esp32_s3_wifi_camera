#include "web_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "wifi_manager.h"

static const char *TAG = "WEB_CONFIG";

static TaskHandle_t s_dns_task_handle;
static void captive_dns_task(void *arg);
static uint8_t s_dns_request[512];
static uint8_t s_dns_response[512];

void web_config_start_captive_portal(void)
{
    if (s_dns_task_handle || !wifi_manager_is_ap_mode()) {
        return;
    }

    BaseType_t task_ok = xTaskCreate(captive_dns_task, "captive_dns", 6144,
                                     NULL, 4, &s_dns_task_handle);
    if (task_ok != pdPASS) {
        s_dns_task_handle = NULL;
        ESP_LOGW(TAG, "Failed to start captive DNS task");
    }
}

static void captive_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Captive DNS socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Captive DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    const uint32_t ap_ip = inet_addr(WIFI_MANAGER_AP_IP);
    ESP_LOGI(TAG, "Captive DNS started, redirecting queries to %s", WIFI_MANAGER_AP_IP);

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int request_len = recvfrom(sock, s_dns_request, sizeof(s_dns_request), 0,
                                   (struct sockaddr *)&client_addr, &client_len);
        if (request_len < 12 || request_len > (int)sizeof(s_dns_response) - 16) {
            continue;
        }

        // Return one A record for every DNS query. The question section is
        // copied unchanged so clients can parse the response normally.
        memcpy(s_dns_response, s_dns_request, 2);
        s_dns_response[2] = 0x81;
        s_dns_response[3] = 0x80;
        s_dns_response[4] = 0;
        s_dns_response[5] = 1;
        s_dns_response[6] = 0;
        s_dns_response[7] = 1;
        s_dns_response[8] = s_dns_response[9] = s_dns_response[10] = s_dns_response[11] = 0;
        memcpy(s_dns_response + 12, s_dns_request + 12, request_len - 12);
        int response_len = request_len;
        s_dns_response[response_len++] = 0xc0;
        s_dns_response[response_len++] = 0x0c;
        s_dns_response[response_len++] = 0;
        s_dns_response[response_len++] = 1;
        s_dns_response[response_len++] = 0;
        s_dns_response[response_len++] = 1;
        s_dns_response[response_len++] = 0;
        s_dns_response[response_len++] = 0;
        s_dns_response[response_len++] = 0;
        s_dns_response[response_len++] = 60;
        s_dns_response[response_len++] = 0;
        s_dns_response[response_len++] = 4;
        memcpy(s_dns_response + response_len, &ap_ip, sizeof(ap_ip));
        response_len += sizeof(ap_ip);
        sendto(sock, s_dns_response, response_len, 0,
               (struct sockaddr *)&client_addr, client_len);
    }
}

static esp_err_t captive_probe_handler(httpd_req_t *req)
{
    char location[64];
    snprintf(location, sizeof(location), "http://%s/", WIFI_MANAGER_AP_IP);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "", 0);
}

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t di = 0;

    for (size_t si = 0; si < src_len && di + 1 < dst_len; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && si + 2 < src_len) {
            unsigned int value;
            if (sscanf(&src[si + 1], "%2x", &value) == 1) {
                dst[di++] = (char)value;
                si += 2;
            }
        } else {
            dst[di++] = src[si];
        }
    }

    dst[di] = '\0';
}

static bool form_get_value(
    const char *body,
    const char *key,
    char *value,
    size_t value_len
)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) {
            return false;
        }

        const char *amp = strchr(eq + 1, '&');
        if (!amp) {
            amp = body + strlen(body);
        }

        if ((size_t)(eq - p) == key_len && memcmp(p, key, key_len) == 0) {
            url_decode(value, value_len, eq + 1, amp - eq - 1);
            return true;
        }

        p = *amp ? amp + 1 : amp;
    }

    return false;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t wifi_page_handler(httpd_req_t *req)
{
    char html[3072];
    esp_ip4_addr_t ip = wifi_manager_get_ip();

    int len = snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Maturo图传相机配网页面</title>"
        "<style>"
        "body{margin:0;font-family:Arial,sans-serif;background:#f4f6f8;color:#1f2933;}"
        "main{max-width:560px;margin:40px auto;padding:24px;background:#fff;border:1px solid #d9e2ec;border-radius:8px;}"
        "h1{font-size:22px;margin:0 0 16px;}"
        "p{font-size:14px;line-height:1.5;color:#52606d;}"
        "label{display:block;font-size:14px;margin:16px 0 6px;}"
        "input{box-sizing:border-box;width:100%%;height:42px;padding:0 12px;border:1px solid #bcccdc;border-radius:6px;font-size:15px;}"
        "button{width:100%%;height:42px;margin-top:20px;border:0;border-radius:6px;background:#ff9800;color:white;font-size:15px;font-weight:700;}"
        ".meta{padding:10px 12px;background:#f0f4f8;border-radius:6px;}"
        "</style></head><body><main>"
        "<h1>Maturo图传相机配网页面</h1>"
        "<p class=\"meta\">当前模式: %s<br>配网地址: http://" IPSTR "/<br>视频地址: http://" IPSTR ":81/</p>"
        "<form method=\"post\" action=\"/web/cgi-bin/his3510/param.cgi\">"
        "<input type=\"hidden\" name=\"cmd\" value=\"setwirelessattr\">"
        "<input type=\"hidden\" name=\"cururl\" value=\"/web/wifi.html\">"
        "<input type=\"hidden\" name=\"-wf_auth\" value=\"3\">"
        "<input type=\"hidden\" name=\"-wf_mode\" value=\"0\">"
        "<input type=\"hidden\" name=\"-wf_enc\" value=\"0\">"
        "<input type=\"hidden\" name=\"-wf_enable\" value=\"1\">"
        "<label>WiFi 名称</label><input name=\"-wf_ssid\" maxlength=\"32\" required>"
        "<label>WiFi 密码</label><input name=\"-wf_key\" type=\"password\" maxlength=\"64\" required>"
        "<button type=\"submit\">保存并重启</button>"
        "</form>"
        "<p>连接路由器后使用路由器分配的 IP；配网热点模式使用 http://%s/。</p>"
        "</main></body></html>",
        wifi_manager_get_mode_name(),
        IP2STR(&ip),
        IP2STR(&ip),
        WIFI_MANAGER_AP_IP
    );

    if (len <= 0 || len >= sizeof(html)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "页面内容过大");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
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

    char command[32] = {0};
    char ssid[33] = {0};
    char password[65] = {0};
    char enable[4] = {0};

    if (!form_get_value(body, "cmd", command, sizeof(command)) ||
        strcmp(command, "setwirelessattr") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "不支持的命令");
    }

    if (form_get_value(body, "-wf_enable", enable, sizeof(enable)) &&
        strcmp(enable, "1") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "不支持关闭 WiFi");
    }

    if (!form_get_value(body, "-wf_ssid", ssid, sizeof(ssid))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 WiFi 名称");
    }
    if (!form_get_value(body, "-wf_key", password, sizeof(password))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 WiFi 密码");
    }

    esp_err_t err = wifi_manager_save_sta(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Save Wi-Fi config failed: 0x%x", err);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WiFi 配置无效");
    }

    ESP_LOGI(TAG, "Wi-Fi config saved by setwirelessattr, restarting");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "保存成功，设备即将重启\n");

    xTaskCreate(restart_task, "wifi_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t handshake_handler(httpd_req_t *req)
{
    char json[256];
    esp_ip4_addr_t ip = wifi_manager_get_ip();
    const char *device_name = wifi_manager_get_device_name();

    int len = snprintf(
        json,
        sizeof(json),
        "{"
        "\"ok\":true,"
        "\"name\":\"%s\","
        "\"ip\":\"" IPSTR "\","
        "\"mode\":\"%s\","
        "\"config_url\":\"http://" IPSTR "/\","
        "\"stream_url\":\"http://" IPSTR ":81/\""
        "}\n",
        device_name,
        IP2STR(&ip),
        wifi_manager_get_mode_name(),
        IP2STR(&ip),
        IP2STR(&ip)
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

    web_config_start_captive_portal();

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &param_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &handshake_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api_handshake_uri));

    ESP_LOGI(TAG, "Wi-Fi config webserver started, handshake API enabled");
    return server;
}
