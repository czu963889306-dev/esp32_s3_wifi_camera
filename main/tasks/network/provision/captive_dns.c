#include "captive_dns.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "network/wifi_manager.h"

static const char *TAG = "CAPTIVE_DNS";

static TaskHandle_t s_dns_task_handle;
static void captive_dns_task(void *arg);
static uint8_t s_dns_request[512];
static uint8_t s_dns_response[512];

void captive_dns_start(void)
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
