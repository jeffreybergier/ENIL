# ENIL tools

Paths and commands below are relative to the repository root.

## Native schema generator

`source/tools/generate.py` generates `source/shared/enil_thrift_schema.inc`
from a pinned LINEJS Thrift IDL supplied as its argument. Run it with
`python3 source/tools/generate.py /path/to/line.thrift` when updating the
reachable protocol schema.

## Windows login probe

This standalone Python tool tests ENIL's candidate Windows identity against
LINE's native QR-login service. It sends Compact Thrift to
`https://legy.line-apps.com/acct/lgn/sq/v1`, uses the secure QR methods and nonce,
and polls `/acct/lp/lgn/sq/v1` for the phone scan and PIN confirmation.
It uses ENIL's existing Worker for E2EE key generation and keychain unwrapping.

The probe stops after token issuance and optional keychain unwrapping. Its
session files are **not compatible with ENIL's current Chrome-gateway transport**.
It does not import accounts into ENIL, test messaging/token refresh, or register
a replacement E2EE key if unwrapping fails. The iOS app now also exposes a native
port of this experiment under **Add Account → Windows login test**. The macOS
Windows selector still uses the earlier Chrome-gateway experiment.

The iOS experiment preserves `Library/Application Support/ENIL/.windows-login-probe/session.json`
inside the app's data container. It is not removed when closing the test screen.
It saves the complete raw token reply before parsing, then the normalized tokens,
certificate, identity, and E2EE/Worker state before optional keychain unwrapping.
Reopening a successful test reuses the saved session without new LINE requests.
The native test file is intentionally rejected by the existing gateway account
parser, so it cannot accidentally start Chrome-gateway sync with native tokens.

## Setup

Requires Python 3.9 or newer. Run these commands from the ENIL repository root:

```sh
python3 -m venv /tmp/enil-windows-probe-env
/tmp/enil-windows-probe-env/bin/python -m pip install -r source/tools/requirements.txt
```

An anonymous endpoint check needs no Worker credentials or phone:

```sh
/tmp/enil-windows-probe-env/bin/python source/tools/probe.py --check-endpoint
```

This creates one unauthenticated QR session and requests a secure QR code.
It does not sign in to an account. A successful response establishes that the
endpoint accepts these requests, not that LINE classifies the session as Windows.

For a full login, set `ENIL_WORKER_URL` and `ENIL_WORKER_SECRET` in the environment
to the same crypto Worker used by ENIL, then run:

```sh
/tmp/enil-windows-probe-env/bin/python source/tools/probe.py
```

The program prints a private output directory and writes `qr.svg` there. Open
that file in a browser on the computer, scan it using LINE on the phone, and
enter the displayed PIN on the phone if requested. Polling starts immediately;
open the image promptly. Ctrl-C stops the probe. It never automatically starts
a second login session after failure.

By default, each run gets a new temporary directory. Use
`--output /path/to/ENIL-extras/windows-login-run-1` to retain results in a specific
**new** directory whose parent already exists. Outputs must remain outside the
ENIL repository. Directories use mode 0700 and files use 0600 on POSIX systems.

## What to check on the phone

After `Token issued`, inspect LINE's logged-in devices. Record the device type
or icon as well as its name: the name `ENIL Windows login probe` was supplied by
the probe and does not independently prove Windows recognition. Also check
whether an existing Chrome login remains active, if testing coexistence.
Remove the probe's device session from the phone when finished.

`report.json` records the advertised identity, RPC names, HTTP statuses,
numeric exception codes, and how far the experiment got. It deliberately leaves
Windows recognition unverified pending this phone-side observation. It excludes
tokens, account identifiers, QR session IDs, and raw server error text.

Other files contain private protocol evidence and credentials:

- `NN-method.response.bin` and `.response.json`: full LINE replies, including
  server error details. Thrift JSON keys are numeric field IDs.
- `qr-key.json`: Worker key state retained before contacting LINE.
- `probe-session.json`: issued tokens and the full login result, saved before
  attempting E2EE unwrapping. This is a probe-specific schema, not `session.json`.
- `e2ee-keys.json`: keys and Worker state if keychain unwrapping succeeds.

Only `VERIFICATION_FAILED` (SecondaryQrCodeException code 2) from
`verifyCertificate` triggers PIN verification. Rate limits, upgrade requirements,
malformed replies, and expired sessions stop the run with their evidence retained.
Long-poll timeouts and HTTP 408 retry within the server's bounded polling budget.
Redirects are rejected, and TLS certificate verification remains enabled.

## Offline tests

```sh
/tmp/enil-windows-probe-env/bin/python -B -m unittest discover \
  -s source/tests -p test_windows_login_probe.py -v
```

The tests exercise fixed request bytes, a synthetic scan/PIN/token flow, the
nonce and headers, URL escaping, error capture, cancellation and timeout
behavior, credential storage, and loopback HTTP errors/redirects. They do not
contact LINE or a real Worker. These Python dependencies are only for the probe;
the native app and Worker builds do not use them.

The native C port also has loopback tests. With `libcjson`, `libcurl`, OpenSSL,
`pkg-config`, and a host C compiler installed, run:

```sh
/tmp/enil-windows-probe-env/bin/python -B -m unittest discover \
  -s source/tests -p test_native_windows_probe.py -v
```

These compile the actual native implementation and verify credential persistence
before unwrapping, recovery from a saved raw token reply, no repeat login after
reopening, and no repeat token request after an uncertain outcome.

## Protocol references and validation

Protocol fields and paths were checked against LINEJS revision
`ef6c3d9f70dd41fa51053615d47f071f58cf8db3`:

- [Device profile](https://github.com/evex-dev/linejs/blob/ef6c3d9f70dd41fa51053615d47f071f58cf8db3/packages/linejs/base/core/utils/devices.ts)
- [Request headers and endpoint](https://github.com/evex-dev/linejs/blob/ef6c3d9f70dd41fa51053615d47f071f58cf8db3/packages/linejs/base/request/mod.ts)
- [QR login field IDs and secure flow](https://github.com/evex-dev/linejs/blob/ef6c3d9f70dd41fa51053615d47f071f58cf8db3/packages/linejs/base/login/mod.ts)
- [Secondary QR exception codes](https://github.com/evex-dev/linejs/blob/ef6c3d9f70dd41fa51053615d47f071f58cf8db3/resources/line/line.thrift)
- [Apache Compact Thrift specification](https://github.com/apache/thrift/blob/v0.22.0/doc/specs/thrift-compact-protocol.md)

On 2026-09-17, a live anonymous check with the pinned Windows identity received
HTTP 200 and valid Compact Thrift success replies for `createSession` and
`createQrCodeForSecure`, including a callback URL and nonce. No account login,
phone device classification, messaging, or coexistence was verified by that check.

The native apps now implement authenticated Talk schemas, native token refresh,
operation polling, and LEGY transport. Production QR handling lives in
`source/shared/enil_native_login.c`; pending recovery/activation lives in
`enil_login_store.c`. This Python probe remains a standalone diagnostic tool.
See [repository guidance](../../AGENTS.md) for the production architecture
and `make test-native` for the combined regression suite.
