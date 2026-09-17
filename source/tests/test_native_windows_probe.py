"""Real C QR flow and durable session against an Apache Thrift loopback server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

from test_windows_login_probe import Flow, reply, TType

REPO = Path(__file__).resolve().parents[2]


class NativeWindowsProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="enil-native-probe-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = str(Path(cls.build.name) / "probe-test")
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libcjson", "libcurl", "openssl"], text=True))
        shared = REPO / "source/shared"
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-ffunction-sections", "-fdata-sections", "-I" + str(shared),
                        str(REPO / "source/tests/native_windows_probe.c"),
                        *[str(shared / name) for name in ["enil_windows_probe.c", "enil_session.c",
                          "enil_identity.c", "enil_http.c", "enil_b64.c"]],
                        "-Wl,--gc-sections", "-Wl,--wrap=curl_easy_perform", "-pthread",
                        *flags, "-o", cls.binary], check=True, timeout=60)

    def run_probe(self, mode):
        flow, failures = Flow(), []
        flow.model_name = "DESKTOPWIN"

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                try:
                    body = self.rfile.read(int(self.headers["Content-Length"]))
                    status, payload = flow.send("https://legy.line-apps.com" + self.path,
                                                body, dict(self.headers), 11)
                    if mode == "token-error" and flow.calls[-1] == "qrCodeLoginV2ForSecure":
                        payload = reply(flow.calls[-1], [(1, TType.STRUCT, [(1, TType.I32, 5)])])
                except Exception as error:
                    failures.append(error)
                    status, payload = 500, b"fixture rejected request"
                self.send_response(status)
                self.send_header("Content-Type", "application/x-thrift")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix="enil-native-probe-test-") as temp:
                result = subprocess.run([self.binary, str(Path(temp) / "session"),
                                        f"http://127.0.0.1:{server.server_port}", mode],
                                        capture_output=True, text=True, timeout=20)
                self.assertFalse(failures, failures)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(flow.calls.count("createSession"), 1)
                self.assertEqual(flow.calls.count("qrCodeLoginV2ForSecure"), 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_durable_credentials_and_reopen_without_network(self):
        self.run_probe("success")

    def test_unwrap_failure_keeps_tokens_and_reopen_avoids_login(self):
        self.run_probe("unwrap-fails")

    def test_uncertain_token_request_is_not_repeated(self):
        self.run_probe("token-error")


if __name__ == "__main__":
    unittest.main()
