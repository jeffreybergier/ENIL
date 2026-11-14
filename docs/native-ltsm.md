# Native LTSM

ENIL defaults to a statically linked **ENILLTSM** C library for all operations
formerly sent to the Cloudflare Worker: request signing, QR/keychain operations,
user/group/room E2EE, group-key distribution, and the separate LEGY transport.
LINE requests still use the existing HTTP client. OBS media encryption is
unchanged. There is no JavaScript engine, WASM interpreter, JIT, local HTTP
server, runtime code download, or automatic remote fallback.

## Boundary and reproducibility

The application is no longer entirely clean-room with respect to extension
crypto: the library contains build-time-translated, verified LINE WASM. Other
extension JavaScript is not embedded or executed by the native app. Review this
distribution boundary before publishing binaries; the translator's MIT license
does not establish licensing for the extension-derived code.

- LINE extension: **3.7.2**, pinned archive metadata and extraction checks in
  `source/cloudflare/scripts/extension.json` and `build.mjs`.
- The existing `native-mids.mjs` adaptation is retained. Adapted WASM SHA-256:
  `0f023ea348adb7c374aa56fd4b2291058516199e053aaf9e969fb1eb32517acc`.
- w2c2 submodule: `8b05ef3d60788642d3dad348781e18942232c3c9`.
- `source/ltsm/generate.py` requires that exact, clean translator checkout. It
  validates the adapted WASM, translates it, derives typed invocation adapters
  and signing identity, adds cancellation checkpoints to all 678 functions, and
  replaces the two abort-on-allocation-failure sites. Unexpected shapes fail.
- `build/ltsm-generated/manifest.json` records hashes of the derived inputs and
  generated outputs. LINE-derived JS, WASM, C and headers are never committed.
  The main generated target is published last; the standard Make generator
  supplies the file lock. Do not concurrently rebuild Worker assets while
  preparing native inputs in a different build root.

The native adapter implements only this module's Embind/emval subset. It is not
an arbitrary Emscripten runtime. `W2C2_RUNTIME_CHECKS=1` is mandatory; memory/table
checks, explicit little-endian memory access, typed indirect calls, and runtime
`isAvailable()` checks remain enabled. Unsupported bindings fail closed.
Upgrading the archive, adaptation, translator, signing identity or host adapter
requires rerunning the differential tests, including a big-endian target.

## Build

In the existing Altivec environment:

```sh
git submodule update --init --recursive
npm ci --prefix source/cloudflare
make ltsm-prepare
make debug release
```

Preparation downloads only when the verified archive cache is missing. CI has
an explicit network-enabled preparation step before its network-disabled SDK
install/build/stage step. Native code does not download anything at runtime.
A subsequent native build can regenerate C from cached inputs without network.

`source/ltsm/products.mk` uses Altivec generators, static products, source groups
and product dependencies. It is included by macOS, iOS and the Linux host-test
Makefile. No app-specific replacement compiler/linker recipes are used.

```sh
make -C source/macOS ENILLTSM CONFIG=release ARCHS="x86_64 arm64"
make -C source/iOS ENILLTSM CONFIG=release ARCHS="armv7 arm64"
make -C source/macOS ENILLTSM CONFIG=release ARCHS="ppc i386"
# Package the archive, public header and translator license:
make -C source/macOS package CONFIG=release PACKAGE_PRODUCTS=ENILLTSM
make -C source/iOS package CONFIG=release PACKAGE_PRODUCTS=ENILLTSM
```

Archives live at `build/<platform>/<config>/libENILLTSM/libENILLTSM.a` when that
product is explicitly built. App dependency builds also create per-architecture
archives under its `Intermediates/`. The public header is
`source/ltsm/enil_ltsm.h`. This implements a **static** library; a separately
packaged dylib/framework is not provided. Consumers also link cJSON, OpenSSL,
pthreads and libm (AltivecCore supplies the crypto/JSON dependencies on Apple).
Use the shared product declarations instead of duplicating compiler recipes.
The library ZIPs are `build/macOS/release/libENILLTSM/ENILLTSM.zip` and
`build/iOS/release/libENILLTSM/ENILLTSM.zip`; each contains the universal archive,
`include/enil_ltsm.h` and `licenses/w2c2.txt`.

GCC 4.2 exceeded the available build-container memory optimizing the obfuscated
functions, including at `-O1` with aggressive compiler GC. Only translated
PPC/i386 sources use `-O0`, in debug and release; handwritten code retains normal
optimization. Clang release sources remain optimized. Native functional success
on an emulated PPC VM is not a latency or battery-life guarantee on real devices.

`ENIL_LOCAL_CRYPTO=0` explicitly builds the old remote backend. Its credentials
are still stored as before, but local builds neither read nor require them.
Preferences explain the local backend and disable the unused credential fields.
The existing Worker-named health gate is retained: a non-cancellation local
crypto failure pauses work until Full Sync resets it. There is no fallback that
sends a failed local operation's keys or plaintext to a Worker.

## Public C API and ownership

```c
#include "enil_ltsm.h"

char *response = NULL;
char error[160];
if (enil_ltsm_request("/sign",
    "{\"path\":\"/S4\",\"body\":\"{}\",\"accessToken\":\"synthetic-token\"}",
    &response, error, sizeof(error))) {
  /* Consume response as JSON. */
  enil_ltsm_free(response);
}
```

The synchronous API accepts the existing Worker's operation path and JSON
contract, not an HTTP URL. The header documents borrowed input and callback
lifetimes and owned output. Run on a background thread. Calls are serialized
inside the library; callbacks must not reenter it. `enil_ltsm_request_ex` checks
cancellation while waiting for the lock, periodically during translated WASM,
and before publishing a response. Cancellation discards the whole request.

Each call gets fresh WASM memory and host state. There is no persistent native
key-handle cache: thread the returned **version-1 `workerRestoreState`** into the
next call. Existing exported key blobs and QR restore records are accepted
without rewriting account files or changing Keychain identities. Interoperability
is tested in both directions against the original WASM, including logical QR
IDs that differ from process-local native handles.

The request cap is 9 MiB. WASM linear memory is fixed at 16 MiB; tracked adapter
allocations are capped at 64 MiB. These are not a total process-RSS bound:
JSON trees, cryptographic libraries, native stacks and caller-owned data add
memory. Per-operation WASM/adapter limits may reject inputs below the JSON cap.
JSON strings containing U+0000 are rejected, rather than silently truncated by
cJSON's C-string representation (literal escaped-backslash text is preserved).
Contexts and tracked sensitive buffers are wiped during teardown; response
strings are wiped by `enil_ltsm_free`. Caller copies and persistent account data
remain the caller's responsibility. Errors never return unauthenticated
plaintext. LEGY's legacy xxHash checksum is **not** a modern authentication MAC;
its protocol behavior is preserved rather than presented as stronger security.

## Offline validation

```sh
make test-host                    # includes the existing Worker tests
make test-ltsm                    # Worker tests, facade checks, differential suite
make -C source/macOS ltsm-test ltsm-check CONFIG=release ARCHS="ppc i386 x86_64 arm64"
make -C source/iOS ltsm-test ltsm-check CONFIG=release ARCHS="armv7 arm64"
```

`test.mjs` drives `ltsm-test` with synthetic keys/messages and compares against
verified original JS/WASM loaded by `reference.mjs`; the reference denies
network fetches. Independent Node X25519/AES-GCM/Compact Thrift fixtures check
user and group crypto. Coverage includes signatures, bidirectional exported
keys and QR restore state, v1/v2 decrypt, v2 encrypt, group-key distribution,
64-bit sequence numbers, malformed input, tamper rejection, subsequent recovery,
and LEGY framing/AES/checksum interoperability. No account files or credentials
are read. Tests of random encryption compare decrypted meaning, not ciphertext.

`ltsm-check` additionally exercises the public API and actual `enil_worker.c`
facade: concurrent callers, cancellation during translated execution, cancellation
while another thread holds the lock, failure recovery, absent credentials, and
the sticky health gate. Its test-only logging/UI stubs do not replace crypto or
facade code. It links without curl; no network implementation is needed.

Sanitizer example (Linux container):

```sh
make -C source/linux ltsm-test ltsm-check CONFIG=debug BUILD_VARIANT=sanitize \
  CC_host=/usr/bin/clang CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  LDFLAGS='-fsanitize=address,undefined'
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  build/linux/debug/variants/sanitize/ltsm-check/ltsm-check
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  node source/ltsm/test.mjs build/linux/debug/variants/sanitize/ltsm-test/ltsm-test
```

For remote native runs, stage only owned files below `~/Desktop`, retain logs,
and give the runner an explicit SSH command, for example:

```sh
node source/ltsm/test.mjs ssh x4-vm Desktop/ENIL-LTSM-20261109-native/ltsm-test-ppc
```

## Opt-in live comparison

Live tests are deliberately excluded from `test-host`. Store `ENIL_WORKER_URL`
and `ENIL_WORKER_SECRET` only in ignored `.env`, mode `0600`, then run:

```sh
node source/ltsm/compare-worker.mjs /explicit/path/to/ltsm-test
```

The script uses synthetic keys/messages only, requires HTTPS, refuses redirects,
limits request duration, and never prints credentials or calls LINE. It does not
reset the Worker's isolate. It compares signatures and bidirectional portable
keys, E2EE and LEGY, not real-account login or delivery. A failed authentication
is a live-service blocker, not a reason to weaken native checks.

## Validation record

Validated on **November 9, 2026** (host date):

| Target | App/library debug + release build/link | Native 124-request suite and API/facade checks |
| --- | --- | --- |
| macOS arm64 | Pass | Both configurations, macOS 15.7.3 / 24G419 |
| macOS x86_64 | Pass | Both configurations, `x9-vm`, 10.9.5 / 13F1911 |
| macOS i386 (GCC 4.2) | Pass | Both configurations, `x9-vm`, 10.9.5 / 13F1911 |
| macOS PPC (GCC 4.2) | Pass | Both configurations, `x4-vm`, 10.4.11 / 8S165 |
| iOS armv7 | Pass | Not run: no device |
| iOS arm64 | Pass | Not run: no device |

Additional results:

- `make clean` followed by `make debug release`: successful universal app
  builds; no non-deprecation compiler warnings.
- App package validation: macOS ZIP and iOS IPA, debug and release. Separate
  universal library ZIPs validated with their public header and w2c2 license.
  App packages contain neither raw JS/WASM nor test harnesses/credentials.
- `make test-host`: 116 Python tests (115 passed; the optional private-database
  test skipped), all 57 Worker tests passed, plus the native facade/concurrency/
  cancellation harness and the 124-request differential suite.
- Linux ASan + UBSan, leak detection enabled: facade/concurrency/cancellation
  harness and all 124 requests passed, with no sanitizer diagnostics.
- Fresh-root generation, compilation and native tests with Docker networking
  disabled: passed using the verified cache. Generated file hashes matched the
  normal build root byte-for-byte.
- Explicit remote backend: release app build and package validation passed for
  macOS arm64 and iOS armv7 with `ENIL_LOCAL_CRYPTO=0`, in separate variants.
  This mode no longer declares or builds native crypto products.
- Clang analysis completed for macOS/x86_64 and iOS/armv7. No diagnostics in the
  new handwritten library or harnesses. The generated-code group disables only
  `deadcode.DeadStores`: w2c2 deliberately emits/reloads scratch locals; all
  analyzer safety checks and runtime bounds checks remain enabled.
- Four existing handwritten analyzer findings remain: possible null string
  comparison in `enil_asset_cache.c:39`, possible nil dictionary key in
  `iOS/QRLoginViewController.m:395`, and unused `rc` assignments in
  `enil_db.c:1053` and `ENILAccount.m:211`. These sites were not changed by this
  work. The null-argument findings merit separate review; unused assignments
  do not themselves establish a runtime failure.
- This checkout has no `check-project` Make target; its consumer-declaration
  tests (`test-build-system`) passed instead.
- Live Worker comparison stopped at **HTTP 401** on its first synthetic `/sign`
  request. No successful live parity result is claimed; the secret was not
  guessed, changed or logged.

VM debug suites are slow under emulation. Initial combined runs exceeded the
20-minute outer command budget without a crypto assertion failure; full reruns
with a longer budget passed. This is functional coverage, not a performance
benchmark. Logs are retained under `build/validation/native-ltsm-20261109/`.

No real account files were used or migrated in the automated validation above.
App GUI launch, real LINE login / message delivery, iOS device execution, and
real Tiger/iOS-minimum hardware behavior are **not** established by those
results. Passing differential fixtures is not an independent security audit.

### User-reported app acceptance — November 13, 2026

- [x] **iPhone 5 / iOS 6:** the user confirmed that the new native-crypto
  ENIL 1.1.4 release IPA worked on the phone after transfer for manual testing.
  Artifact: `build/iOS/release/ENIL/ENIL.ipa`, copied as
  `ENIL-1.1.4-native-crypto-20261109.ipa`.
  SHA-256: `8f2793872dec2b3eae9aeca1ad8535466db10744fc89667650134e6b98cf40aa`.
- The user also reported that the Mavericks app works correctly.

This establishes user-reported native app acceptance on iPhone 5 / iOS 6,
not merely a cross-build. Individual login, messaging, backgrounding and
recovery scenarios were not itemized in the report. The standalone iOS
124-request suite and API/facade checks remain unrun; other iOS versions,
arm64 devices and minimum-supported-OS behavior remain unverified.

### Desktop QR login acceptance — November 15, 2026 (device date)

- [x] **Tiger 10.4.11 / PPC (`x4-vm`), Desktop profile:** the user
  confirmed a fresh QR login, including phone PIN verification, with the clean
  ENIL 1.1.4 release built from `39b7c7b`. The normal app was staged below
  `~/Desktop/ENIL-QR-login-20261115-081356/`; it is not a test harness.
- [ ] **Chrome profile:** not confirmed. The phone reported expired PIN/QR
  codes. Console logs show an existing QR session/key being reused on retry,
  followed by HTTP 400 expiry responses. The initial PIN failure's cause is
  not established; do not treat this as successful Chrome login coverage.
- [ ] **Tablet profile:** not confirmed. Shared implementation with Desktop
  does not establish acceptance of that client identity.

Read-only inspection of Application Support and console logs found the expected
Desktop native-thrift account identity, nonempty access/refresh tokens and E2EE
state, a private per-profile certificate file, and a retired native recovery
record beside the active account. Session/certificate files have mode 0600.
The account directory matches its saved MID. A checksum-verified, coherent
DB/WAL capture contained one account, six contacts, two chats, seven messages,
sticker/sticon catalogs, and no talk exceptions. No account contents or secrets
are checked in. Full database integrity/index validation was not performed:
the inspection host lacks Apple's LOCALIZED collation.

Console logs confirm successful scan, PIN verification, and secure login;
certificate error 2 took the expected PIN-verification fallback. Initial SSE
startup waited for the first sync cursor. Sync reported `Done` at 09:42:37 JST,
then SSE started with a local revision. No new ENIL crash report was present
at inspection. This establishes Desktop login and initial sync/startup, not
all messaging, reconnect, or account-recovery scenarios. Private inspection
artifacts remain ignored under `build/validation/qr-login-release/private/`.
