"""Saved Windows login retry through the real Worker client and health gate."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

REPO = Path(__file__).resolve().parents[2]


class NativeLoginRetryTests(unittest.TestCase):
    def test_user_retry_recovers_keys_without_reissuing_credentials(self):
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length']))
                requests.append((self.path, self.headers.get('X-Worker-Secret'), json.loads(body)))
                status = 503 if len(requests) <= 2 else 200
                payload = json.dumps({
                    'keys': [{'keyId': 7, 'exportedKey': 'synthetic-key'}],
                    'workerRestoreState': {'recovered': True},
                }).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='enil-login-retry-') as root:
                shared = REPO / 'source/shared'
                binary = str(Path(root) / 'retry-test')
                flags = shlex.split(subprocess.check_output(
                    ['pkg-config', '--cflags', '--libs', 'libcjson', 'libcurl', 'openssl'], text=True))
                sources = ['enil_qrlogin.c', 'enil_native_login.c', 'enil_login_store.c',
                           'enil_session.c', 'enil_identity.c', 'enil_worker.c', 'enil_health.c',
                           'enil_http.c', 'enil_b64.c', 'enil_line.c', 'enil_chrome_gateway.c', 'enil_native.c',
                           'enil_thrift.c', 'enil_api_json.c', 'enil_talkserv.c', 'enil_api_call.c']
                subprocess.run([
                    'cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-I' + str(shared),
                    str(REPO / 'source/tests/native_login_retry.c'),
                    *[str(shared / name) for name in sources],
                    '-Wl,--gc-sections', '-Wl,--wrap=curl_easy_perform', '-pthread',
                    *flags, '-o', binary,
                ], check=True, timeout=60)
                result = subprocess.run(
                    [binary, root, f'http://127.0.0.1:{server.server_port}'],
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(len(requests), 3)
            for path, secret, body in requests:
                self.assertEqual(path, '/e2ee/unwrap-keychain')
                self.assertEqual(secret, 'synthetic-secret')
                self.assertEqual(body, {
                    'workerRestoreState': {'synthetic': True}, 'qrKeyId': 42,
                    'peerPublicKey': 'synthetic-peer', 'encryptedKeyChain': 'synthetic-keychain',
                })
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
