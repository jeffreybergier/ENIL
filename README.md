# ENIL

LINE Messenger clients for retro Mac OS X and iOS, with a Cloudflare Worker
for the LINE extension's cryptographic operations. Native source was migrated
from ENIL-cocoa at commit `c1359001724b2651eac73e6b0c129078e291da9c`.

| Directory | Purpose |
| --- | --- |
| `source/shared/` | Shared C/Objective-C protocol, database, sync, media, and account code |
| `source/macOS/` | AppKit app, resources, and Mac Makefile |
| `source/iOS/` | UIKit app, resources, signing entitlements, and iOS Makefile |
| `source/deps/qrcodegen/` | Pinned QR-code generator submodule |
| `source/cloudflare/` | Crypto Worker and extension download/build pipeline |

## Native development

Initialize the dependency after cloning:

```sh
git submodule update --init --recursive
```

Native builds require the Altivec engine/libraries and OSXCross toolchains
provided by the Altivec Intelligence container, plus three external SDK archives:
`MacOSX10.5.sdk.tar.xz`, `MacOSX11.3.sdk.tar.xz`, and `iPhoneOS8.4.sdk.tar.gz`.
Place them in the ignored `.altivec-sdk/` directory, then run from the repo root:

```sh
docker compose run --rm altivec-sdk preflight
docker compose run --rm altivec-sdk install
docker compose run --rm altivec bash -lc 'make clean && make -j2 debug release'
```

Inside a prepared container, build directly from the repository root:

```sh
make                         # Release builds for macOS and iOS
make debug
make macOS-release           # One platform
make iOS-debug
make analyze                 # Reports under build/<platform>/analyze/
make validate
make help
```

Native outputs are in `build/macOS/{debug,release}/` and
`build/iOS/{debug,release}/`. Each configuration contains `Intermediates/`,
`ENIL.app`, debug symbols, and its `ENIL.zip` (Mac) or `ENIL.ipa` (iOS).
The default `build/` sits beside this root Makefile. Override it with
`BUILD_ROOT=/another/path`; relative overrides resolve against the repository
root, including direct child calls such as `make -C source/iOS debug`.
The underlying Altivec rules do not support whitespace in output paths.

Use `make macOS-<target>` or `make iOS-<target>` to forward any child target.
The child `Makefile` files are public wrappers; source lists, flags, and
Altivec compiler/linker rules are configured in each platform's `build.mk`.
`source/make/native-targets.mk` selects output directories and packages iOS
archives without changing the container's installed build engine.

`make clean` removes both native platform output trees; `make macOS-clean`
or `make iOS-clean` removes only one. Unrelated files in a custom build root
are retained, and source/Git directories are rejected as cleaning destinations.
Run cleaning before a parallel build, e.g. `make clean && make -j2 debug release`.
A source checkout alone does not supply SDKs or `/altivec` engine libraries.

Configure the Worker URL and secret in the app's preferences. The native
client also accepts `ENIL_WORKER_URL` and `ENIL_WORKER_SECRET` through its
environment; credentials are not embedded in the committed app source.

Each account's `session.json` saves `clientIdentity.profileId` (the client kind),
`transport`, and the exact application/User-Agent/device values. New Chrome
sessions use `chrome-gateway`; new Windows desktop sessions use `native-thrift`.
Missing identity on old sessions retains the frozen Chrome defaults. The earlier
Windows-header experiment explicitly saved `chrome-gateway` and remains on that
transport; it is never silently converted into a native session.

Both native apps route Windows QR login, Talk RPCs, token refresh, media identity,
and event delivery according to that saved snapshot. Talk and polling use Compact
Thrift inside LEGY encrypted framing; Shop and refresh use their native endpoints.
The Worker handles framing crypto and E2EE, while the app makes every LINE request.
**Deploy the updated Worker before using the Windows account transport.**

A successful `.windows-login-probe/session.json` is reused when choosing Windows
in Add Account, including offline E2EE recovery. No new QR is needed. Windows
login also retains durable `.native-login-*` pending folders outside disposable
staging, so cancellation cannot discard issued credentials. Each login has a
stable `loginId`, and reauthentication attempts record the login they replace.
Activation retires the pending folder permanently, so older
attempts cannot replace newer credentials. Failed Windows sign-ins offer
**Retry saved login** and **Start new QR**; starting fresh retains the previous
attempt for recovery but excludes it from automatic reuse.

Normal session updates preserve unknown login fields and merge changes against
the loaded snapshot. Saves are atomic, fsynced, and private (0600). Refresh replies
are journaled in `session.json.refresh-pending` before applying rotating tokens;
an interrupted update replays that saved reply without another refresh request.
Journals belong to a specific login and record a refresh commit ID, so recovery
also survives inline access-token rotation. Superseded journals are retained as
`session.json.refresh-pending.retired.*`.
Native polling persists separate global/individual cursors as decimal strings.

The protocol definitions are based on pinned LINEJS research (see
[source/tools/native-schema/generate.py](source/tools/native-schema/generate.py)).
A standalone [Windows login probe](source/tools/windows-login/README.md) remains
available for protocol diagnostics.
See [native development](docs/native-development.md), [outstanding work](PLAN.md),
and [release instructions](RELEASE.md).

## Linux-hosted tests

```sh
apt-get install libcjson-dev libcurl4-openssl-dev libssl-dev pkg-config build-essential
python3 -m pip install -r source/tools/windows-login/requirements.txt
npm --prefix source/cloudflare ci
make test-host               # All host protocol, recovery, build-system, and Worker checks
make test-build-system       # Fast wrapper/cleanup checks without Apple SDKs
make test-client-identity    # Synthetic session and loopback HTTP integration
make test-native             # Windows transport, QR, and recovery regression tests
make cloudflare-test         # Worker suite only
```

These tests do not launch a Mac/iOS app. Identity tests compile a temporary
host executable and capture synthetic QR/API/media/SSE requests on loopback,
including concurrent profiles; they never contact LINE or the Worker.
Build-system tests create and remove temporary fixtures; Worker tests use the existing ignored download
cache and generated JS/WASM under `source/cloudflare`. Native source outputs
are always under the selected root build directory.

## Cloudflare Worker

See [source/cloudflare/README.md](source/cloudflare/README.md) for local use,
validation, and deployment. Cloudflare Workers Builds continues to use
`source/cloudflare` as its root, `npm run build` as its build command, and
`npx wrangler deploy` as its deploy command.

Downloaded LINE code and generated JavaScript/WASM are build artifacts, excluded
from Git. The native apps call LINE directly and use the Worker for crypto.
