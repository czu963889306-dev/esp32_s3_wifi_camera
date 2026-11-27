#pragma once

#include <stddef.h>

#include "esp_netif_ip_addr.h"

#define RTSP_URL_FORMAT "rtsp://" IPSTR ":8554/stream"

void stream_url_get(char *url, size_t size);
