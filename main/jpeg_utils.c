#include "jpeg_utils.h"

#include <stdlib.h>
#include <string.h>

#include "img_converters.h"

esp_err_t camera_frame_to_jpeg(const camera_fb_t *fb, uint8_t quality, uint8_t **out, size_t *out_len)
{
    if (!fb || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = NULL;
    *out_len = 0;

    if (fb->format == PIXFORMAT_JPEG) {
        uint8_t *copy = (uint8_t *)malloc(fb->len);
        if (!copy) {
            return ESP_ERR_NO_MEM;
        }

        memcpy(copy, fb->buf, fb->len);
        *out = copy;
        *out_len = fb->len;
        return ESP_OK;
    }

    if (!frame2jpg((camera_fb_t *)fb, quality, out, out_len)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
