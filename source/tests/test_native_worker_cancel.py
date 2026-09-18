"""Stop/reconnect must cancel real Worker sockets at either LEGY stage."""
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

from thrift.Thrift import TType
from test_windows_login_probe import reply

REPO = Path(__file__).resolve().parents[2]


class NativeWorkerCancelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="enil-worker-cancel-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = str(Path(cls.temp.name) / "cancel")
        shared = REPO / "source/shared"
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libcjson", "libcurl", "openssl"], text=True))
        sources = ["enil_native.c", "enil_thrift.c", "enil_identity.c", "enil_line.c",
                   "enil_http.c", "enil_b64.c", "enil_session.c", "enil_api_json.c",
                   "enil_sse.c", "enil_worker.c", "enil_health.c"]
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
            "-ffunction-sections", "-fdata-sections", "-I" + str(shared),
            str(REPO / "source/tests/native_worker_cancel.c"),
            *[str(shared / name) for name in sources], "-Wl,--gc-sections",
            "-Wl,--wrap=curl_easy_perform", "-pthread", *flags, "-o", cls.binary,
        ], check=True)

    def check_stage(self, stage):
        requests = []
        blocked = []
        condition = threading.Condition()
        release = threading.Event()
        # A valid full-sync reply. Decode-stage cancellation
        # must prevent it from reaching the event callback or session cursors.
        wire = reply("sync", [(0, TType.STRUCT, [
            (2, TType.STRUCT, [(2, TType.I64, 20)])
        ])])

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                raw = self.rfile.read(int(self.headers["Content-Length"]))
                requests.append(self.path)
                if self.path == "/transport/legy/" + stage:
                    with condition:
                        blocked.append(self.path)
                        condition.notify_all()
                    release.wait(30)
                    self.close_connection = True
                    return
                if self.path == "/transport/legy/encode":
                    body = json.dumps({"body": json.loads(raw)["body"],
                                       "key": "synthetic", "xLcs": "0008synthetic"}).encode()
                elif self.path == "/enc":
                    body = wire
                else:
                    body = json.dumps({"body": base64.b64encode(wire).decode(), "status": 200}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        process = subprocess.Popen([
            self.binary, f"http://127.0.0.1:{server.server_port}",
            str(Path(self.temp.name) / (stage + ".json")),
        ], stdin=subprocess.PIPE)
        try:
            # Exceed the poller's five-failure limit: user cancellation must not
            # consume that budget or poison the real Worker/LINE health gates.
            for expected in range(1, 8):
                with condition:
                    self.assertTrue(condition.wait_for(lambda: len(blocked) >= expected, timeout=4),
                                    f"{stage} request {expected} did not start")
                process.stdin.write(b"k" if expected < 7 else b"q")
                process.stdin.flush()
            self.assertEqual(process.wait(timeout=4), 0)
            self.assertEqual(requests.count("/transport/legy/encode"), 7)
            self.assertEqual(requests.count("/enc"), 7 if stage == "decode" else 0)
            self.assertEqual(requests.count("/transport/legy/decode"), 7 if stage == "decode" else 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
            release.set()
            server.shutdown()
            server.server_close()
            thread.join()

    def test_cancel_worker_encode_on_reconnect_and_stop(self):
        self.check_stage("encode")

    def test_cancel_worker_decode_on_reconnect_and_stop(self):
        self.check_stage("decode")
