#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    const uint8_t *scan;
    size_t len;
    uint16_t width, height;
    uint8_t type, quant[128];
} jpeg_scan_t;
bool rtsp_jpeg_parse(const uint8_t *data, size_t len, jpeg_scan_t *out);
