#pragma once

#include "esp_http_server.h"

httpd_handle_t start_config_webserver(void);
void web_config_start_captive_portal(void);
