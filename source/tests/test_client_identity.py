"""Compile native code and capture synthetic QR/API/media/SSE traffic on loopback.

Requires a host C compiler and pkg-config packages libcjson, libcurl, openssl.
Downloaded extension material and real account fixtures are never used.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

REPO = Path(__file__).resolve().parents[2]
CHROME_APP = "CHROMEOS\t3.7.2\tChrome_OS"
WINDOWS_APP = "DESKTOPWIN\t9.7.0.3556\tWINDOWS\t10.0.0-NT-x64"
CHROME_UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
             "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36")
WINDOWS_UA = "Line/9.7.0.3556"


class ClientIdentityTests(unittest.TestCase):
    def test_session_lifecycle_and_wire_identity(self):
        self.check_session_lifecycle_and_wire_identity("default", "en", "en-US", "en_US")

    def test_japanese_gateway_headers_catalog_and_events(self):
        self.check_session_lifecycle_and_wire_identity("ja", "ja", "ja-JP", "ja_JP")

    def check_session_lifecycle_and_wire_identity(self, selected, language, accept, lal):
        records = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.handle_request()

            def do_POST(self):
                self.handle_request()

            def handle_request(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                headers = {k.lower(): v for k, v in self.headers.items()}
                records.append((self.path, headers, body))
                app = headers.get("x-line-application", "")
                profile = "desktopwin" if app.startswith("DESKTOPWIN") else "chrome"
                data = {}
                content_type = "application/json"
                extra = {}
                if self.path.endswith("/createSession"):
                    data = {"authSessionId": "synthetic-session"}
                elif self.path.endswith("/createQrCode"):
                    data = {"callbackUrl": "line://synthetic", "longPollingIntervalSec": 1,
                            "longPollingMaxCount": 1}
                elif self.path.endswith("/qrCodeLoginV2"):
                    data = {"certificate": "synthetic-certificate",
                            "tokenV3IssueResult": {"accessToken": profile,
                                                   "refreshToken": "synthetic-refresh"},
                            "metaData": {"keyId": "1", "publicKey": "synthetic",
                                         "encryptedKeyChain": "synthetic"}}
                elif self.path.endswith("/getProfile"):
                    data = {"mid": "synthetic-mid", "displayName": "Synthetic", "regionCode": "JP"}
                elif self.path.endswith("/getOwnedProductSummaries"):
                    data = {"productList": []}
                elif self.path.endswith("/tokenRefresh"):
                    data = {"tokenV3IssueResult": {"accessToken": profile + "-refreshed",
                            "refreshToken": "synthetic-refreshed", "durationUntilRefreshInSec": 3600,
                            "tokenIssueTimeEpochSec": 1}}
                payload = json.dumps({"message": "OK", "data": data}).encode()
                if self.path.startswith("/r/talk/"):
                    extra["x-obs-oid"] = "synthetic-oid"
                    content_type = "application/octet-stream"
                    payload = b"image"
                elif self.path.startswith("/api/operation/receive"):
                    content_type = "text/event-stream"
                    payload = b'event: ping\ndata: {}\n\n'
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(payload)))
                for name, value in extra.items():
                    self.send_header(name, value)
                self.end_headers()
                self.wfile.write(payload)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix="enil-identity-tests-") as temp:
                flags = shlex.split(subprocess.check_output(
                    ["pkg-config", "--cflags", "--libs", "libcjson", "libcurl", "openssl", "sqlite3"], text=True))
                shared = REPO / "source/shared"
                sources = ["enil_identity.c", "enil_session.c", "enil_api_json.c", "enil_line.c", "enil_chrome_gateway.c",
                           "enil_http.c", "enil_talkserv.c", "enil_db.c", "enil_api_call.c", "enil_qrlogin.c",
                           "enil_native.c", "enil_thrift.c", "enil_native_login.c", "enil_obs.c", "enil_b64.c", "enil_crypto.c", "enil_sse.c", "enil_native_poll.c"]
                binary = str(Path(temp) / "identity-test")
                subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra",
                                "-Wno-deprecated-declarations", "-ffunction-sections", "-fdata-sections",
                                "-I" + str(shared), str(REPO / "source/tests/client_identity.c"),
                                *[str(shared / name) for name in sources],
                                "-Wl,--gc-sections", "-Wl,--wrap=curl_easy_perform", "-pthread",
                                "-Wl,--wrap=enil_db_set_local_rev",
                                *flags, "-o", binary], check=True, timeout=60)
                result = subprocess.run([binary, temp, f"http://127.0.0.1:{server.server_port}", selected],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

        self.assertFalse(any(path == "/must-not-send" for path, _, _ in records))
        for application, user_agent in [(CHROME_APP, CHROME_UA), (WINDOWS_APP, WINDOWS_UA)]:
            requests = [(path, h, b) for path, h, b in records if h.get("x-line-application") == application]
            for path, headers, body in requests:
                if path.startswith("/api/"):
                    self.assertEqual(headers.get("accept-language"), accept, path)
                    self.assertEqual(headers.get("x-lal"), lal, path)
            catalogs = [json.loads(b) for path, _, b in requests
                        if path.endswith("/getOwnedProductSummaries")]
            self.assertEqual(catalogs, [[shop, 0, 1000, {"language": language, "country": "JP"}]
                                        for shop in ["stickershop", "sticonshop"]])
            for method in ["createSession", "createQrCode", "checkQrCodeVerified",
                           "verifyCertificate", "qrCodeLoginV2", "getProfile"]:
                matches = [(h, b) for path, h, b in requests if path.endswith("/" + method)]
                self.assertEqual(len(matches), 1, method)
                self.assertEqual(matches[0][0]["user-agent"], user_agent)
                self.assertEqual(matches[0][0]["x-line-chrome-version"], "3.7.2")
            completion = next(json.loads(b)[0] for path, _, b in requests if path.endswith("/qrCodeLoginV2"))
            self.assertEqual(completion["systemName"], "WINDOWS" if application == WINDOWS_APP else "CHROMEOS")
            self.assertEqual(completion["modelName"], "DESKTOPWIN" if application == WINDOWS_APP else "CHROME")
            removal = [json.loads(b) for path, _, b in requests if path.endswith('/sendChatRemoved')]
            self.assertEqual(len(removal), 1)
            self.assertEqual(removal[0][1:], ['synthetic-chat', '9007199254740993', 1800000000000])
            for part in ["/tokenRefresh", "/r/talk/", "/api/operation/receive"]:
                matches = [h for path, h, _ in requests if part in path]
                self.assertTrue(matches, part)
                self.assertTrue(all(h["user-agent"] == "saved-session-agent" for h in matches))
            parallel = [h for path, h, _ in requests if path == "/parallel"]
            self.assertEqual(len(parallel), 5)
            self.assertTrue(all(h["user-agent"] == user_agent for h in parallel))
            self.assertTrue(all(h["x-line-access"] == ("desktopwin" if application == WINDOWS_APP else "chrome")
                                for h in parallel))
        assets = [h for path, h, _ in records if path == "/test-avatar"]
        self.assertEqual(len(assets), 2)
        self.assertTrue(all(h["user-agent"] == "saved-session-agent" for h in assets))


if __name__ == "__main__":
    unittest.main()
