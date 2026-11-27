#include "app_runtime.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "config/device_config.h"
#include "network/udp_discovery.h"
#include "network/wifi_manager.h"
#include "platform/board_control.h"
#include "platform/camera_init.h"
#include "tasks/io/i2c_slave_comm.h"
#include "tasks/io/serial_config.h"
#include "tasks/network/http/web_mjpeg.h"
#include "tasks/network/microros/ros_camera.h"
#include "tasks/network/provision/web_config.h"
#include "tasks/network/rtsp/rtsp_server.h"

static const char *TAG = "GC0308_STREAM";

void app_runtime_startup(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(board_control_start());
    ret = device_config_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera config load failed (%s). Hold BOOT for factory reset", esp_err_to_name(ret));
        return;
    }
    ESP_ERROR_CHECK(serial_config_start());

    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(i2c_slave_comm_start());
    ESP_ERROR_CHECK(camera_init());

    if (!start_config_webserver()) {
        ESP_LOGW(TAG, "Wi-Fi config webserver not started");
    }

    if (device_mode("http")) {
        if (!start_webserver()) {
            ESP_LOGE(TAG, "HTTP stream failed to start");
        }
    } else if (device_mode("rtsp")) {
        ESP_ERROR_CHECK(rtsp_server_start());
    } else if (device_mode("microros")) {
        ESP_ERROR_CHECK(ros_camera_start());
    }

    ESP_ERROR_CHECK(udp_discovery_start());
}
