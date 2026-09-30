"""Stale paths must become pending again without touching healthy cached rows."""
import ctypes
from pathlib import Path
import shlex
import sqlite3
import subprocess
import tempfile
import unittest
from test_native_png import png

REPO = Path(__file__).resolve().parents[2]


class AssetCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="enil-cache-")
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / "cache.so"
        stub = Path(cls.temp.name) / "log.c"
        stub.write_text('void enil_log(const char *tag, const char *fmt, ...) {(void)tag;(void)fmt;}')
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "sqlite3", "zlib"], text=True))
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        str(REPO / "source/shared/enil_asset_cache.c"),
                        str(REPO / "source/shared/enil_png.c"), str(stub), *flags,
                        "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.sqlite3_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        cls.lib.sqlite3_close.argtypes = [ctypes.c_void_p]
        cls.lib.enil_asset_cache_reconcile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    def test_missing_empty_truncated_corrupt_and_zero_dimension_assets_are_requeued(self):
        with tempfile.TemporaryDirectory(dir=self.temp.name) as td:
            root = Path(td)
            assets = root / "purchases"
            assets.mkdir()
            for name, data in {"good": png(), "empty": b"", "short": png()[:-1],
                               "bad": b"not a png", "dimensions": png()}.items():
                (assets / name).write_bytes(data)
            db_path = root / "enil.sqlite"
            with sqlite3.connect(db_path) as db:
                for table in ("stickers_v2", "sticons_v2"):
                    db.execute(f"CREATE TABLE {table} (id TEXT, image_path TEXT, thumb_path TEXT, "
                               "image_width INTEGER, image_height INTEGER, thumb_width INTEGER, thumb_height INTEGER)")
                    for name in ("good", "empty", "short", "bad", "missing", "dimensions", "bad-thumb"):
                        image = "purchases/" + ("good" if name == "bad-thumb" else name)
                        thumb = "purchases/missing" if name == "bad-thumb" else image
                        dim = 0 if name == "dimensions" else 1
                        db.execute(f"INSERT INTO {table} VALUES (?,?,?,?,?,?,?)",
                                   (name, image, thumb, dim, dim, dim, dim))
            handle = ctypes.c_void_p()
            self.assertEqual(self.lib.sqlite3_open(bytes(db_path), ctypes.byref(handle)), 0)
            try:
                self.assertEqual(self.lib.enil_asset_cache_reconcile(handle, bytes(root)), 0)
                self.assertEqual(self.lib.enil_asset_cache_reconcile(handle, bytes(root)), 0)
            finally:
                self.lib.sqlite3_close(handle)
            with sqlite3.connect(db_path) as db:
                for table in ("stickers_v2", "sticons_v2"):
                    self.assertEqual(db.execute(f"SELECT id FROM {table} WHERE image_path IS NOT NULL").fetchall(),
                                     [("good",)])
                    self.assertEqual(db.execute(f"SELECT count(*) FROM {table} WHERE image_path IS NULL "
                                                "AND thumb_path IS NULL AND image_width=0").fetchone(), (6,))
            self.assertEqual((assets / "good").read_bytes(), png())
