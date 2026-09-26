"""Offline protocol and transport checks; never contacts LINE or the Worker."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

from thrift.Thrift import TMessageType, TType
from thrift.protocol.TCompactProtocol import TCompactProtocol
from thrift.transport.TTransport import TMemoryBuffer

PROBE = Path(__file__).resolve().parents[1] / "tools/probe.py"
spec = importlib.util.spec_from_file_location("windows_login_probe", PROBE)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def fixture_struct(p, fields):
    p.writeStructBegin("")
    for fid, kind, value in fields:
        p.writeFieldBegin("", kind, fid)
        if kind == TType.STRUCT:
            fixture_struct(p, value)
        elif kind == TType.MAP:
            p.writeMapBegin(TType.STRING, TType.STRING, len(value))
            for key, item in value.items():
                p.writeString(key)
                p.writeString(item)
            p.writeMapEnd()
        else:
            {TType.STRING: p.writeString, TType.I32: p.writeI32,
             TType.I64: p.writeI64, TType.BOOL: p.writeBool}[kind](value)
        p.writeFieldEnd()
    p.writeFieldStop()
    p.writeStructEnd()


def reply(method, fields, kind=TMessageType.REPLY, sequence=0):
    transport = TMemoryBuffer()
    p = TCompactProtocol(transport)
    p.writeMessageBegin(method, kind, sequence)
    fixture_struct(p, fields)
    p.writeMessageEnd()
    return transport.getvalue()


class Flow:
    model_name = "ENIL Windows login probe"
    system_name = "WINDOWS"
    auto_login_required = False
    application = probe.APPLICATION
    user_agent = probe.USER_AGENT
    def __init__(self, *, certificate_code=2, poll_timeout=False, unwrap_fails=False):
        self.certificate_code = certificate_code
        self.poll_timeout = poll_timeout
        self.unwrap_fails = unwrap_fails
        self.calls = []
        self.worker_calls = []
        self.requests = []

    def send(self, url, body, headers, timeout):
        if url.startswith("https://worker.example/"):
            path = url.removeprefix("https://worker.example")
            assert headers["X-Worker-Secret"] == "worker-secret"
            assert "X-Line-Application" not in headers
            data = json.loads(body)
            self.worker_calls.append((path, data))
            if path == "/keygen":
                result = {"keyId": 7, "publicKey": "A+B/C==",
                          "workerRestoreState": {"syntheticPrivate": "private"}}
            else:
                assert path == "/e2ee/unwrap-keychain"
                assert data["qrKeyId"] == 7
                assert data["peerPublicKey"] == "peer"
                if self.unwrap_fails:
                    return 500, b'{"error":"private server message"}'
                result = {"keys": [{"keyId": 9, "exportedKey": "synthetic"}],
                          "workerRestoreState": {}}
            return 200, json.dumps(result).encode()
        assert url.startswith(probe.GATEWAY + "/")
        assert headers["X-Line-Application"] == self.application
        assert headers["User-Agent"] == self.user_agent
        assert headers["Content-Type"] == "application/x-thrift"
        assert not ({"X-Worker-Secret", "X-Hmac", "Origin", "X-Line-Chrome-Version"} & headers.keys())
        p = TCompactProtocol(TMemoryBuffer(body))
        method, kind, seq = p.readMessageBegin()
        assert kind == TMessageType.CALL and seq == 0
        args = probe.read_value(p, TType.STRUCT)
        self.calls.append(method)
        self.requests.append((url, headers, args))
        polling = method.startswith("check")
        assert url.endswith(probe.POLL_PATH if polling else probe.LOGIN_PATH)
        if polling:
            assert headers["X-Line-Access"] == "synthetic-session"
            assert headers["X-LST"] == "1000"
            assert timeout == 11
        else:
            assert "X-Line-Access" not in headers
        if method == "createSession":
            assert args == {}
            result = [(1, TType.STRING, "synthetic-session")]
        elif method == "createQrCodeForSecure":
            result = [(1, TType.STRING, "line://login?existing=1"),
                      (2, TType.I32, 2), (3, TType.I32, 1), (4, TType.STRING, "nonce")]
        elif method == "verifyCertificate" and self.certificate_code is not None:
            return 200, reply(method, [(1, TType.STRUCT,
                                       [(1, TType.I32, self.certificate_code),
                                        (2, TType.STRING, "secret server text")])])
        elif method == "createPinCode":
            result = [(1, TType.STRING, "123456")]
        elif method == "qrCodeLoginV2ForSecure":
            assert args[1] == {1: "synthetic-session", 2: self.system_name,
                               3: self.model_name, 4: self.auto_login_required, 5: "nonce"}
            result = [(1, TType.STRING, "certificate"),
                      (3, TType.STRUCT, [(1, TType.STRING, "private-access-token"),
                                        (2, TType.STRING, "private-refresh-token"),
                                        (6, TType.I64, 1800000000)]),
                      (4, TType.STRING, "private-mid"),
                      (10, TType.MAP, {"publicKey": "peer", "encryptedKeyChain": "encrypted", "keyId": "1"})]
        elif method == "checkQrCodeVerified" and self.poll_timeout:
            self.poll_timeout = False
            raise probe.PollTimeout("synthetic timeout")
        else:
            result = []
        if method != "createSession":
            assert args[1][1] == "synthetic-session"
        return 200, reply(method, [(0, TType.STRUCT, result)])


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="enil-probe-test-")
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name)

    def run_flow(self, **options):
        flow = Flow(**options)
        line = probe.Line(self.output, send=flow.send, status=lambda _: None)
        worker = probe.Worker("https://worker.example", "worker-secret", send=flow.send)
        report, urls, pins = {}, [], []
        with patch.object(probe.time, "sleep"):
            probe.run_login(line, worker, self.output, report,
                            on_qr=lambda url, _: urls.append(url), on_pin=pins.append)
        return flow, line, report, urls, pins

    def test_reference_request_bytes(self):
        # Compact message header, CALL/version, sequence zero, method, STOP.
        self.assertEqual(probe.encode_call("createSession", []),
                         bytes.fromhex("8221000d63726561746553657373696f6e00"))
        self.assertEqual(probe.encode_call("createQrCodeForSecure", probe.request_fields("s")),
                         bytes.fromhex("82210015") + b"createQrCodeForSecure" +
                         bytes.fromhex("1c1801730000"))

    def test_full_flow_headers_nonce_polling_key_capture(self):
        flow, line, report, urls, pins = self.run_flow()
        self.assertEqual(flow.calls, ["createSession", "createQrCodeForSecure",
                         "checkQrCodeVerified", "verifyCertificate", "createPinCode",
                         "checkPinCodeVerified", "qrCodeLoginV2ForSecure"])
        self.assertIn("secret=A%2BB%2FC%3D%3D", urls[0])
        self.assertIn("existing=1", urls[0])
        self.assertIn("123456", pins[0])
        self.assertTrue(report["tokenIssued"])
        self.assertTrue(report["e2eeUnwrapped"])
        self.assertEqual(report["windowsRecognition"], "requires-phone-device-list-check")
        self.assertEqual([p for p, _ in flow.worker_calls], ["/keygen", "/e2ee/unwrap-keychain"])
        state = json.loads((self.output / "probe-session.json").read_text())
        self.assertEqual(state["accessToken"], "private-access-token")
        self.assertEqual(state["transport"], "native-thrift")
        self.assertFalse((self.output / "session.json").exists())
        public = json.dumps({**report, "requests": line.events})
        for secret in ["private-access-token", "private-refresh-token", "private-mid",
                       "secret server text", "synthetic-session", "worker-secret"]:
            self.assertNotIn(secret, public)
        for path in self.output.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_certificate_success_skips_pin(self):
        flow, _, _, _, pins = self.run_flow(certificate_code=None)
        self.assertNotIn("createPinCode", flow.calls)
        self.assertFalse(pins)

    def test_rate_limit_does_not_attempt_pin(self):
        with self.assertRaises(probe.RpcError) as cm:
            self.run_flow(certificate_code=5)
        self.assertEqual(cm.exception.code, 5)
        self.assertFalse((self.output / "probe-session.json").exists())
        self.assertFalse(any("createPinCode" in p.name for p in self.output.iterdir()))

    def test_timeout_retries_poll_not_session_creation(self):
        flow, _, _, _, _ = self.run_flow(poll_timeout=True)
        self.assertEqual(flow.calls.count("checkQrCodeVerified"), 2)
        self.assertEqual(flow.calls.count("createSession"), 1)

    def test_unwrap_failure_preserves_issued_credentials(self):
        _, _, report, _, _ = self.run_flow(unwrap_fails=True)
        self.assertTrue(report["tokenIssued"])
        self.assertFalse(report["e2eeUnwrapped"])
        self.assertTrue((self.output / "probe-session.json").exists())

    def test_expired_session_is_terminal(self):
        line = probe.Line(self.output, send=lambda *a: (410, b"expired"), status=lambda _: None)
        with self.assertRaises(probe.HttpError):
            probe.poll(line, "checkQrCodeVerified", "session", 3, 1)
        self.assertEqual(len(line.events), 1)

    def test_malformed_and_wrong_replies(self):
        for data in [b"", b"not thrift", b"\x82", reply("other", []),
                     reply("method", [], sequence=1),
                     reply("method", [], kind=TMessageType.CALL),
                     reply("method", []) + b"junk"]:
            with self.subTest(data=data), self.assertRaises(probe.ProbeError):
                probe.decode_reply("method", data)

    def test_void_response_and_extra_stop(self):
        for trailing in (b"", b"\0"):
            self.assertEqual(probe.decode_reply("method", reply("method", []) + trailing),
                             (TMessageType.REPLY, {}))

    def test_missing_nonce_stops_before_display_or_poll(self):
        flow = Flow()

        def send(url, body, headers, timeout):
            response = flow.send(url, body, headers, timeout)
            if flow.calls and flow.calls[-1] == "createQrCodeForSecure":
                return 200, reply("createQrCodeForSecure", [(0, TType.STRUCT,
                    [(1, TType.STRING, "line://synthetic")])])
            return response

        line = probe.Line(self.output, send=send, status=lambda _: None)
        worker = probe.Worker("https://worker.example", "worker-secret", send=flow.send)
        with self.assertRaisesRegex(probe.ProbeError, "nonce"):
            probe.run_login(line, worker, self.output, {},
                            on_qr=lambda *args: self.fail("must not display QR"))
        self.assertEqual(flow.calls, ["createSession", "createQrCodeForSecure"])

    def test_cancellation_keeps_report_without_retry(self):
        output = self.output / "cancelled"
        real_line = probe.Line

        def cancel(*args):
            raise KeyboardInterrupt

        with patch.object(probe, "Line", side_effect=lambda path, **kw: real_line(path, send=cancel, **kw)), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(["--check-endpoint", "--output", str(output)]), 130)
        report = json.loads((output / "report.json").read_text())
        self.assertEqual(report["error"], "Cancelled")
        self.assertFalse(report["tokenIssued"])
        self.assertEqual(len(report["requests"]), 1)

    def test_thrift_application_exception_is_preserved(self):
        payload = reply("method", [(1, TType.STRING, "private message"), (2, TType.I32, 1)],
                        kind=TMessageType.EXCEPTION)
        line = probe.Line(self.output, send=lambda *a: (200, payload), status=lambda _: None)
        with self.assertRaises(probe.RpcError) as cm:
            line.call("method", [])
        self.assertTrue(cm.exception.application_exception)
        self.assertNotIn("private message", str(cm.exception))
        self.assertIn("private message", (self.output / "01-method.response.json").read_text())

    def test_http_error_body_is_preserved_without_logging(self):
        messages = []
        line = probe.Line(self.output, send=lambda *a: (403, b"private-body"), status=messages.append)
        with self.assertRaises(probe.HttpError):
            line.call("createSession", [])
        self.assertEqual((self.output / "01-createSession.response.bin").read_bytes(), b"private-body")
        self.assertNotIn("private-body", str(messages))

    def test_existing_outputs_and_repository_capture_are_rejected(self):
        path = self.output / "existing"
        path.write_text("keep")
        with self.assertRaises(FileExistsError):
            probe.private_write(path, b"overwrite")
        self.assertEqual(path.read_text(), "keep")
        with self.assertRaises(probe.ProbeError):
            probe.output_directory(str(probe.REPO / "captures"))
        fresh = probe.output_directory(str(self.output / "new"))
        self.assertEqual(fresh.stat().st_mode & 0o777, 0o700)

    def test_worker_endpoint_validation(self):
        for url in ["http://worker.example", "https://user:pass@worker.example",
                    "https://worker.example?token=secret", "https://worker.example#fragment", ""]:
            with self.subTest(url=url), self.assertRaises(probe.ProbeError):
                probe.Worker(url, "secret")

    def test_endpoint_mode_needs_no_worker_and_reports_no_login(self):
        output = self.output / "endpoint"
        flow = Flow()
        real_line = probe.Line
        with patch.object(probe, "Line", side_effect=lambda path, **kw: real_line(path, send=flow.send, **kw)), \
                patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe.main(["--check-endpoint", "--output", str(output)]), 0)
        report = json.loads((output / "report.json").read_text())
        self.assertEqual(report["phase"], "secure-qr-created")
        self.assertFalse(report["tokenIssued"])
        self.assertEqual(report["windowsRecognition"], "unverified")
        self.assertFalse(flow.worker_calls)

    def test_qr_svg_written_without_network(self):
        with contextlib.redirect_stdout(io.StringIO()):
            probe.display_qr("line://synthetic?secret=synthetic", self.output)
        self.assertIn(b"<svg", (self.output / "qr.svg").read_bytes())

    def test_http_post_preserves_errors_and_refuses_redirects(self):
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                requests.append(self.path)
                self.rfile.read(int(self.headers["Content-Length"]))
                status = 307 if self.path == "/redirect" else 403
                self.send_response(status)
                self.send_header("Location", "/credential-leak")
                self.send_header("Content-Length", "6")
                self.end_headers()
                self.wfile.write(b"detail")

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            origin = f"http://127.0.0.1:{server.server_port}"
            for path, code in [("/error", 403), ("/redirect", 307)]:
                self.assertEqual(probe.post(origin + path, b"request", {}, 2), (code, b"detail"))
            self.assertEqual(requests, ["/error", "/redirect"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    unittest.main()
