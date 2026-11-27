set(APP_SRCS
    "app_main.c"
    "app_runtime.c"
    "config/device_config.c"
    "platform/board_control.c"
    "platform/camera_init.c"
    "network/wifi_manager.c"
    "network/udp_discovery.c"
    "network/stream_url.c"
    "tasks/io/i2c_slave_comm.c"
    "tasks/io/serial_config.c"
    "tasks/network/provision/captive_dns.c"
    "tasks/network/provision/web_config.c"
    "tasks/network/http/web_mjpeg.c"
    "tasks/network/microros/ros_camera.c"
    "tasks/network/rtsp/rtsp_jpeg.c"
    "tasks/network/rtsp/rtsp_server.c"
    "tasks/network/webrtc/webrtc_camera.c"
)

idf_component_register(
    SRCS ${APP_SRCS}
    EMBED_TXTFILES "tasks/network/webrtc/webrtc.html"
    INCLUDE_DIRS "."
    REQUIRES
        console
        esp_peer
        esp_h264
        esp32-camera
        micro_ros_espidf_component
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        nvs_flash
        esp_psram
        esp_driver_usb_serial_jtag
        esp_driver_i2c
        lwip
)
