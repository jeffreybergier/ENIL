"""JPEG completeness checks used alongside legacy ImageIO decoding."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


def segment(marker, data):
    return bytes([255, marker]) + (len(data) + 2).to_bytes(2, "big") + data


def jpeg():
    # Marker fixture, not a decodable image. ImageIO is tested separately.
    return (b"\xff\xd8" + segment(0xe1, b"embedded \xff\xd9 marker") +
            segment(0xda, b"scan header") + b"entropy\xff\x00\xff\xd0data" +
            b"\xff\xd9")


class JPEGTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="enil-jpeg-")
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / "jpeg.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        str(REPO / "source/shared/enil_jpeg.c"),
                        "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.enil_jpeg_complete.argtypes = [ctypes.c_char_p]

    def check(self, data, expected):
        path = Path(self.temp.name) / "image.jpg"
        path.write_bytes(data)
        self.assertEqual(self.lib.enil_jpeg_complete(bytes(path)), expected)

    def test_scan_and_progressive_scans(self):
        self.check(jpeg(), 1)
        self.check(jpeg()[:-2] + segment(0xc4, b"table") +
                   segment(0xda, b"second scan") + b"more\xff\xd9", 1)

    def test_every_truncation_including_embedded_eoi_is_rejected(self):
        data = jpeg()
        for n in range(len(data)):
            with self.subTest(size=n):
                self.check(data[:n], 0)

    def test_bad_markers_and_lengths(self):
        for data in [b"", b"<html>error</html>", b"\xff\xd8\xff\xd9",
                     b"\xff\xd8\xff\xe1\x00\x01",
                     b"\xff\xd8\xff\xe1\xff\xff\xff\xd9",
                     b"\xff\xd8\xff\xd0", b"\xff\xd8garbage"]:
            with self.subTest(data=data):
                self.check(data, 0)
