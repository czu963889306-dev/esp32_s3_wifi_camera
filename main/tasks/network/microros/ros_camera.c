#include "ros_camera.h"
#include "config/device_config.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rcl/rcl.h"
#include "rcl/error_handling.h"
#include "rmw_microros/rmw_microros.h"
#include "rosidl_runtime_c/string_functions.h"
#include "sensor_msgs/msg/compressed_image.h"
#include "uxr/client/config.h"
#include "network/wifi_manager.h"

#if !defined(RMW_UXRCE_TRANSPORT_UDP) || !defined(UCLIENT_TWEAK_XRCE_WRITE_LIMIT)
#error "ROS camera requires UDP micro-ROS with support for writes larger than 64 KiB"
#endif

static const char *TAG = "ROS_CAMERA";
static TaskHandle_t s_task;

static void cleanup_result(rcl_ret_t result)
{
    if (result != RCL_RET_OK) {
        rcl_reset_error();
    }
}

static void ros_camera_task(void *arg)
{
    (void)arg;
    const device_config_t *config = device_config_get();
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char node_name[48];
    snprintf(node_name, sizeof(node_name), "esp32_camera_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char port[6];
    snprintf(port, sizeof(port), "%u", (unsigned)config->agent_port);
    // Unique XRCE session key when several cameras share one Agent.
    uint32_t client_key = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                          ((uint32_t)mac[4] << 8) | mac[5];
    if (client_key == 0) {
        client_key = 1;
    }
    ESP_LOGI(TAG, "Waiting for Wi-Fi and Agent %s:%s", config->agent_ip, port);

    for (;;) {
        if (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        rcl_allocator_t allocator = rcl_get_default_allocator();
        rcl_init_options_t init = rcl_get_zero_initialized_init_options();
        rcl_context_t context = rcl_get_zero_initialized_context();
        rcl_node_t node = rcl_get_zero_initialized_node();
        rcl_publisher_t publisher = rcl_get_zero_initialized_publisher();
        sensor_msgs__msg__CompressedImage message = {0};
        bool init_ready = false, context_ready = false;
        bool node_ready = false, publisher_ready = false;

#define CHECK_RCL(call) do { \
    rcl_ret_t rc = (call); \
    if (rc != RCL_RET_OK) { \
        ESP_LOGW(TAG, "%s failed: %d", #call, (int)rc); \
        rcl_reset_error(); \
        goto cleanup; \
    } \
} while (0)

        CHECK_RCL(rcl_init_options_init(&init, allocator));
        init_ready = true;
        CHECK_RCL(rcl_init_options_set_domain_id(&init, config->domain_id));
        rmw_init_options_t *rmw_options = rcl_init_options_get_rmw_init_options(&init);
        if (rmw_uros_options_set_udp_address(config->agent_ip, port, rmw_options) != RMW_RET_OK ||
            rmw_uros_options_set_client_key(client_key, rmw_options) != RMW_RET_OK ||
            rmw_uros_ping_agent_options(200, 1, rmw_options) != RMW_RET_OK) {
            goto cleanup;
        }
        CHECK_RCL(rcl_init(0, NULL, &init, &context));
        context_ready = true;

        rcl_node_options_t node_options = rcl_node_get_default_options();
        node_options.enable_rosout = false;
        rcl_ret_t node_result = rcl_node_init(&node, node_name, config->ros_namespace, &context, &node_options);
        cleanup_result(rcl_node_options_fini(&node_options));
        CHECK_RCL(node_result);
        node_ready = true;

        // Best-effort XRCE cannot fragment a JPEG larger than the transport MTU.
        // Reliable publication uses rmw_microxrcedds' streamed fragmentation.
        rcl_publisher_options_t pub_options = rcl_publisher_get_default_options();
        pub_options.qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        pub_options.qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
        pub_options.qos.depth = 1;
        CHECK_RCL(rcl_publisher_init(&publisher, &node,
                  ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, CompressedImage),
                  config->topic, &pub_options));
        publisher_ready = true;
        if (rmw_uros_set_publisher_session_timeout(
                rcl_publisher_get_rmw_handle(&publisher), 200) != RMW_RET_OK) {
            goto cleanup;
        }
        if (!rosidl_runtime_c__String__assign(&message.header.frame_id, config->frame_id) ||
            !rosidl_runtime_c__String__assign(&message.format, "jpeg")) {
            ESP_LOGE(TAG, "Failed to allocate image metadata");
            goto cleanup;
        }

        char resolved_topic[164];
        device_config_resolved_topic(config, resolved_topic, sizeof(resolved_topic));
        ESP_LOGI(TAG, "Agent %s:%s, domain=%lu, topic=%s, max=%lu Hz",
                 config->agent_ip, port, config->domain_id,
                 resolved_topic, config->fps);
        int64_t next_sync_us = 0;
        int64_t next_ping_us = 0;
        int64_t next_frame_us = 0;
        unsigned published = 0;
        int64_t next_log_us = esp_timer_get_time() + 5000000;
        while (wifi_manager_is_connected()) {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= next_ping_us) {
                if (rmw_uros_ping_agent(200, 1) != RMW_RET_OK) {
                    ESP_LOGW(TAG, "Agent unavailable; reconnecting");
                    break;
                }
                next_ping_us = now_us + 2000000;
            }
            if (now_us >= next_sync_us) {
                bool synced = rmw_uros_sync_session(200) == RMW_RET_OK;
                next_sync_us = now_us + (synced ? 30000000 : 1000000);
            }
            // Never publish boot-relative times as ROS epoch timestamps.
            if (!rmw_uros_epoch_synchronized() || now_us < next_frame_us) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            next_frame_us = now_us + 1000000 / config->fps;
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                ESP_LOGW(TAG, "Camera capture failed");
                continue;
            }
            if (fb->format != PIXFORMAT_JPEG || fb->len < 2 ||
                fb->buf[0] != 0xff || fb->buf[1] != 0xd8 ||
                fb->len > CONFIG_CAMERA_ROS_MAX_JPEG_BYTES) {
                ESP_LOGW(TAG, "Dropping invalid/oversized JPEG (%u bytes)", (unsigned)fb->len);
                esp_camera_fb_return(fb);
                continue;
            }

            // Camera timestamps use esp_timer_get_time(), so translate capture
            // time into Agent epoch time rather than stamping at transmit time.
            int64_t capture_us = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
            int64_t stamp_ns = rmw_uros_epoch_nanos() - (esp_timer_get_time() - capture_us) * 1000;
            message.header.stamp.sec = (int32_t)(stamp_ns / 1000000000LL);
            message.header.stamp.nanosec = (uint32_t)(stamp_ns % 1000000000LL);
            // Borrow the camera buffer until synchronous serialization finishes.
            message.data.data = fb->buf;
            message.data.size = fb->len;
            message.data.capacity = fb->len;
            rcl_ret_t result = rcl_publish(&publisher, &message, NULL);
            message.data.data = NULL;
            message.data.size = message.data.capacity = 0;
            esp_camera_fb_return(fb);
            if (result != RCL_RET_OK) {
                ESP_LOGW(TAG, "Image publish failed (%d); recreating session", (int)result);
                rcl_reset_error();
                break;
            }
            published++;
            if (esp_timer_get_time() >= next_log_us) {
                ESP_LOGI(TAG, "Published %u JPEG frames", published);
                next_log_us = esp_timer_get_time() + 5000000;
            }
            vTaskDelay(1);
        }

cleanup:
        // The camera owns data.data; only free strings owned by this task.
        rosidl_runtime_c__String__fini(&message.header.frame_id);
        rosidl_runtime_c__String__fini(&message.format);
        if (context_ready) {
            (void)rmw_uros_set_context_entity_destroy_session_timeout(
                rcl_context_get_rmw_context(&context), 0);
        }
        if (publisher_ready) {
            cleanup_result(rcl_publisher_fini(&publisher, &node));
        }
        if (node_ready) {
            cleanup_result(rcl_node_fini(&node));
        }
        if (context_ready) {
            cleanup_result(rcl_shutdown(&context));
            cleanup_result(rcl_context_fini(&context));
        }
        if (init_ready) {
            cleanup_result(rcl_init_options_fini(&init));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#undef CHECK_RCL
}

esp_err_t ros_camera_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    return xTaskCreate(ros_camera_task, "ros_camera", 12288, NULL, 4, &s_task) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
