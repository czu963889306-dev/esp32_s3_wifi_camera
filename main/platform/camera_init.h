#pragma once

#include "esp_err.h"
#include "sensor.h"

#define CAMERA_WEBRTC_FRAME_SIZE FRAMESIZE_QQVGA

esp_err_t camera_init(void);
