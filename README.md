# ESP32-S3 Wi-Fi 相机

ESP32-S3 相机固件，支持 HTTP MJPEG、RTSP、WebRTC 和 micro-ROS UDP 四种视频模式。出厂默认使用 HTTP MJPEG，升级固件后沿用已保存的模式。

## 快速开始

首次启动时，连接相机创建的 `Maturo_CAM_<MAC>` 热点，打开 `http://192.168.1.88/` 配置 Wi-Fi。相机接入路由器后，在同一局域网打开 `http://<设备IP>/`，选择视频模式并点击「保存视频设置并重启」。可从路由器的客户端列表或 `python3 scripts/test_discovery.py` 获取设备 IP。

| 视频模式 | 观看或订阅方式 | 图像格式 |
| --- | --- | --- |
| HTTP（默认） | 浏览器打开 `http://<设备IP>:81/` | 640×480 MJPEG |
| RTSP | 用 VLC 或 FFplay 通过 TCP 打开 `rtsp://<设备IP>:8554/stream` | 640×480 JPEG |
| WebRTC（仅作传输演示） | 浏览器打开 `http://<设备IP>/webrtc` | H.264 软件编码，160×120，实测约 10 fps |
| micro-ROS UDP | 启动 micro-ROS Agent，订阅 `/camera/image_raw/compressed` 或自定义话题 | 640×480 JPEG，ROS 2 `CompressedImage` |

## micro-ROS 相机话题

| 项目 | 默认值 |
| --- | --- |
| 话题 | `/camera/image_raw/compressed` |
| 类型 | `sensor_msgs/msg/CompressedImage` |
| `format` | `jpeg` |
| `header.frame_id` | `camera_optical_frame` |
| Domain ID | `0` |
| 发布上限 | `25 Hz`（实际帧率取决于 Wi-Fi、JPEG 大小和 Agent 应答） |
| Agent | `192.168.31.69:8888`，可在配网页修改 |
| QoS | Reliable / Keep Last / depth 1 |

JPEG 保留传感器压缩，发送时直接借用相机帧缓冲，不在 MCU 上解码或再次压缩。
Reliable XRCE 流将大图像分片发送；UDP MTU 为 1400，流 history 为 8。
使用支持流式分片的 Humble `rmw_microxrcedds`，帧可以大于 MTU × history；
默认丢弃超过 128 KiB 的帧，并在发布失败、Wi-Fi 断开或 Agent 离线后重建会话。
与 Agent 完成时间同步后才发布图像，时间戳由摄像头采集时间换算得到。
节点名称和 XRCE client key 根据设备 MAC 生成，可让多台设备连接同一 Agent。
多相机场景还需为每台相机配置不同的话题和 frame ID。

当前发布的是压缩图像，不发布未压缩的 `sensor_msgs/msg/Image`。
未提供相机标定数据，因此不发布 `CameraInfo`，也不添加 TF。

## 编译与烧录

使用 ESP-IDF 5.5.3、ESP32-S3，当前 `sdkconfig` 配置 4 MiB Flash 和 PSRAM。
micro-ROS 组件已放在 `components/micro_ros_espidf_component/`，来源版本见该目录的 `UPSTREAM`。
初次编译需要联网拉取 micro-ROS / ROS 2 Humble 源码，并需要 `colcon`、`vcstool` 和 ROS Python 构建依赖。
可在已安装 ROS 2 Humble 的 Ubuntu 22.04 环境构建，ESP-IDF Python 环境同样需要这些依赖。
上游构建说明：https://github.com/micro-ROS/micro_ros_espidf_component/tree/humble

```bash
. /path/to/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`idf.py menuconfig` → `ROS 2 camera` 可设置编译默认值；串口保存的配置优先。
网页保存的 Agent 地址/端口与串口使用同一配置存储，保存后重启生效。
旧版 `ros_camera` NVS 的 Agent 地址会自动迁移；已有配置不会被编译默认值覆盖。
`app-colcon.meta` 控制 micro-ROS 传输与实体数量；修改后会自动重建 micro-ROS 静态库。

## 网页切换与恢复出厂设置

打开配置页 `http://<设备IP>/`（端口 80），在“视频模式”中选择 **HTTP 视频流 (MJPEG)**、**micro-ROS (UDP)**、**RTSP** 或 **WebRTC**，点击“保存视频设置并重启”。选中 micro-ROS (UDP) 时，可编辑 Agent IPv4 地址、UDP 端口和 ROS 2 图像话题。切到其他模式会隐藏这些设置，并保留上次保存的值。选中 RTSP 时，网页显示带当前设备 IP 的 RTSP 播放地址，可复制到 VLC 或 FFplay 使用 TCP 播放。切换模式无需重新填写 Wi-Fi。端口 81 的 `http://<设备IP>:81/` 用于观看 HTTP 视频流。

图像话题支持绝对名（如 `/front_camera/image/compressed`）和相对名（如 `image/compressed`，会加上节点 namespace）。最多 95 个 ASCII 字符，各段用单个 `/` 分隔，每段以英文字母或下划线开头，后续可含英文字母、数字或下划线。网页和固件均校验格式。保存重启后，在电脑上订阅新的话题名。

设备运行时，连续按住 **BOOT（GPIO0）至少 3 秒**，再松开。相机会清除已保存的 Wi-Fi、视频和 ROS 参数并自动重启。连接 `Maturo_CAM_<MAC>` 热点后可访问 `http://192.168.1.88/` 重新配网，或直接打开 `http://192.168.1.88:81/` 观看默认 HTTP 视频流。

## 配网与接入 ROS 2

1. 设备优先连接已保存的 Wi-Fi；未配置或连接失败时创建 `Maturo_CAM_<MAC>` 无密码热点。
2. 连接热点，打开 `http://192.168.1.88/`。
3. 填写 Wi-Fi 名称和密码，点击“保存 Wi-Fi 并重启”。
4. 电脑连接到与相机互通的网络，打开 `http://<设备IP>/`，选择 micro-ROS (UDP)，填写电脑的局域网 IPv4 地址和 Agent 端口，保存视频设置并重启。电脑需允许接收该 UDP 端口（默认 8888）。
5. 在电脑运行 Humble Agent：

```bash
source /opt/ros/humble/setup.bash
export ROS_DOMAIN_ID=0
ros2 run micro_ros_agent micro_ros_agent udp4 --port 8888
```

如果未安装 Agent，可在 Linux 使用 Docker：

```bash
docker run --rm -it --net=host -e ROS_DOMAIN_ID=0 \
  microros/micro-ros-agent:humble udp4 --port 8888
```

另开终端验证（同样 source ROS 并设置对应的 `ROS_DOMAIN_ID`）：

```bash
ros2 topic info /camera/image_raw/compressed -v
ros2 topic hz /camera/image_raw/compressed
python3 scripts/test_ros_camera.py --frames 10 --timeout 20
# 可选：安装 python3-opencv 后验证 JPEG 可解码
python3 scripts/test_ros_camera.py --decode
```

图像查看器需选择 compressed 传输，例如安装 `rqt_image_view` 和 `compressed_image_transport` 后：

```bash
ros2 run rqt_image_view rqt_image_view
```

## 串口参数与模式切换

相机的串口配置入口是板载 **USB Serial/JTAG 调试口**（Linux 通常为 `/dev/ttyACM0`）。连接后按行发送 `camera` 或 `wifi` 命令，支持 CR、LF 和 CRLF。启动完成后发送 `camera get`，可查看 `ACTIVE`、`PENDING` 和 `SAVED` 三组配置。GPIO43/44 的 UART0 配置入口已移除。

字符串包含空格时加双引号，含反斜杠时写成两个反斜杠。

```text
camera get
camera set mode microros
camera set fps 25
camera set quality 12
camera save
camera reboot
```

`camera get` 输出 `ACTIVE`（运行配置）、`PENDING`（串口待保存配置）和 `SAVED`（下次启动配置）。
`camera set` 暂存修改，`camera save` 校验并原子保存整个视频配置，`camera reboot` 应用；
重启不会自动保存。`camera reload` 丢弃串口暂存修改，重新读取已保存配置。
如果网页修改过 Agent 地址，在串口继续编辑前执行 `camera reload`，避免旧暂存值覆盖网页修改。

| 参数 | 有效值 / 默认值 |
| --- | --- |
| `mode` | `http`（默认，MJPEG）、`microros`（UDP）、`rtsp`、`webrtc` |
| `fps` | 1–30，默认 25，是发布上限 |
| `quality` | 0–63，默认 12；数值越小 JPEG 质量越高，带宽越大 |
| `agent_ip` | IPv4，默认 `192.168.31.69` |
| `agent_port` | 1–65535，默认 8888 |
| `domain_id` | 0–232，默认 0 |
| `topic` | 相对或绝对 ROS 话题名，默认 `/camera/image_raw/compressed` |
| `namespace` | 默认空字符串；非空值以 `/` 开头；也可用 `ros_namespace` 参数名 |
| `frame_id` | 字母、数字、下划线或 `/`，默认 `camera_optical_frame` |

读取网络状态与设置 Wi-Fi：

```text
wifi get
wifi set "Your WiFi" "YourPassword"
camera reboot
```

`wifi set` 成功后立即保存 Wi-Fi，重启应用；不会读取或打印保存的密码。
输入的密码仍会被串口终端回显，请妥善管理终端日志。

### WebRTC 模式（“保密画质”）

此模式仅用于演示 ESP32-S3 可以通过 WebRTC 向浏览器传输视频。ESP32-S3 没有 H.264 硬件编码器，本固件依靠 CPU 软件编码。在这块板上，320×240 实测约 3 fps，降到 160×120 才达到约 10 fps，画面细节很少。

在配置页选择 **WebRTC**，保存并重启，点击“观看 WebRTC 视频”或直接打开 `http://<设备IP>/webrtc`。也可以在 USB 调试口依次输入：

```text
camera set mode webrtc
camera save
camera reboot
```

观看时，浏览器和相机需在同一局域网，浏览器需支持 WebRTC H.264。一次支持一位观看者。

### RTSP 模式

```text
camera set mode rtsp
camera save
camera reboot
```

播放地址 `rtsp://<设备IP>:8554/stream`，使用 **JPEG/RTP over TCP、单客户端**。
不支持 H.264/H.265、音频、UDP RTP、多客户端或身份认证。
支持 OPTIONS、DESCRIBE、SETUP、PLAY、PAUSE、GET_PARAMETER、TEARDOWN；
客户端需使用 RTSP keepalive 或 RTCP，在 60 秒内保持会话活跃。

```bash
ffplay -rtsp_transport tcp rtsp://<设备IP>:8554/stream
# VLC 启用 RTP over RTSP (TCP)
vlc --rtsp-tcp rtsp://<设备IP>:8554/stream
```

### HTTP 模式

```text
camera set mode http
camera save
camera reboot
```

浏览器访问 `http://<设备IP>:81/` 获取 MJPEG，单客户端。
HTTP 模式也使用已保存的 fps 和 quality。

### micro-ROS 模式

```text
camera set mode microros
camera set agent_ip 192.168.31.69
camera set agent_port 8888
camera set domain_id 0
camera save
camera reboot
```

上位机运行 Agent，按前文 ROS 2 接入步骤订阅话题。

### 自定义 Agent 端口、话题和 namespace

```text
camera set agent_port 9999
camera set namespace /robot1
camera set topic camera/image_raw/compressed
camera get
camera save
camera reboot
```

此配置创建 `/robot1` 下的相机节点，发布 `/robot1/camera/image_raw/compressed`。
电脑上 Agent 使用相同端口：`ros2 run micro_ros_agent micro_ros_agent udp4 --port 9999`。
`camera get` 同时显示配置话题 `topic` 和最终完整话题 `resolved_topic`。

相对话题（开头没有 `/`）会附加 namespace；绝对话题（以 `/` 开头）不附加 namespace。
因此只修改 namespace、保留默认绝对话题时，话题仍为 `/camera/image_raw/compressed`，节点 namespace 会改变。
执行 `camera set namespace ""` 清空 namespace（默认值）；`/` 也表示根 namespace。namespace 不会自动改变 `frame_id` 或 Domain ID。
旧版 NVS 视频配置会保留所有原参数，并补上默认空 namespace，无需清空配置。

## 配网与设备发现

- 四种模式均保留 80 端口配网页、UDP 发现和 I2C 功能。
- `/handshake`、`/api/handshake` 返回 `video_mode`、`stream_url` 和 ROS 配置。
  HTTP 模式返回 HTTP URL，RTSP 模式返回 RTSP URL，WebRTC 模式返回浏览器播放页地址，micro-ROS 模式的 URL 为空。
- UDP `IMAGE_TRANSPORT` 为 `microros`、`http`、`rtsp` 或 `webrtc`；只有 HTTP 模式的 `STREAM_PORT` 为 81，其余为 0；RTSP 另报 `RTSP_PORT:8554`。
- `python3 scripts/test_discovery.py` 检查发现与握手；`--stream` 仅测试 HTTP 模式。
- 旧 `CAMERA_HTTP_PREVIEW` 编译选项已移除，使用网页或串口选择视频模式。

## 验证

```bash
python3 -m unittest discover -s tests -v
```

RTSP 主机测试需要系统 C 编译器、FFmpeg、Python OpenCV 和 NumPy，使用本地 18554 端口。
测试直接编译固件 RTSP 代码，用模拟相机帧验证请求拆包、UDP 拒绝及 FFmpeg 解码，并启用 AddressSanitizer/UBSan。
主机测试不代替摄像头硬件和 Wi-Fi 实机测试；固件编译完成后仍需验证串口保存/重启、四模式切换及实际帧率。

## 工程结构

| 位置 | 职责 |
| --- | --- |
| `main/app_main.c`、`main/app_runtime.c` | ESP-IDF 入口、启动顺序及视频服务选择 |
| `main/config/` | `board_config.h` 集中管理摄像头、BOOT、LED 和 I2C 引脚。 `device_config.c` 管理 NVS 参数与 ROS 名称校验 |
| `main/platform/` | 摄像头初始化与采集自检、BOOT 长按与指示灯 |
| `main/network/` | Wi-Fi、UDP 设备发现、供网页和 I2C 查询的播放地址 |
| `main/tasks/io/` | 板载 USB 调试口配置命令、I2C 从机通信 |
| `main/tasks/network/provision/` | 配网页模板、表单和握手接口、配网热点 DNS |
| `main/tasks/network/http/`、`rtsp/`、`webrtc/` | MJPEG 服务、RTSP/RTP 打包、WebRTC 编码和播放页 |
| `main/tasks/network/microros/` | ROS 2 图像发布、时间同步与 Agent 重连 |

- `components/micro_ros_espidf_component/`：Humble micro-ROS 构建组件及许可证
- `managed_components/`：ESP-IDF 管理的摄像头、JPEG、H.264 和 WebRTC 组件
- `scripts/test_ros_camera.py`：ROS 2 图像接收验证工具
- `partitions.csv` / `sdkconfig`：分区及 ESP-IDF 配置

