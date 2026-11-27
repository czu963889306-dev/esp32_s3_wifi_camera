#include "serial_config.h"

#include "config/device_config.h"
#include "esp_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "network/wifi_manager.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define COMMAND_LINE_SIZE 256
static device_config_t pending;
static void print_config(const char *kind, const device_config_t *c) {
    char resolved[164];
    device_config_resolved_topic(c, resolved, sizeof(resolved));
    printf("%s mode=%s fps=%lu quality=%lu agent_ip=%s agent_port=%lu domain_id=%lu topic=%s "
           "namespace=\"%s\" frame_id=%s\n",
           kind, c->mode, (unsigned long)c->fps, (unsigned long)c->quality, c->agent_ip,
           (unsigned long)c->agent_port, (unsigned long)c->domain_id, c->topic, c->ros_namespace,
           c->frame_id);
    printf("%s resolved_topic=%s\n", kind, resolved);
}
static int camera_command(int argc, char **argv) {
    esp_err_t e = ESP_OK;
    if (argc == 2 && !strcmp(argv[1], "get")) {
        print_config("ACTIVE", device_config_get());
        print_config("PENDING", &pending);
        device_config_t saved;
        e = device_config_load(&saved);
        if (e == ESP_OK)
            print_config("SAVED", &saved);
    } else if (argc == 4 && !strcmp(argv[1], "set")) {
        device_config_t next = pending;
        const char *key = argv[2], *value = argv[3];
        char *dst = NULL;
        size_t cap = 0;
        if (!strcmp(key, "mode")) {
            if (!strcasecmp(value, "http"))
                value = "http";
            else if (!strcasecmp(value, "rtsp"))
                value = "rtsp";
            else if (!strcasecmp(value, "webrtc"))
                value = "webrtc";
            else if (!strcasecmp(value, "microros"))
                value = "microros";
            else {
                puts("ERROR mode must be http, rtsp, webrtc or microros");
                return 1;
            }
            dst = next.mode;
            cap = sizeof(next.mode);
        } else if (!strcmp(key, "agent_ip")) {
            dst = next.agent_ip;
            cap = sizeof(next.agent_ip);
        } else if (!strcmp(key, "topic")) {
            dst = next.topic;
            cap = sizeof(next.topic);
        } else if (!strcmp(key, "namespace") || !strcmp(key, "ros_namespace")) {
            dst = next.ros_namespace;
            cap = sizeof(next.ros_namespace);
        } else if (!strcmp(key, "frame_id")) {
            dst = next.frame_id;
            cap = sizeof(next.frame_id);
        }
        if (dst) {
            if (strlen(value) >= cap)
                e = ESP_ERR_INVALID_ARG;
            else
                strcpy(dst, value);
        } else {
            char *end;
            errno = 0;
            unsigned long n = strtoul(value, &end, 10);
            if (errno || !*value || *end || value[0] == '-' || n > UINT32_MAX)
                e = ESP_ERR_INVALID_ARG;
            else if (!strcmp(key, "fps") && n >= 1 && n <= 30)
                next.fps = n;
            else if (!strcmp(key, "quality") && n <= 63)
                next.quality = n;
            else if (!strcmp(key, "domain_id") && n <= 232)
                next.domain_id = n;
            else if (!strcmp(key, "agent_port") && n >= 1 && n <= 65535)
                next.agent_port = n;
            else
                e = ESP_ERR_INVALID_ARG;
        }
        if (e == ESP_OK && !device_config_valid(&next))
            e = ESP_ERR_INVALID_ARG;
        if (e != ESP_OK) {
            printf("ERROR invalid %s; use camera get to inspect current values\n", key);
            return 1;
        }
        pending = next;
        puts("OK staged; run camera save then camera reboot");
    } else if (argc == 2 && !strcmp(argv[1], "save")) {
        e = device_config_save(&pending);
        if (e == ESP_OK)
            puts("OK saved; camera reboot to apply");
    } else if (argc == 2 && !strcmp(argv[1], "reload")) {
        e = device_config_load(&pending);
    } else if (argc == 2 && !strcmp(argv[1], "reboot")) {
        puts("OK rebooting");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        puts("camera get | set "
             "<mode|fps|quality|agent_ip|agent_port|domain_id|topic|namespace|frame_id> "
             "<value> | save | reload | reboot");
        return 1;
    }
    if (e != ESP_OK)
        printf("ERROR %s\n", esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
}
static int wifi_command(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "get")) {
        esp_ip4_addr_t ip = wifi_manager_get_ip();
        printf("mode=%s connected=%d ip=" IPSTR "\n", wifi_manager_get_mode_name(),
               wifi_manager_is_connected(), IP2STR(&ip));
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "set")) {
        esp_err_t e = wifi_manager_save_sta(argv[2], argv[3]);
        printf("%s; reboot to apply\n", esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    puts("wifi get | wifi set <ssid> <password> (quote values containing spaces)");
    return 1;
}
static void usb_command_task(void *arg) {
    char line[COMMAND_LINE_SIZE];
    char chunk[64];
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        int count = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), portMAX_DELAY);
        for (int i = 0; i < count; ++i) {
            char c = chunk[i];
            if (c == '\r' || c == '\n') {
                if (!used && !overflow)
                    continue;
                if (overflow) {
                    puts("ERROR command too long");
                } else {
                    char *argv[8];
                    line[used] = '\0';
                    int argc = esp_console_split_argv(line, argv, sizeof(argv) / sizeof(argv[0]));
                    if (argc && !strcmp(argv[0], "camera"))
                        camera_command(argc, argv);
                    else if (argc && !strcmp(argv[0], "wifi"))
                        wifi_command(argc, argv);
                    else
                        puts("Commands: camera get | camera set <key> <value> | camera save | camera reload | camera reboot | wifi get | wifi set <ssid> <password>");
                }
                fflush(stdout);
                used = 0;
                overflow = false;
            } else if (c == '\b' || c == 0x7f) {
                if (used)
                    --used;
            } else if (used < sizeof(line) - 1 && !overflow) {
                line[used++] = c;
            } else {
                overflow = true;
            }
        }
    }
}

esp_err_t serial_config_start(void) {
    pending = *device_config_get();
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK)
        return err;
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(usb_command_task, "usb_commands", 6144, NULL, 5, NULL) != pdPASS) {
        usb_serial_jtag_vfs_use_nonblocking();
        usb_serial_jtag_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
