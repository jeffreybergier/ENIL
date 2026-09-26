# ENIL

ENIL contains native LINE clients for retro macOS/iOS and their Cloudflare
crypto Worker. Paths and build commands below are relative to this repository.

## Native clients

For changes to `source/shared/`, `source/macOS/`, `source/iOS/`, native build
configuration, or native release automation, follow the native rules below.
Outstanding app work is in [AGENTS.TODO.md](AGENTS.TODO.md). Earlier detailed
protocol research can be read from Git history with
`git show b4b99a2^:docs/native-development.md`; verify its status notes
against current code before using them.

Native validation: from the repository root, use `make clean` followed by 
`make debug release` when shared build inputs change. Clean-build macOS and iOS
when shared code changes. For platform-only code, build the affected target.
Inspect compiler warnings and errors. A container cross-build does not establish
that the app launches on a Mac or iOS device; report separately whether that was
tested.

Native outputs live in `build/<platform>/<configuration>/Intermediates` and
the surrounding configuration directory. `BUILD_ROOT` overrides the root.
Use platform-prefixed root targets (`macOS-debug`, `iOS-release`, etc.) for
individual builds. Child source lists and flags live in `build.mk`; the
platform `Makefile` files forward through `source/make/native-targets.mk`.
Run `make test-host` for the build-system and Worker tests hosted on Linux.

The native clients must not execute LINE extension code. They call the Worker
for LTSM crypto and LINE directly for normal API traffic.

## Cloudflare Worker

For `source/cloudflare/`, follow its [guidance](source/cloudflare/AGENTS.md).
Native compiler, UI, memory-management, and Apple SDK rules do not apply to
Worker-only changes. Run `npm test` from `source/cloudflare` for Worker changes.

The build downloads and verifies the extension, then generates the required
JavaScript/WASM. Keep downloaded extension material and generated runtime
assets out of Git. The running Worker is a crypto service and must not make
outbound LINE requests. Do not move extension code into the native clients.

## Dependencies & Build Inputs

A `git clone --recursive` is **not** buildable on its own. Beyond the
committed app source, three input sources are needed:

- **Submodule** (in-repo via `--recursive`): `qrcodegen`, HTTPS and pinned.
- **`/altivec` engine root**: the Make includes
  (`altivec_common_{mac,phone}.mk`, absolute-path) plus prebuilt
  AltivecCore and AltivecCocoa outputs. cJSON, sqlite, libcurl/OpenSSL/zlib,
  the Font Awesome renderer/fonts, and the macOS `AI*` controller base classes
  come from `/altivec/libs/{core,cocoa}/build-{mac,phone}`.
- **`/osxcross` SDK**: every Apple framework (WebKit, Security, ImageIO,
  CoreText, QuartzCore, …) and `-lpthread`/`-ldl`.

The GHCR image contains no Apple SDKs. For local builds, place
`MacOSX10.5.sdk.tar.xz`, `MacOSX11.3.sdk.tar.xz`, and
`iPhoneOS8.4.sdk.tar.gz` in `./.altivec-sdk`, then run
`docker compose run --rm altivec-sdk preflight` and
`docker compose run --rm altivec-sdk install`. Release CI obtains those three
archives from the repository secrets `ALTIVEC_SDK_MACOS_105_URL`,
`ALTIVEC_SDK_MACOS_113_URL`, and `ALTIVEC_SDK_IPHONEOS_84_URL`, then verifies
their pinned hashes before use.

The **Cloudflare Worker** (`enil-cloud`) is a runtime crypto service, not a
build input — see **Cloudflare Worker Role** below.

## House Rules

Please observe the following sections carefully to ensure modifications
to the app do not break our architecture rules

### Strategy

- The Cloudflare worker NEVER talks to the LINE API directly.
- sqlite is the source of truth for the Cocoa UI.
- libcurl + cJSON sync LINE REST endpoints into sqlite.
- Multi-account: every account's data lives in
  `Application Support/ENIL/<accountId>/`.

Required libraries: `cJSON`, `libcurl`, `sqlite`.

### Repository boundaries

- Keep `.env`, local Worker secrets, SDK archives, generated headers, and build
  outputs out of Git.
- Preserve `source/deps/qrcodegen` as a pinned submodule.
- Research archives and captured fixtures live separately in ENIL-extras.
  They are not native or Worker build inputs.
- Native release packaging is configured in `.altivec-release.yml`; GitHub
  release automation is in `.github/workflows/release.yml`.
  
### C and Objective-C boundary

Keep shared protocol, database, sync, and network code in `source/shared/`.
The macOS and iOS UI calls the model through Objective-C APIs and does not
import `enil_*`, `cJSON`, `sqlite`, or `qrcodegen` directly. The sanctioned
model bridges are `ENILAccount.m`, `ENILDictionary.m`, and `enil_cocoa.m`.
The last file holds the shared Apple-framework implementations behind
`enil_cocoa_*.h` pure-C interfaces. C files must not import Cocoa frameworks.
The About/Preferences screens may call `cJSON_Version()` and
`sqlite3_libversion()` to display linked-library versions. Objective-C runtime
and POSIX headers are allowed where needed.

`session.json` holds tokens and E2EE recovery state and cannot be recreated by
sync. `enil.sqlite`, generated chat/picker HTML, and downloaded assets can be
rebuilt. Keep account data under `Application Support/ENIL/<accountId>/`.

For inline sticons, the outgoing text contains one `$` or `(altText)` marker
per resource, in resource order. The `REPLACE.sticon.resources` field carries
the matching resource identities. Preserve this ordering across UI and engine
changes.
  
### ARC on iOS (UI only)

The iOS target compiles its **UI-only** Objective-C — `source/iOS/*.m`, the
`build.mk` `SOURCES` list — with ARC (`-fobjc-arc`). All shared code
(`source/shared/*`, carried in `EXTRA_SOURCES`) stays manual-retain/release
(MRC): it also builds into the Tiger PPC macOS target, where ARC does not exist
and `retain` / `release` / `[super dealloc]` are required. (The macOS target is
MRC throughout.)

- ARC and MRC objects interoperate at the binary level with zero runtime cost;
  the flag is per translation unit. It is scoped in `source/iOS/build.mk` via a
  target-specific variable, `$(SOURCES:.m=.o): IOS_FLAGS += -fobjc-arc`, so the
  shared engine (`altivec_common_phone.mk`) is untouched. Convention: UI files
  go in `SOURCES` (ARC); portable C (cJSON, qrcodegen, the eventual shared
  `enil_*`) goes in `EXTRA_SOURCES` (never ARC).
- No `__bridge` casts are needed: the C↔Objective-C layering rule (below)
  already keeps `enil_*` / `cJSON` / `sqlite` / `qrcodegen` pointers out of UI
  files, so iOS UI code never holds a raw C pointer.
- Deployment floor is iOS 4.3, where zeroing `__weak` is unavailable. Use
  `assign` / `__unsafe_unretained` for delegate back-pointers; reserve `weak`
  for if/when the floor rises to iOS 5+.
  
### Cross-platform code & deprecated APIs

All `#pragma clang diagnostic ignored "-Wdeprecated-declarations"` blocks
live in `source/macOS/XPAppKit.{h,m}`. Caller code stays clean.

- For deprecated method calls: add a category method `XP_<verb>:` in
  `XPAppKit`. The pragma push/pop wraps the deprecated call inside the
  implementation — callers see no pragmas.
- For deprecated constants: pick the modern name at compile time via
  `#if MAC_OS_X_VERSION_MAX_ALLOWED >= <version>` and expose an `XP*` macro.
  Example: `XPModalResponseOK` → `NSModalResponseOK` on 10.13+ SDK,
  `NSOKButton` otherwise (both equal 1).
- When the modern API exists and is callable on the oldest target, prefer
  it at runtime via `respondsToSelector:` + a typed `objc_msgSend` cast.
  This is the modern default — no signature lookup, no boxing, type-checked
  by the compiler. Illustrative pattern (for an `NSNumber`
  `numberWithUnsignedInteger:`-style shim):

  ```objc
  SEL modernSel = @selector(numberWithUnsignedInteger:);
  if ([(id)self respondsToSelector:modernSel]) {
    return ((id (*)(id, SEL, XPUInteger))objc_msgSend)(self, modernSel, value);
  }
  return [self numberWithUnsignedInt:(unsigned int)value];
  ```

  Reach for `NSInvocation` only when the cast is impractical — struct
  return/argument types, or selecting one of several signatures at runtime.
  `XP_setSourceListStyle` is the canonical `NSInvocation` example.
- Blocks (`^`) cannot be used in code that targets the Tiger PPC build —
  the toolchain doesn't support them. For APIs like
  `beginSheetModalForWindow:completionHandler:` there's no modern fallback
  to prefer at runtime; silence the deprecated delegate-style call inside
  `XPAppKit`.
- Existing entries to study before adding new ones: `XP_beginSheet:…`,
  `_XP_NSToolbarSeparatorItemIdentifier`, `XP_setSourceListStyle`,
  `XP_stringByRemovingPercentEncoding`.
- Foundation-only Tiger shims (no AppKit) live in
  `source/shared/XPFoundation.{h,m}` so shared code (`ENILAccount` etc.) can
  use them without dragging in AppKit. Same `XP_*` naming, same patterns.
