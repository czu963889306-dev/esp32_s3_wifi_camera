#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t version;
    char mode[12], agent_ip[16], topic[96], frame_id[64];
    uint32_t agent_port, fps, quality, domain_id;
    // Appended in version 2; the prefix retains the version 1 NVS layout.
    char ros_namespace[64];
} device_config_t;

esp_err_t device_config_init(void);
const device_config_t *device_config_get(void);
esp_err_t device_config_load(device_config_t *out);
bool device_config_valid(const device_config_t *cfg);
esp_err_t device_config_save(const device_config_t *cfg);
bool device_config_resolved_topic(const device_config_t *cfg, char *out, size_t size);
bool device_mode(const char *mode);
