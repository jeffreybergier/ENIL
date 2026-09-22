"""Compact Thrift checked against Apache Thrift, plus loopback native routing."""
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest
from thrift.Thrift import TType, TMessageType
from thrift.protocol.TCompactProtocol import TCompactProtocol
from thrift.transport.TTransport import TMemoryBuffer
from test_windows_login_probe import reply, probe
REPO = Path(__file__).resolve().parents[2]


class NativeTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build_dir.cleanup)
        cls.binary = str(Path(cls.build_dir.name) / 'native')
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'libcjson', 'libcurl', 'openssl', 'sqlite3'], text=True))
        shared = REPO / 'source/shared'
        sources = [
            'enil_native.c', 'enil_thrift.c', 'enil_identity.c', 'enil_line.c',
            'enil_http.c', 'enil_b64.c', 'enil_session.c', 'enil_login_store.c',
            'enil_api_json.c', 'enil_sse.c', 'enil_talkserv.c', 'enil_api_call.c', 'enil_db.c',
        ]
        subprocess.run([
            'cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-ffunction-sections', '-fdata-sections',
            '-I' + str(shared), str(REPO / 'source/tests/native_transport.c'),
            *[str(shared / name) for name in sources],
            '-Wl,--gc-sections', '-Wl,--wrap=curl_easy_perform',
            '-Wl,--wrap=curl_easy_getinfo', '-pthread', *flags, '-o', cls.binary,
            '-Wl,--wrap=enil_db_set_local_rev',
        ], check=True)

    def run_native(self, *args, **kw):
        return subprocess.run([self.binary, 'http://127.0.0.1:1', *args],
                              capture_output=True, check=True, **kw).stdout

    def decode(self, method, wire):
        return json.loads(self.run_native('decode', method, input=wire))

    def test_language_headers_and_catalog_locale_on_native_wire(self):
        self.check_language_and_identity("desktopwin", "DESKTOPWIN\t9.7.0.3556\tWINDOWS\t10.0.0-NT-x64",
                                         "Line/9.7.0.3556")

    def test_android_identity_for_catalog_talk_sync_and_refresh(self):
        self.check_language_and_identity("android", "ANDROIDSECONDARY\t26.6.2\tAndroid OS\t16",
                                         "Line/26.6.2")

    def check_language_and_identity(self, profile, application, user_agent):
        records = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                raw = self.rfile.read(int(self.headers['Content-Length']))
                protocol = TCompactProtocol(TMemoryBuffer(raw))
                method, _, _ = protocol.readMessageBegin()
                args = probe.read_value(protocol, TType.STRUCT)
                records.append((self.path, method, dict(self.headers), args))
                # Empty successful structs suffice for these transport checks.
                # Catalog replies additionally contain an empty product list.
                buffer = TMemoryBuffer()
                protocol = TCompactProtocol(buffer)
                protocol.writeMessageBegin(method, TMessageType.REPLY, 0)
                protocol.writeStructBegin('result')
                protocol.writeFieldBegin('success', TType.STRUCT, 0)
                protocol.writeStructBegin('success')
                if method == 'getOwnedProductSummaries':
                    protocol.writeFieldBegin('productList', TType.LIST, 1)
                    protocol.writeListBegin(TType.STRUCT, 0)
                    protocol.writeListEnd()
                    protocol.writeFieldEnd()
                protocol.writeFieldStop()
                protocol.writeStructEnd()
                protocol.writeFieldEnd()
                protocol.writeFieldStop()
                protocol.writeStructEnd()
                protocol.writeMessageEnd()
                wire = buffer.getvalue()
                self.send_response(200)
                self.send_header('Content-Length', str(len(wire)))
                self.end_headers()
                self.wfile.write(wire)

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for selected, language, accept, lal in [('default', 'en', 'en-US', 'en_US'),
                                                     ('ja', 'ja', 'ja-JP', 'ja_JP')]:
                with self.subTest(language=language):
                    records.clear()
                    subprocess.run([self.binary, f'http://127.0.0.1:{server.server_port}',
                                    'language', selected, profile], check=True, timeout=10)
                    self.assertEqual([method for _, method, _, _ in records],
                                     ['getOwnedProductSummaries', 'getOwnedProductSummaries',
                                      'getProfile', 'sync', 'refresh'])
                    for _, _, headers, _ in records:
                        self.assertEqual(headers.get('Accept-Language'), accept)
                        self.assertEqual(headers.get('X-LAL'), lal)
                        self.assertEqual(headers.get('X-Line-Application'), application)
                        self.assertEqual(headers.get('User-Agent'), user_agent)
                        for absent in ['origin', 'cookie', 'x-hmac', 'x-line-chrome-version']:
                            self.assertNotIn(absent, {k.lower() for k in headers})
                    for (path, _, _, args), shop in zip(records[:2], ['stickershop', 'sticonshop']):
                        self.assertEqual(path, '/TSHOP4')
                        self.assertEqual(args, {2: shop, 3: 0, 4: 1000,
                                                5: {1: language, 2: 'JP'}})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_exact_binary_message_and_i64_encoding(self):
        chunks = [b'\0\xff\x80test', bytes(range(256))]
        message = {'from': 'self', 'to': 'peer', 'createdTime': '9007199254740993',
                   'contentType': 0, 'chunks': [base64.b64encode(x).decode() for x in chunks]}
        actual = self.run_native('encode', 'sendMessage', json.dumps([37, message]))
        buffer = TMemoryBuffer()
        protocol = TCompactProtocol(buffer)
        protocol.writeMessageBegin('sendMessage', TMessageType.CALL, 0)
        protocol.writeStructBegin('args')
        protocol.writeFieldBegin('seq', TType.I32, 1)
        protocol.writeI32(37)
        protocol.writeFieldEnd()
        protocol.writeFieldBegin('message', TType.STRUCT, 2)
        protocol.writeStructBegin('message')
        for name, fid in [('from', 1), ('to', 2)]:
            protocol.writeFieldBegin(name, TType.STRING, fid)
            protocol.writeString(message[name])
            protocol.writeFieldEnd()
        protocol.writeFieldBegin('time', TType.I64, 5)
        protocol.writeI64(9007199254740993)
        protocol.writeFieldEnd()
        protocol.writeFieldBegin('content', TType.I32, 15)
        protocol.writeI32(0)
        protocol.writeFieldEnd()
        protocol.writeFieldBegin('chunks', TType.LIST, 20)
        protocol.writeListBegin(TType.STRING, 2)
        for chunk in chunks:
            protocol.writeBinary(chunk)
        protocol.writeListEnd()
        protocol.writeFieldEnd()
        protocol.writeFieldStop()
        protocol.writeStructEnd()
        protocol.writeFieldEnd()
        protocol.writeFieldStop()
        protocol.writeStructEnd()
        protocol.writeMessageEnd()
        self.assertEqual(actual, buffer.getvalue())

    def test_revision_and_malformed_reply(self):
        wire = reply('getLastOpRevision', [(0, TType.I64, 9007199254740993)])
        self.assertEqual(self.decode('getLastOpRevision', wire)['success'], '9007199254740993')
        for i in range(len(wire)):
            r = subprocess.run([self.binary, 'http://127.0.0.1:1', 'decode',
                               'getLastOpRevision'], input=wire[:i], capture_output=True)
            self.assertEqual(r.returncode, 2, i)

    def test_native_routing_refresh_and_exceptions(self):
        records = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                raw = self.rfile.read(int(self.headers['Content-Length']))
                protocol = TCompactProtocol(TMemoryBuffer(raw))
                method, kind, seq = protocol.readMessageBegin()
                records.append((self.path, method, dict(self.headers), raw))
                fields = [
                    (0, TType.STRUCT, [
                        (1, TType.STRING, 'synthetic-mid'), (20, TType.STRING, 'Synthetic')])]
                if method == 'refresh':
                    fields = [(0, TType.STRUCT, [(1, TType.STRING, 'rotated'), (2, TType.I64, 3600),
                               (4, TType.I64, 1800000000), (5, TType.STRING, 'rotated-refresh')])]
                if method == 'getE2EEPublicKey':
                    fields = [(1, TType.STRUCT, [(1, TType.I32, 5), (2, TType.STRING, 'missing')])]
                wire = reply(method, fields)
                self.send_response(200)
                self.send_header('Content-Length', str(len(wire)))
                self.end_headers()
                self.wfile.write(wire)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            origin = f'http://127.0.0.1:{server.server_port}'
            for path, body in [('/api/talk/thrift/Talk/TalkService/getProfile', '[0]'), ('/api/auth/tokenRefresh',
                                                                                         '{"refreshToken":"synthetic-refresh"}'), ('/api/talk/thrift/Talk/TalkService/getE2EEPublicKey', '["peer",1,1]')]:
                out = subprocess.check_output([self.binary, origin, path, body], text=True)
                status, data = out.split('\n', 1)
                data = json.loads(data)
                if path.endswith('getE2EEPublicKey'):
                    self.assertEqual(status, '400')
                    self.assertEqual(data['code'], 5)
                else:
                    self.assertEqual(status, '200')
                    self.assertEqual(data['message'], 'OK')
            self.assertEqual([r[0] for r in records], ['/enc', '/EXT/auth/tokenrefresh/v1', '/enc'])
            for path, method, headers, raw in records:
                lower = {k.lower(): v for k, v in headers.items()}
                self.assertTrue(lower['x-line-application'].startswith('DESKTOPWIN\t'))
                self.assertEqual(lower['user-agent'], 'Line/9.7.0.3556')
                for absent in ['origin', 'cookie', 'x-hmac', 'x-line-chrome-version']:
                    self.assertNotIn(absent, lower)
                if path == '/enc':
                    self.assertEqual(lower['x-lcs'], '0008synthetic')
                    self.assertNotIn('x-line-access', lower)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_stale_sync_preserves_rotated_tokens_and_raw_login(self):
        self.run_native('save', str(Path(self.build_dir.name) / 'session.json'))
        self.assertEqual((Path(self.build_dir.name) / 'session.json').stat().st_mode & 0o777, 0o600)

    def test_native_requests_keep_their_login_generation_across_reauthentication(self):
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                raw = self.rfile.read(int(self.headers['Content-Length']))
                protocol = TCompactProtocol(TMemoryBuffer(raw))
                method, _, _ = protocol.readMessageBegin()
                requests.append(method)
                body = reply('getLastOpRevision', [(0, TType.I64, 20)])
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as root:
                subprocess.run([self.binary, f'http://127.0.0.1:{server.server_port}',
                                'request-generation', str(Path(root) / 'session.json')],
                               check=True, timeout=10)
            self.assertEqual(requests, ['getLastOpRevision', 'getLastOpRevision'])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_only_completed_requests_can_expire_as_idle_polls(self):
        for phase in ['connect', 'tls', 'upload', 'idle']:
            with self.subTest(phase=phase):
                output = self.run_native('timeout-' + phase, 'unused', text=True)
                status = int(output.splitlines()[0])
                self.assertEqual(status, 204 if phase == 'idle' else 0)

    def test_idle_poll_and_truncated_response_timeouts(self):
        for partial_response in [False, True]:
            with self.subTest(partial_response=partial_response):
                release = threading.Event()

                class Handler(BaseHTTPRequestHandler):
                    def log_message(self, *args):
                        pass

                    def do_POST(self):
                        self.rfile.read(int(self.headers['Content-Length']))
                        if partial_response:
                            self.send_response(200)
                            self.send_header('Content-Length', '100')
                            self.end_headers()
                            self.wfile.write(b'partial')
                            self.wfile.flush()
                        release.wait(5)
                        self.close_connection = True

                server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    output = subprocess.check_output([
                        self.binary, f'http://127.0.0.1:{server.server_port}',
                        'short-poll', 'unused'], text=True, timeout=5)
                    self.assertEqual(int(output.splitlines()[0]),
                                     200 if partial_response else 204)
                    self.assertEqual(output.splitlines()[1], '')
                finally:
                    release.set()
                    server.shutdown()
                    server.server_close()
                    thread.join()

    def test_polling_persists_exact_cursors_only_after_success(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']))
                buffer = TMemoryBuffer()
                protocol = TCompactProtocol(buffer)
                protocol.writeMessageBegin('sync', TMessageType.REPLY, 0)
                protocol.writeStructBegin('result')
                protocol.writeFieldBegin('success', TType.STRUCT, 0)
                protocol.writeStructBegin('success')
                protocol.writeFieldBegin('operationResponse', TType.STRUCT, 1)
                protocol.writeStructBegin('opResponse')
                protocol.writeFieldBegin('operations', TType.LIST, 1)
                protocol.writeListBegin(TType.STRUCT, 1)
                protocol.writeStructBegin('op')
                protocol.writeFieldBegin('revision', TType.I64, 1)
                protocol.writeI64(9007199254740993)
                protocol.writeFieldEnd()
                protocol.writeFieldBegin('type', TType.I32, 3)
                protocol.writeI32(0)
                protocol.writeFieldEnd()
                protocol.writeFieldStop()
                protocol.writeStructEnd()
                protocol.writeListEnd()
                protocol.writeFieldEnd()
                protocol.writeFieldBegin('hasMoreOps', TType.BOOL, 2)
                protocol.writeBool(False)
                protocol.writeFieldEnd()
                for fid, value in [(3, 9007199254740994), (4, 9007199254740995)]:
                    protocol.writeFieldBegin('events', TType.STRUCT, fid)
                    protocol.writeStructBegin('events')
                    protocol.writeFieldBegin('revision', TType.I64, 2)
                    protocol.writeI64(value)
                    protocol.writeFieldEnd()
                    protocol.writeFieldStop()
                    protocol.writeStructEnd()
                    protocol.writeFieldEnd()
                protocol.writeFieldStop()
                protocol.writeStructEnd()
                protocol.writeFieldEnd()
                protocol.writeFieldStop()
                protocol.writeStructEnd()
                protocol.writeFieldEnd()
                protocol.writeFieldStop()
                protocol.writeStructEnd()
                protocol.writeMessageEnd()
                data = buffer.getvalue()
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for mode in ['poll', 'poll-fail']:
                subprocess.run([self.binary,
                                f'http://127.0.0.1:{server.server_port}',
                                mode,
                                str(Path(self.build_dir.name) / (mode + '.json'))],
                               check=True,
                               timeout=10)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_kick_interrupts_blocked_native_polls_without_failure_or_cursor_advance(self):
        requests = []
        condition = threading.Condition()
        release = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                wire = self.rfile.read(int(self.headers['Content-Length']))
                with condition:
                    requests.append(wire)
                    condition.notify_all()
                release.wait(15)
                self.close_connection = True

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        process = subprocess.Popen([
            self.binary, f'http://127.0.0.1:{server.server_port}', 'poll-kick',
            str(Path(self.build_dir.name) / 'kick-session.json')], stdin=subprocess.PIPE)
        try:
            for expected in range(1, 8):
                with condition:
                    self.assertTrue(condition.wait_for(lambda: len(requests) >= expected, timeout=4),
                                    f'poll {expected} did not start after reconnect')
                process.stdin.write(b'k' if expected < 7 else b'q')
                process.stdin.flush()
            self.assertEqual(process.wait(timeout=4), 0)
            for wire in requests:
                protocol = TCompactProtocol(TMemoryBuffer(wire))
                self.assertEqual(protocol.readMessageBegin(), ('sync', TMessageType.CALL, 0))
                protocol.readStructBegin()
                self.assertEqual(protocol.readFieldBegin()[1:], (TType.STRUCT, 1))
                protocol.readStructBegin()
                self.assertEqual(protocol.readFieldBegin()[1:], (TType.I64, 1))
                self.assertEqual(protocol.readI64(), 10)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
            release.set()
            server.shutdown()
            server.server_close()
            thread.join()

    def test_interrupted_refresh_recovers_without_network(self):
        self.run_native('refresh-recovery', str(Path(self.build_dir.name) / 'refresh-session.json'))

    def test_reauthentication_during_refresh_rejects_old_credentials(self):
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                raw = self.rfile.read(int(self.headers['Content-Length']))
                protocol = TCompactProtocol(TMemoryBuffer(raw))
                method, _, _ = protocol.readMessageBegin()
                requests.append((method, self.headers.get('X-Line-Access')))
                generation = 'A' if len(requests) == 1 else 'B'
                body = reply('refresh', [(0, TType.STRUCT, [
                    (1, TType.STRING, 'refreshed-' + generation),
                    (5, TType.STRING, 'refresh-' + generation + '-next')])])
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as root:
                (Path(root) / 'synthetic-mid').mkdir()
                (Path(root) / 'staged').mkdir()
                subprocess.run([self.binary, f'http://127.0.0.1:{server.server_port}',
                                'refresh-reauth', root], check=True, timeout=10)
            self.assertEqual(requests, [('refresh', 'access-A'), ('refresh', 'access-B')])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_remove_chat_adapts_gateway_timestamp_to_native_arguments(self):
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                requests.append(self.rfile.read(int(self.headers['Content-Length'])))
                body = reply('sendChatRemoved', [])
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([self.binary, f'http://127.0.0.1:{server.server_port}',
                            'remove-chat', 'unused'], check=True, timeout=10)
            self.assertEqual(len(requests), 1)
            protocol = TCompactProtocol(TMemoryBuffer(requests[0]))
            self.assertEqual(protocol.readMessageBegin(), ('sendChatRemoved', TMessageType.CALL, 0))
            protocol.readStructBegin()
            fields = []
            while True:
                _, kind, fid = protocol.readFieldBegin()
                if kind == TType.STOP:
                    break
                value = protocol.readI32() if kind == TType.I32 else protocol.readString()
                fields.append((fid, kind, value))
                protocol.readFieldEnd()
            self.assertEqual(fields, [
                (1, TType.I32, 37),
                (2, TType.STRING, 'c00000000000000000000000000000000'),
                (3, TType.STRING, '9007199254740993'),
            ])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_binary_message_reply(self):
        buffer = TMemoryBuffer()
        protocol = TCompactProtocol(buffer)
        protocol.writeMessageBegin('sendMessage', TMessageType.REPLY, 0)
        protocol.writeStructBegin('result')
        protocol.writeFieldBegin('success', TType.STRUCT, 0)
        protocol.writeStructBegin('message')
        protocol.writeFieldBegin('id', TType.STRING, 4)
        protocol.writeString('synthetic-message')
        protocol.writeFieldEnd()
        protocol.writeFieldBegin('chunks', TType.LIST, 20)
        protocol.writeListBegin(TType.STRING, 1)
        protocol.writeBinary(b'\0\xff\x80ciphertext')
        protocol.writeListEnd()
        protocol.writeFieldEnd()
        protocol.writeFieldStop()
        protocol.writeStructEnd()
        protocol.writeFieldEnd()
        protocol.writeFieldStop()
        protocol.writeStructEnd()
        protocol.writeMessageEnd()
        decoded = self.decode('sendMessage', buffer.getvalue())['success']
        self.assertEqual(decoded['chunks'], [base64.b64encode(b'\0\xff\x80ciphertext').decode()])

    def test_superseded_and_committed_refresh_journals(self):
        calls = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                wire = self.rfile.read(int(self.headers['Content-Length']))
                protocol = TCompactProtocol(TMemoryBuffer(wire))
                method, _, _ = protocol.readMessageBegin()
                calls.append(method)
                body = reply('refresh', [(0, TType.STRUCT, [
                    (1, TType.STRING, 'fresh-from-fixture'),
                    (5, TType.STRING, 'fresh-refresh-token')])])
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for mode in ['journal-stale', 'journal-superseded',
                         'journal-committed', 'journal-legacy-committed']:
                with self.subTest(mode=mode):
                    subprocess.run([self.binary, f'http://127.0.0.1:{server.server_port}', mode,
                                    str(Path(self.build_dir.name) / (mode + '.json'))], check=True, timeout=10)
            self.assertEqual(calls, ['refresh', 'refresh'])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
