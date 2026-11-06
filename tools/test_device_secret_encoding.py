import hashlib
import hmac
import unittest

from profile_broadcast_protocol import build_common
from token_broadcast_gui import build_frame


class DeviceSecretEncodingTest(unittest.TestCase):
    def builders(self, key):
        return (
            lambda: build_common(txn_id=1, device_secret=key, token=1),
            lambda: build_frame("abcdef123456", 1, 1, 1, key),
        )

    def test_utf8_64_bytes_sign_like_firmware(self):
        key = "密" * 21 + "&"
        self.assertEqual(len(key.encode("utf-8")), 64)
        for build in self.builders(key):
            frame = build()
            self.assertEqual(frame[-8:], hmac.new(
                key.encode("utf-8"), frame[:-8], hashlib.sha256).digest()[:8])

    def test_rejects_oversized_or_embedded_nul(self):
        for key in ("密" * 22, "a" * 65, "a\0b", ""):
            for build in self.builders(key):
                with self.assertRaises(ValueError):
                    build()
