#!/usr/bin/env python3
"""Isolated DESKTOPWIN QR-login experiment; never opens an ENIL account.

Protocol reference: evex-dev/linejs ef6c3d9f70dd41fa51053615d47f071f58cf8db3,
base/login/mod.ts and base/request/mod.ts. Uses Apache's Compact implementation.
E2EE key generation/unwrapping stays in ENIL's existing crypto Worker.
"""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

from thrift.Thrift import TMessageType, TType
from thrift.protocol.TCompactProtocol import TCompactProtocol
from thrift.transport.TTransport import TMemoryBuffer

GATEWAY = "https://legy.line-apps.com"
LOGIN_PATH = "/acct/lgn/sq/v1"
POLL_PATH = "/acct/lp/lgn/sq/v1"
APPLICATION = "DESKTOPWIN\t9.7.0.3556\tWINDOWS\t10.0.0-NT-x64"
USER_AGENT = "Line/9.7.0.3556"
MAX_RESPONSE = 2 * 1024 * 1024
MAX_ITEMS = 10000
REPO = Path(__file__).resolve().parents[3]


class ProbeError(Exception):
    """A diagnostic safe to print without credentials or raw server text."""


class RpcError(ProbeError):
    def __init__(self, method, code, application_exception=False):
        self.code = code
        self.application_exception = application_exception
        super().__init__(f"{method}: {'Thrift' if application_exception else 'LINE'} "
                         f"exception, code={code}; see private response capture")


class HttpError(ProbeError):
    def __init__(self, method, status):
        self.status = status
        super().__init__(f"{method}: HTTP {status}; see private response capture")


class PollTimeout(ProbeError):
    pass


def write_struct(protocol, fields):
    protocol.writeStructBegin("")
    for field_id, field_type, value in fields:
        protocol.writeFieldBegin("", field_type, field_id)
        if field_type == TType.STRUCT:
            write_struct(protocol, value)
        elif field_type == TType.STRING:
            protocol.writeString(value)
        elif field_type == TType.BOOL:
            protocol.writeBool(value)
        else:
            raise ValueError("unsupported request field type")
        protocol.writeFieldEnd()
    protocol.writeFieldStop()
    protocol.writeStructEnd()


def encode_call(method, fields):
    transport = TMemoryBuffer()
    protocol = TCompactProtocol(transport)
    # LINEJS uses sequence zero for these stateless HTTP RPCs.
    protocol.writeMessageBegin(method, TMessageType.CALL, 0)
    write_struct(protocol, fields)
    protocol.writeMessageEnd()
    return transport.getvalue()


def read_value(protocol, field_type, depth=0):
    if depth > 32:
        raise ProbeError("Thrift response nesting limit exceeded")
    readers = {TType.BOOL: protocol.readBool, TType.BYTE: protocol.readByte,
               TType.I16: protocol.readI16, TType.I32: protocol.readI32,
               TType.I64: protocol.readI64, TType.DOUBLE: protocol.readDouble}
    if field_type in readers:
        return readers[field_type]()
    if field_type == TType.STRING:
        value = protocol.readBinary()
        try:
            return value.decode("utf-8")
        except UnicodeDecodeError:
            return {"binaryBase64": base64.b64encode(value).decode("ascii")}
    if field_type == TType.STRUCT:
        protocol.readStructBegin()
        result = {}
        for _ in range(MAX_ITEMS):
            _, kind, field_id = protocol.readFieldBegin()
            if kind == TType.STOP:
                protocol.readStructEnd()
                return result
            if field_id in result:
                raise ProbeError("Duplicate Thrift field")
            result[field_id] = read_value(protocol, kind, depth + 1)
            protocol.readFieldEnd()
        raise ProbeError("Thrift field count limit exceeded")
    if field_type in (TType.LIST, TType.SET):
        begin, end = ((protocol.readListBegin, protocol.readListEnd)
                      if field_type == TType.LIST else
                      (protocol.readSetBegin, protocol.readSetEnd))
        kind, count = begin()
        if not 0 <= count <= MAX_ITEMS:
            raise ProbeError("Thrift collection limit exceeded")
        result = [read_value(protocol, kind, depth + 1) for _ in range(count)]
        end()
        return result
    if field_type == TType.MAP:
        key_type, value_type, count = protocol.readMapBegin()
        if not 0 <= count <= MAX_ITEMS:
            raise ProbeError("Thrift map limit exceeded")
        result = {}
        for _ in range(count):
            key = read_value(protocol, key_type, depth + 1)
            if not isinstance(key, (str, int, float, bool)):
                raise ProbeError("Unsupported Thrift map key")
            result[key] = read_value(protocol, value_type, depth + 1)
        protocol.readMapEnd()
        return result
    raise ProbeError("Unsupported Thrift response type")


def decode_reply(method, payload):
    try:
        transport = TMemoryBuffer(payload)
        protocol = TCompactProtocol(transport, string_length_limit=MAX_RESPONSE,
                                    container_length_limit=MAX_ITEMS)
        name, message_type, sequence = protocol.readMessageBegin()
        if name != method or sequence != 0:
            raise ProbeError("Thrift response method/sequence mismatch")
        if message_type not in (TMessageType.REPLY, TMessageType.EXCEPTION):
            raise ProbeError("Unexpected Thrift message type")
        result = read_value(protocol, TType.STRUCT)
        protocol.readMessageEnd()
        # LINEJS's encoder can append one extra STOP after the result struct.
        if transport.read(2) not in (b"", b"\0"):
            raise ProbeError("Unexpected trailing Thrift data")
        return message_type, result
    except ProbeError:
        raise
    except Exception:
        raise ProbeError("Malformed Compact Thrift response; see private capture") from None


def private_write(path, content):
    """All outputs are new files beneath a fresh owner-only directory."""
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as f:
        f.write(content)


def save_json(path, value):
    private_write(path, (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode())


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def post(url, body, headers, timeout):
    request = urllib.request.Request(url, body, headers, method="POST")
    opener = urllib.request.build_opener(NoRedirect())
    try:
        response = opener.open(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        response = error
    except (TimeoutError, socket.timeout):
        raise PollTimeout("Request timed out") from None
    except urllib.error.URLError as error:
        if isinstance(error.reason, (TimeoutError, socket.timeout)):
            raise PollTimeout("Request timed out") from None
        raise ProbeError("Network/TLS request failed") from None
    try:
        with response:
            payload = response.read(MAX_RESPONSE + 1)
            if len(payload) > MAX_RESPONSE:
                raise ProbeError("Response exceeds size limit")
            return response.code, payload
    except (TimeoutError, socket.timeout):
        raise PollTimeout("Response timed out") from None


class Line:
    def __init__(self, output, send=post, status=print):
        self.output, self.send, self.status = output, send, status
        self.events = []

    def call(self, method, fields, *, polling=False, session=None, interval=30):
        path = POLL_PATH if polling else LOGIN_PATH
        headers = {"Content-Type": "application/x-thrift",
                   "Accept": "application/x-thrift", "User-Agent": USER_AGENT,
                   "X-Line-Application": APPLICATION, "X-LAL": "en_US",
                   "X-LPV": "1", "X-LHM": "POST"}
        if polling:
            headers.update({"X-Line-Access": session, "X-LST": str(interval * 1000)})
        event = {"method": method, "path": path}
        self.events.append(event)
        stem = f"{len(self.events):02d}-{method}"
        self.status(f"{method}: POST {path}")
        try:
            http, payload = self.send(GATEWAY + path, encode_call(method, fields),
                                      headers, interval + 10 if polling else 30)
        except PollTimeout:
            event["outcome"] = "timeout"
            self.status(f"{method}: timeout")
            raise
        event["httpStatus"] = http
        private_write(self.output / (stem + ".response.bin"), payload)
        self.status(f"{method}: HTTP {http}")
        if http != 200:
            raise HttpError(method, http)
        message_type, result = decode_reply(method, payload)
        save_json(self.output / (stem + ".response.json"), result)
        if message_type == TMessageType.EXCEPTION:
            code = result.get(2)
            event["thriftExceptionCode"] = code if isinstance(code, int) else None
            raise RpcError(method, event["thriftExceptionCode"], True)
        if 1 in result:
            error = result[1]
            code = error.get(1) if isinstance(error, dict) else None
            event["lineExceptionCode"] = code if isinstance(code, int) else None
            raise RpcError(method, event["lineExceptionCode"])
        if 0 not in result and not (polling or method == "verifyCertificate"):
            raise ProbeError(f"{method}: missing result")
        return result.get(0, {})


class Worker:
    def __init__(self, url, secret, send=post):
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme != "https" or not parsed.hostname or parsed.username
                or parsed.password or parsed.query or parsed.fragment):
            raise ProbeError("ENIL_WORKER_URL must be an HTTPS URL without credentials/query")
        if not secret or any(ord(c) < 32 or ord(c) == 127 for c in secret):
            raise ProbeError("ENIL_WORKER_SECRET is missing or invalid")
        self.url, self.secret, self.send = url.rstrip("/"), secret, send

    def call(self, path, body):
        http, payload = self.send(self.url + path, json.dumps(body).encode(),
                                  {"Content-Type": "application/json",
                                   "X-Worker-Secret": self.secret}, 60)
        if http != 200:
            raise ProbeError(f"Worker {path}: HTTP {http}")
        try:
            result = json.loads(payload)
        except ValueError:
            raise ProbeError(f"Worker {path}: invalid JSON") from None
        if not isinstance(result, dict) or "error" in result:
            raise ProbeError(f"Worker {path}: invalid result")
        return result


def request_fields(session, *extra):
    return [(1, TType.STRUCT, [(1, TType.STRING, session), *extra])]


def required_string(obj, key, label):
    value = obj.get(key) if isinstance(obj, dict) else None
    if not isinstance(value, str) or not value:
        raise ProbeError(f"Missing or invalid {label}")
    return value


def poll(line, method, session, count, interval):
    for attempt in range(count):
        started = time.monotonic()
        try:
            line.call(method, request_fields(session), polling=True,
                      session=session, interval=interval)
            return
        except HttpError as error:
            # An expired session (410) is terminal. Never recreate it silently.
            if error.status != 408:
                raise
        except PollTimeout:
            pass
        if attempt + 1 < count:
            time.sleep(max(0, interval - (time.monotonic() - started)))
    raise ProbeError(f"{method}: polling budget exhausted")


def display_qr(url, output):
    import qrcode
    import qrcode.image.svg
    qr = qrcode.QRCode(border=4)
    qr.add_data(url)
    qr.make(fit=True)
    image = qr.make_image(image_factory=qrcode.image.svg.SvgPathImage)
    data = io.BytesIO()
    image.save(data)
    private_write(output / "qr.svg", data.getvalue())
    print(f"Scan {output / 'qr.svg'} with LINE on your phone.", flush=True)


def run_login(line, worker, output, report, on_qr=display_qr, on_pin=print):
    key = worker.call("/keygen", {"workerRestoreState": {}})
    public_key = required_string(key, "publicKey", "Worker public key")
    if not isinstance(key.get("keyId"), int) or not isinstance(key.get("workerRestoreState"), dict):
        raise ProbeError("Invalid Worker key state")
    # Persist key material before the first LINE request, independently of login.
    save_json(output / "qr-key.json", key)
    created = line.call("createSession", [])
    session = required_string(created, 1, "authSessionId")
    qr = line.call("createQrCodeForSecure", request_fields(session))
    callback = required_string(qr, 1, "callbackUrl")
    nonce = required_string(qr, 4, "secure QR nonce")
    count, interval = qr.get(2, 12), qr.get(3, 30)
    if (type(count) is not int or type(interval) is not int
            or not 1 <= count <= 120 or not 1 <= interval <= 180
            or count * interval > 1800):
        raise ProbeError("Invalid QR polling limits")
    report["phase"] = "waiting-for-scan"
    parts = urllib.parse.urlsplit(callback)
    query = urllib.parse.parse_qsl(parts.query, keep_blank_values=True)
    query = [(k, v) for k, v in query if k not in ("secret", "e2eeVersion")]
    query.extend([("secret", public_key), ("e2eeVersion", "1")])
    url = urllib.parse.urlunsplit(parts._replace(query=urllib.parse.urlencode(query)))
    on_qr(url, output)
    poll(line, "checkQrCodeVerified", session, count, interval)
    report["phase"] = "verifying-certificate"
    try:
        line.call("verifyCertificate", request_fields(session, (2, TType.STRING, "")))
    except RpcError as error:
        # SecondaryQrCodeException.VERIFICATION_FAILED. Do not turn a rate
        # limit, upgrade requirement, or transport failure into a PIN attempt.
        if error.application_exception or error.code != 2:
            raise
        pin = line.call("createPinCode", request_fields(session))
        value = required_string(pin, 1, "PIN")
        if not value.isascii() or not value.isdigit() or len(value) > 12:
            raise ProbeError("Invalid PIN response")
        on_pin(f"Enter PIN {value} in LINE on your phone.")
        report["phase"] = "waiting-for-pin"
        poll(line, "checkPinCodeVerified", session, count, interval)
    report["phase"] = "issuing-token"
    result = line.call("qrCodeLoginV2ForSecure", request_fields(
        session, (2, TType.STRING, "WINDOWS"),
        (3, TType.STRING, "ENIL Windows login probe"),
        (4, TType.BOOL, False), (5, TType.STRING, nonce)))
    token = result.get(3)
    access = required_string(token, 1, "access token")
    refresh = required_string(token, 2, "refresh token")
    # Deliberately NOT ENIL's session schema; no accidental Chrome-gateway reuse.
    state = {"probeSchemaVersion": 1, "transport": "native-thrift",
             "application": APPLICATION, "userAgent": USER_AGENT,
             "accessToken": access, "refreshToken": refresh,
             "certificate": result.get(1), "mid": result.get(4),
             "loginResult": result, "workerRestoreState": key["workerRestoreState"]}
    save_json(output / "probe-session.json", state)
    report.update(phase="token-issued", tokenIssued=True,
                  windowsRecognition="requires-phone-device-list-check")
    metadata = result.get(10, {})
    if (isinstance(metadata, dict) and metadata.get("publicKey")
            and metadata.get("encryptedKeyChain")
            and metadata.get("errorCode") in (None, "0", "SUCCESS")):
        # Keep the successful token result even if this optional diagnostic fails.
        try:
            unwrapped = worker.call("/e2ee/unwrap-keychain", {
                "workerRestoreState": key["workerRestoreState"], "qrKeyId": key["keyId"],
                "peerPublicKey": metadata["publicKey"],
                "encryptedKeyChain": metadata["encryptedKeyChain"]})
            if not isinstance(unwrapped.get("keys"), list) or not unwrapped["keys"]:
                raise ProbeError("Worker returned no E2EE keys")
            save_json(output / "e2ee-keys.json", unwrapped)
            report["e2eeUnwrapped"] = True
        except ProbeError as error:
            report["e2eeUnwrapped"] = False
            report["e2eeError"] = str(error)
    else:
        report["e2eeUnwrapped"] = False
        report["e2eeError"] = "No supported keychain metadata; private login response retained"


def output_directory(argument):
    if not argument:
        path = Path(tempfile.mkdtemp(prefix="enil-windows-login-"))
    else:
        path = Path(argument).expanduser().resolve()
        if path == REPO or REPO in path.parents:
            raise ProbeError("Keep captured fixtures outside the ENIL repository (use ENIL-extras)")
        path.mkdir(mode=0o700, parents=False, exist_ok=False)
    return path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", help="new directory outside ENIL; defaults to a private temp directory")
    parser.add_argument("--check-endpoint", action="store_true",
                        help="only create an anonymous QR session; no Worker or phone required")
    args = parser.parse_args(argv)
    report = {"application": APPLICATION, "gateway": GATEWAY,
              "phase": "starting", "tokenIssued": False,
              "windowsRecognition": "unverified"}
    output = None
    line = None
    code = 0
    try:
        worker = None if args.check_endpoint else Worker(
            os.environ.get("ENIL_WORKER_URL", ""), os.environ.get("ENIL_WORKER_SECRET", ""))
        output = output_directory(args.output)
        print(f"Private results: {output}", flush=True)
        line = Line(output, status=lambda message: print(message, flush=True))
        if args.check_endpoint:
            created = line.call("createSession", [])
            session = required_string(created, 1, "authSessionId")
            qr = line.call("createQrCodeForSecure", request_fields(session))
            required_string(qr, 1, "callbackUrl")
            required_string(qr, 4, "nonce")
            report["phase"] = "secure-qr-created"
            print("Endpoint accepted secure QR creation. Account/device recognition remains untested.")
        else:
            run_login(line, worker, output, report,
                      on_pin=lambda message: print(message, flush=True))
            print("Token issued. Check LINE's logged-in devices on your phone for the device type.")
            print("This probe has not tested messaging, refresh, or Chrome/Windows coexistence.")
    except KeyboardInterrupt:
        report["error"] = "Cancelled"
        code = 130
    except (ProbeError, OSError) as error:
        # OSError may include URL/path details; do not expose arbitrary text.
        report["error"] = str(error) if isinstance(error, ProbeError) else "Local file/network operation failed"
        print(report["error"], file=sys.stderr)
        code = 1
    finally:
        if output is not None:
            report["requests"] = line.events if line else []
            save_json(output / "report.json", report)
    return code


if __name__ == "__main__":
    sys.exit(main())
