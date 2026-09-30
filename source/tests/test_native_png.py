"""Check cached PNG integrity, including interrupted and corrupt downloads."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[2]


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png():
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"\0\xff\0\0\xff")) + chunk(b"IEND", b""))


class PNGTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="enil-png-")
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / "png.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        str(REPO / "source/shared/enil_png.c"), "-lz", "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.enil_png_info.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int),
                                         ctypes.POINTER(ctypes.c_int)]

    def check(self, data, valid):
        path = Path(self.temp.name) / "image.png"
        path.write_bytes(data)
        w, h = ctypes.c_int(-1), ctypes.c_int(-1)
        result = self.lib.enil_png_info(bytes(path), ctypes.byref(w), ctypes.byref(h))
        self.assertEqual(result, valid)
        self.assertEqual((w.value, h.value), (1, 1) if valid else (0, 0))

    def test_static_and_animated_png(self):
        self.check(png(), True)
        # APNG adds animation chunks without changing the base PNG image.
        self.check(png()[:33] + chunk(b"acTL", struct.pack(">II", 1, 0)) +
                   chunk(b"fcTL", struct.pack(">IIIIIHHBB", 0, 1, 1, 0, 0, 1, 10, 0, 0)) +
                   png()[33:], True)

    def test_every_truncation_is_rejected(self):
        data = png()
        for n in range(len(data)):
            with self.subTest(size=n):
                self.check(data[:n], False)

    def test_corrupt_bytes_bad_signature_and_trailing_garbage(self):
        data = png()
        for n in range(len(data)):
            with self.subTest(offset=n):
                damaged = bytearray(data)
                damaged[n] ^= 1
                self.check(damaged, False)
        self.check(data + b"garbage", False)
        self.check(b"<html>HTTP 200 error page</html>", False)
