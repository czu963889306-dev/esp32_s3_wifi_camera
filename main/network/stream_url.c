#include "stream_url.h"

#include <stdio.h>

#include "config/device_config.h"
#include "wifi_manager.h"

void stream_url_get(char *url, size_t size)
{
    esp_ip4_addr_t ip = wifi_manager_get_ip();
    if (device_mode("http"))
        snprintf(url, size, "http://" IPSTR ":81/", IP2STR(&ip));
    else if (device_mode("rtsp"))
        snprintf(url, size, RTSP_URL_FORMAT, IP2STR(&ip));
    else if (device_mode("webrtc"))
        snprintf(url, size, "http://" IPSTR "/webrtc", IP2STR(&ip));
    else if (size)
        url[0] = 0;
}
