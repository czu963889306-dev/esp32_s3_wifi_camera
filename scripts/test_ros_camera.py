#!/usr/bin/env python3
"""Receive and validate real ROS 2 camera messages (requires sourced ROS 2)."""
from __future__ import annotations

import argparse
import time

import rclpy
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage


def validate_image(message: CompressedImage, frame_id: str) -> None:
    if message.format != "jpeg":
        raise ValueError(f"Expected jpeg, got {message.format!r}")
    if message.header.frame_id != frame_id:
        raise ValueError(f"Unexpected frame ID: {message.header.frame_id!r}")
    if message.header.stamp.sec <= 0 or not 0 <= message.header.stamp.nanosec < 1_000_000_000:
        raise ValueError("Missing or invalid synchronized timestamp")
    data = bytes(message.data)
    if not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
        raise ValueError("Incomplete JPEG frame")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topic", default="/camera/image_raw/compressed")
    parser.add_argument("--frame-id", default="camera_optical_frame")
    parser.add_argument("--frames", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--decode", action="store_true", help="Also decode JPEG with OpenCV")
    args = parser.parse_args()
    if args.frames <= 0 or args.timeout <= 0:
        parser.error("--frames and --timeout must be positive")
    if args.decode:
        import cv2
        import numpy as np

    rclpy.init()
    node = rclpy.create_node("test_esp32_ros_camera")
    received = 0
    error = None
    last_stamp = 0

    def on_image(message: CompressedImage) -> None:
        nonlocal received, error, last_stamp
        try:
            validate_image(message, args.frame_id)
            stamp = message.header.stamp.sec * 1_000_000_000 + message.header.stamp.nanosec
            if stamp <= last_stamp:
                raise ValueError("Capture timestamps are not increasing")
            last_stamp = stamp
            dimensions = ""
            if args.decode:
                image = cv2.imdecode(np.frombuffer(bytes(message.data), dtype=np.uint8), cv2.IMREAD_COLOR)
                if image is None:
                    raise ValueError("JPEG decoding failed")
                dimensions = f" {image.shape[1]}x{image.shape[0]}"
            received += 1
            print(f"Frame {received}: {len(message.data)} bytes{dimensions}, stamp={stamp}", flush=True)
        except ValueError as exc:
            error = str(exc)

    node.create_subscription(CompressedImage, args.topic, on_image, qos_profile_sensor_data)
    deadline = time.monotonic() + args.timeout
    try:
        while received < args.frames and error is None and time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.1)
    finally:
        node.destroy_node()
        rclpy.shutdown()
    if error:
        print(f"FAIL: {error}")
        return 1
    if received < args.frames:
        print(f"FAIL: timed out, received {received}/{args.frames} frames")
        return 1
    print(f"PASS: received {received} valid camera frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
