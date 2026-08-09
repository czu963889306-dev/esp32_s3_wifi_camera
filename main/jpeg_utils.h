#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_camera.h"

esp_err_t camera_frame_to_jpeg(const camera_fb_t *fb, uint8_t quality, uint8_t **out, size_t *out_len);
