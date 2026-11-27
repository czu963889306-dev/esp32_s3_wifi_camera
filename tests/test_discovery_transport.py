import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from test_discovery import Device, apply_statustext  # noqa: E402


class DiscoveryTransportTests(unittest.TestCase):
    def test_disabled_preview_stays_disabled_after_status_updates(self):
        payload = {}
        for text in ("STREAM_PORT:0", "DEVICE_IP:192.168.1.20", "HTTP_PORT:80",
                     "IMAGE_TRANSPORT:micro_ros", "DEVICE_NAME:camera"):
            apply_statustext(payload, text, "192.168.1.20")
        device = Device(("192.168.1.20", 33333), payload)
        self.assertEqual(device.stream_url, "")
        self.assertEqual(payload["stream_url"], "")
        self.assertEqual(payload["image_transport"], "micro_ros")
        self.assertEqual(device.handshake_url, "http://192.168.1.20/handshake")

    def test_legacy_camera_keeps_preview(self):
        payload = {}
        apply_statustext(payload, "STREAM_PORT:81", "192.168.1.20")
        self.assertEqual(Device(("192.168.1.20", 33333), payload).stream_url,
                         "http://192.168.1.20:81/")

    def test_disabling_previously_advertised_preview_removes_url(self):
        payload = {}
        apply_statustext(payload, "STREAM_PORT:81", "192.168.1.20")
        apply_statustext(payload, "STREAM_PORT:0", "192.168.1.20")
        self.assertEqual(Device(("192.168.1.20", 33333), payload).stream_url, "")


if __name__ == "__main__":
    unittest.main()
