#include "board_control.h"
#include "config/board_config.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "network/wifi_manager.h"

#define BUTTON_POLL_MS       50
#define BUTTON_LONG_PRESS_MS 3000
#define LED_FAST_BLINK_MS    120

static const char *TAG = "BOARD_CONTROL";

static TaskHandle_t s_control_task;

static void board_control_task(void *arg)
{
    bool led_on = false;
    TickType_t pressed_at = 0;
    bool was_pressed = false;
    uint32_t blink_elapsed_ms = 0;
    bool reset_on_release = false;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        bool button_pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;

        if (button_pressed && !was_pressed) {
            pressed_at = xTaskGetTickCount();
        } else if (button_pressed && !reset_on_release &&
                   xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS)) {
            reset_on_release = true;
            ESP_LOGI(TAG, "Release BOOT to restore factory settings");
        } else if (!button_pressed && reset_on_release) {
            reset_on_release = false;
            esp_err_t err = nvs_flash_erase();
            if (err == ESP_OK) {
                esp_restart();
            }
            ESP_LOGE(TAG, "Factory reset failed: %s", esp_err_to_name(err));
        }
        was_pressed = button_pressed;

        if (wifi_manager_is_connected() && !wifi_manager_is_ap_mode()) {
            led_on = true;
            blink_elapsed_ms = 0;
        } else {
            blink_elapsed_ms += BUTTON_POLL_MS;
            if (blink_elapsed_ms >= LED_FAST_BLINK_MS) {
                blink_elapsed_ms = 0;
                led_on = !led_on;
            }
        }
        gpio_set_level(WIFI_LED_GPIO, led_on ? WIFI_LED_ON : WIFI_LED_OFF);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t board_control_start(void)
{
    if (s_control_task) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Configure BOOT button failed");

    io_conf.pin_bit_mask = (1ULL << WIFI_LED_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Configure Wi-Fi LED failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(WIFI_LED_GPIO, WIFI_LED_OFF), TAG, "Set Wi-Fi LED failed");

    BaseType_t ok = xTaskCreate(
        board_control_task,
        "board_control",
        3072,
        NULL,
        5,
        &s_control_task
    );
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
