#include "device_config.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
static device_config_t active;
// Accept ordinary ROS path tokens. Private names and substitutions are intentionally excluded.
static bool valid_ros_path(const char *name, bool namespace_path) {
    if (!*name)
        return namespace_path;
    if (namespace_path && name[0] != '/')
        return false;
    const char *p = name + (name[0] == '/');
    if (!*p)
        return namespace_path;
    bool start = true;
    for (; *p; ++p) {
        if (*p == '/') {
            if (start)
                return false;
            start = true;
        } else {
            if (!(isalnum((unsigned char)*p) || *p == '_') || (start && isdigit((unsigned char)*p)))
                return false;
            start = false;
        }
    }
    return !start;
}

bool device_config_resolved_topic(const device_config_t *c, char *out, size_t size) {
    int n;
    if (c->topic[0] == '/')
        n = snprintf(out, size, "%s", c->topic);
    else
        n = snprintf(out, size, "%s/%s", strcmp(c->ros_namespace, "/") ? c->ros_namespace : "",
                     c->topic);
    return n >= 0 && (size_t)n < size;
}
bool device_config_valid(const device_config_t *c) {
    struct in_addr ip;
    if (c->version != 2 || !memchr(c->mode, 0, sizeof(c->mode)) ||
        !memchr(c->agent_ip, 0, sizeof(c->agent_ip)) || !memchr(c->topic, 0, sizeof(c->topic)) ||
        !memchr(c->frame_id, 0, sizeof(c->frame_id)) ||
        !memchr(c->ros_namespace, 0, sizeof(c->ros_namespace)))
        return false;
    if (strcmp(c->mode, "http") && strcmp(c->mode, "microros") && strcmp(c->mode, "rtsp") &&
        strcmp(c->mode, "webrtc"))
        return false;
    if (inet_pton(AF_INET, c->agent_ip, &ip) != 1 || ntohl(ip.s_addr) == 0 ||
        ntohl(ip.s_addr) >= 0xe0000000U)
        return false;
    if (c->agent_port < 1 || c->agent_port > 65535 || c->fps < 1 || c->fps > 30 ||
        c->quality > 63 || c->domain_id > 232)
        return false;
    if (!valid_ros_path(c->topic, false) || !valid_ros_path(c->ros_namespace, true) ||
        !c->frame_id[0])
        return false;
    for (const char *p = c->frame_id; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '/'))
            return false;
    return true;
}
esp_err_t device_config_load(device_config_t *c) {
    *c = (device_config_t){.version = 2,
                           .ros_namespace = "",
                           .mode = "http",
                           .agent_ip = CONFIG_CAMERA_ROS_AGENT_IP,
                           .topic = CONFIG_CAMERA_ROS_TOPIC,
                           .frame_id = CONFIG_CAMERA_ROS_FRAME_ID,
                           .agent_port = CONFIG_CAMERA_ROS_AGENT_PORT,
                           .fps = CONFIG_CAMERA_ROS_FPS,
                           .quality = 12,
                           .domain_id = CONFIG_CAMERA_ROS_DOMAIN_ID};
    nvs_handle_t n;
    esp_err_t e = nvs_open("camera_cfg", NVS_READONLY, &n);
    if (e == ESP_OK) {
        device_config_t saved = {0};
        size_t size = sizeof(saved);
        e = nvs_get_blob(n, "config", &saved, &size);
        nvs_close(n);
        if (e == ESP_OK) {
            if (size == offsetof(device_config_t, ros_namespace) && saved.version == 1) {
                saved.version = 2;
                saved.ros_namespace[0] = '\0';
            } else if (size != sizeof(saved)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (!device_config_valid(&saved))
                return ESP_ERR_INVALID_ARG;
            *c = saved;
            return ESP_OK;
        }
        if (e != ESP_ERR_NVS_NOT_FOUND)
            return e;
    } else if (e != ESP_ERR_NVS_NOT_FOUND)
        return e;
    // Migrate the previous firmware's Agent settings on first save.
    if (nvs_open("ros_camera", NVS_READONLY, &n) == ESP_OK) {
        device_config_t legacy = *c;
        size_t size = sizeof(legacy.agent_ip);
        uint16_t port;
        if (nvs_get_str(n, "agent_ip", legacy.agent_ip, &size) == ESP_OK &&
            nvs_get_u16(n, "agent_port", &port) == ESP_OK) {
            legacy.agent_port = port;
            if (device_config_valid(&legacy))
                *c = legacy;
        }
        nvs_close(n);
    }
    return device_config_valid(c) ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t device_config_save(const device_config_t *c) {
    if (!device_config_valid(c))
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t n;
    esp_err_t e = nvs_open("camera_cfg", NVS_READWRITE, &n);
    if (e != ESP_OK)
        return e;
    e = nvs_set_blob(n, "config", c, sizeof(*c));
    if (e == ESP_OK)
        e = nvs_commit(n);
    nvs_close(n);
    return e;
}
esp_err_t device_config_init(void) { return device_config_load(&active); }
const device_config_t *device_config_get(void) { return &active; }
bool device_mode(const char *mode) { return strcmp(active.mode, mode) == 0; }
