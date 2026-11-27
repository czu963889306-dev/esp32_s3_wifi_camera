#include "camera_init.h"
#include "config/board_config.h"
#include "config/device_config.h"

#include "esp_camera.h"
#include "esp_log.h"

#define CAMERA_FRAME_SIZE           FRAMESIZE_VGA
#define CAMERA_PIXEL_FORMAT         PIXFORMAT_JPEG

static const char *TAG = "APP_CAMERA";

static esp_err_t camera_capture_self_test(bool raw_video)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera self-test capture failed");
        return ESP_FAIL;
    }

    bool valid = raw_video ? fb->format == PIXFORMAT_YUV422 && fb->len == fb->width * fb->height * 2
                           : fb->format == PIXFORMAT_JPEG && fb->len >= 2 &&
                             fb->buf[0] == 0xff && fb->buf[1] == 0xd8;
    if (!valid) {
        ESP_LOGE(TAG, "Unexpected camera frame, format=%d len=%u",
                 fb->format, (unsigned int)fb->len);
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera self-test passed: captured %ux%u frame, %u bytes",
             (unsigned int)fb->width,
             (unsigned int)fb->height,
             (unsigned int)fb->len);
    esp_camera_fb_return(fb);
    return ESP_OK;
}

esp_err_t camera_init(void)
{
    bool raw_video = device_mode("webrtc");
    camera_config_t config = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sccb_sda   = CAM_PIN_SIOD,
        .pin_sccb_scl   = CAM_PIN_SIOC,

        .pin_d7         = CAM_PIN_D7,
        .pin_d6         = CAM_PIN_D6,
        .pin_d5         = CAM_PIN_D5,
        .pin_d4         = CAM_PIN_D4,
        .pin_d3         = CAM_PIN_D3,
        .pin_d2         = CAM_PIN_D2,
        .pin_d1         = CAM_PIN_D1,
        .pin_d0         = CAM_PIN_D0,

        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,

        .xclk_freq_hz   = 20000000,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,

        .pixel_format   = raw_video ? PIXFORMAT_YUV422 : CAMERA_PIXEL_FORMAT,
        .frame_size     = raw_video ? CAMERA_WEBRTC_FRAME_SIZE : CAMERA_FRAME_SIZE,
        // JPEG is compressed by the OV3660 sensor before the frame reaches the ESP32-S3.
        .jpeg_quality   = device_config_get()->quality,
        .fb_count       = 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        return err;
    }

    if (raw_video && !esp_camera_get_psram_mode()) {
        err = esp_camera_set_psram_mode(true);
        if (err != ESP_OK)
            return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        camera_sensor_info_t *info = esp_camera_sensor_get_info(&sensor->id);
        if (info) {
            ESP_LOGI(TAG, "Camera sensor detected: %s", info->name);
        } else {
            ESP_LOGI(TAG, "Camera sensor detected successfully");
        }
        sensor->set_vflip(sensor, 1);
        sensor->set_whitebal(sensor, 1);
        sensor->set_wb_mode(sensor, 0);
        sensor->set_special_effect(sensor, 0);
        sensor->set_ae_level(sensor, 0);
        sensor->set_exposure_ctrl(sensor, 1);
        sensor->set_gain_ctrl(sensor, 1);
        sensor->set_contrast(sensor, 0);
        sensor->set_saturation(sensor, 0);
    }

    ESP_LOGI(TAG, "Camera init success");
    return camera_capture_self_test(raw_video);
}
