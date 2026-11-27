#pragma once

#include "driver/gpio.h"

#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_XCLK     15
#define CAM_PIN_PCLK     13
#define CAM_PIN_RESET    -1
#define CAM_PIN_PWDN     -1
#define CAM_PIN_D0       11
#define CAM_PIN_D1       9
#define CAM_PIN_D2       8
#define CAM_PIN_D3       10
#define CAM_PIN_D4       12
#define CAM_PIN_D5       18
#define CAM_PIN_D6       17
#define CAM_PIN_D7       16

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define WIFI_LED_GPIO    GPIO_NUM_2
#define WIFI_LED_ON      0
#define WIFI_LED_OFF     1

#define I2C_SLAVE_COMM_ADDR 0x42
#define I2C_SLAVE_COMM_SCL_GPIO 47
#define I2C_SLAVE_COMM_SDA_GPIO 48
