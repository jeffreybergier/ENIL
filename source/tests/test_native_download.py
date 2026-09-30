"""Real loopback transfers exercise cache publication and interrupted writes."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

REPO = Path(__file__).resolve().parents[2]


class DownloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="enil-download-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "download"
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libcurl", "libcjson"], text=True))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-DENIL_HTTP_TIMEOUT_SECONDS=2L", "-DENIL_HTTP_STALL_SECONDS=1L",
                        "-I" + str(REPO / "source/shared"),
                        str(REPO / "source/tests/native_download.c"),
                        str(REPO / "source/shared/enil_http.c"),
                        "-pthread", *flags, "-o", str(cls.binary)], check=True)

    def transfer(self, mode, existing=None, kill=False, cancel=False):
        ready, release = threading.Event(), threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.send_response(404 if mode == "404" else 200)
                self.send_header("Content-Length", "100" if mode != "ok" else "4")
                self.end_headers()
                self.wfile.write(b"data")
                self.wfile.flush()
                ready.set()
                if mode == "stall":
                    release.wait(10)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(dir=self.temp.name) as td:
                dest = Path(td) / "sticker.png"
                if existing is not None:
                    dest.write_bytes(existing)
                proc = subprocess.Popen([str(self.binary),
                    f"http://127.0.0.1:{server.server_port}/image", str(dest),
                    *(["cancel"] if cancel else [])])
                try:
                    self.assertTrue(ready.wait(5))
                    if kill:
                        proc.kill()
                    code = proc.wait(timeout=5)
                    self.assertEqual(code == 0, mode == "ok")
                    if mode == "ok":
                        self.assertEqual(dest.read_bytes(), b"data")
                    elif existing is not None:
                        self.assertEqual(dest.read_bytes(), existing)
                    else:
                        self.assertFalse(dest.exists())
                    if not kill:
                        self.assertEqual(list(Path(td).glob("*.download-*")), [])
                finally:
                    if proc.poll() is None:
                        proc.kill()
                    proc.wait()
        finally:
            release.set()
            server.shutdown()
            server.server_close()
            thread.join()

    def test_complete_response_replaces_destination(self):
        self.transfer("ok", b"previous")

    def test_http_and_truncated_responses_preserve_destination(self):
        for mode in ("404", "short"):
            for existing in (None, b"previous"):
                with self.subTest(mode=mode, existing=existing):
                    self.transfer(mode, existing)

    def test_process_death_never_publishes_partial_response(self):
        for existing in (None, b"previous"):
            with self.subTest(existing=existing):
                self.transfer("stall", existing, kill=True)

    def test_stalled_download_times_out_without_poisoning_cache(self):
        self.transfer("stall")

    def test_cancelled_download_preserves_cached_file(self):
        self.transfer("stall", b"previous", cancel=True)
