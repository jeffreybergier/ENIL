# ENIL native development

Hello, you are working on a reverse engineered version of LINE for Retro
computers and devices. This is called ENIL because that is LINE backwards.
It is based on the LINE Chrome Extension (the "helper app" that pairs with a
phone via QR code). The user scans a QR with the full LINE app on their phone
to log in.

This document covers only the native clients in `source/shared/`,
`source/macOS/`, and `source/iOS/`. It was migrated from ENIL-cocoa's agent
instructions; Worker-only changes follow `source/cloudflare/README.md` instead.
All source paths and shell commands are relative to the ENIL repository root.
Outstanding work lives in [PLAN.md](../PLAN.md).

The optional Go reference is `beeper/matrix-line-messenger`, revision
`85db773e1be024fce676562cffaa6e1185a94d49`. Its `AGENTS.md`,
`pkg/connector/consts.go`, and `pkg/line/structs.go` are research references,
not ENIL dependencies. A local copy currently remains in the separate
ENIL-cloud checkout under `matrix-line/`; no fixed filesystem location is
required. Archived protocol research lives separately in ENIL-extras/cloudflare.

## Toolchain

- **Primary Toolchain:** OSXCross 0.13 (ppc-test branch)
- **Host:** Ubuntu 22 (aarch64/x86_64)
- **Installation Path:** `/osxcross/target/bin` (already in `PATH`)
- **Engine Root:** `/altivec`
- **ENIL:** repository root (`/repo/ENIL` inside Compose)
- **Native clients:** `source/shared/`, `source/macOS/`, and `source/iOS/`
- **Cloudflare Worker:** `source/cloudflare/` — runtime crypto service

### Build / deploy

```sh
make clean && make -j2 debug release
```

All four architectures (ppc, x86, x64, arm64) must produce zero warnings.

For shared native code or common build inputs, verify both targets with clean
builds from the repository root. For platform-only changes, build the affected
target. These requirements do not apply to Worker-only changes.

```sh
make clean
make -j2 debug release
```

Check compiler errors and warnings, especially non-deprecation warnings.
When a device or Mac deployment is performed, also check launch output for
`NSException` and `does not recognize selector`. Report device launch testing
separately from cross-compilation; a successful build does not prove launch.

When the user requests deployment, pass the built app path and the requested
device hostname. For example:

```sh
/altivec/bin/altivec-deploy build/macOS/debug/ENIL.app -d <device-host> --yes 60
```

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

## Working Agreements

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

### Clean-room separation from the Chrome extension

The ENIL native clients (and any auxiliary native client code in this repo) is a **clean-room**
client. Do not import, eval, or otherwise execute any code from the LINE
Chrome extension (`chrome/`, deobfuscated bundles, `ltsm.wasm` glue, etc.).

- Reading deobfuscated source to *learn* the protocol is fine and expected.
  Copying LINE API paths, header names, and Thrift-JSON body shapes is
  protocol knowledge, not executable code — that is allowed.
- All HMAC signing, Curve25519 keygen, and E2EE encrypt/decrypt operations
  go through the Cloudflare Worker's HTTP endpoints. If a new crypto
  primitive is needed, add a Worker endpoint — do not embed extension or
  WASM code on the client.
- Rule of thumb: everything the client does must be expressible as a libcurl
  HTTP request + cJSON parsing + stdlib (or OpenSSL) crypto. If it needs
  more, it belongs in the Worker.

## Strategy

- The Cloudflare worker NEVER talks to the LINE API directly.
- sqlite is the source of truth for the Cocoa UI.
- libcurl + cJSON sync LINE REST endpoints into sqlite.
- Multi-account: every account's data lives in
  `Application Support/ENIL/<accountId>/`.

Required libraries: `cJSON`, `libcurl`, `sqlite`.

### Per-Account Data Layout

```
~/Library/Application Support/ENIL/<accountId>/
  session.json         ← tokens, workerRestoreState, E2EE keys, QR login state
  enil.sqlite          ← all synced LINE data (chats, messages, friends)
  sticker-picker.html  ← rendered picker HTML (sticker mode) — see below
  sticon-picker.html   ← rendered picker HTML (sticon mode) — see below
  chats/               ← rendered per-chat HTML pages — see below
    _empty.html        ← "No Chat Selected" placeholder
    <chatId>.html      ← initial page render for one chat
    …
  media/               ← downloaded images and video
  purchases/           ← stickers and emoji packs
```

`session.json` mirrors `.line_session.json` from the Node.js client — same
fields, same structure — so the worker and any other client can read it
without translation. It is the one file that cannot be reconstructed;
`enil.sqlite` can always be rebuilt by re-syncing from LINE.

The two `*-picker.html` files are render artifacts written by
`-[ENILAccount writeStickerPickerHTMLForSticonMode:css:thumbSize:]` so the
WKWebView inside `StickerViewController` can load them via
`-loadFileURL:allowingReadAccessToURL:` — the only WKWebView entry point
that gives the page a real `file://` origin (`loadHTMLString:baseURL:`
hands it an opaque origin that silently blocks every `<img src>` fetch
as cross-origin). They are overwritten on every reload and can be deleted
freely; the picker rewrites them on next open.

The `chats/` directory holds the same kind of render artifacts for the
message pane. `-[ENILAccount writeChatPageHTMLForChatId:outOldest:outHasMore:]`
writes one HTML file per chat (`chats/<chatId>.html`) plus a shared
`chats/_empty.html` placeholder for the no-chat-selected state, and
`ChatMessagesViewController` (the `AIWebViewController` subclass that
fills the message pane) loads them via the same `loadFileURL:` route.
The directory is created lazily on first write and is safe to delete —
content rewrites on the next chat open. `-deleteChatLocally:` does NOT
currently clean up `chats/<chatId>.html` when a chat's DB rows are
dropped; that cleanup is a documented follow-up (orphan files are
harmless but accumulate).

### Code Organisation

All code that does not import UIKit or AppKit goes in `source/shared/`. This
includes the database layer, worker client, LINE API client, and sync engine.
Platform-specific targets (`source/macOS/`, `source/iOS/`) contain only UI
code.

**Sanctioned platform-bridge exceptions in `source/shared/`.** Two shared
files import a UI/platform framework on purpose, because abstracting one
across the macOS/iOS targets *is* their job. Both present a framework-free
API to the rest of the tree and fork internally by `TARGET_OS_IPHONE` / SDK:

- `enil_cocoa.m` — imports Mac frameworks (ImageIO/CoreFoundation/Foundation)
  behind the pure-C `enil_cocoa_*` headers.
- `AIUserNotificationCenter.m` — imports UIKit on iOS (and weak-links
  UserNotifications where the SDK has it) behind the block-free, Foundation-only
  `AIUserNotificationCenter.h`. A `UNUserNotificationCenter`-shaped facade for
  instant local notifications: UN → `NSUserNotification`/`UILocalNotification`
  → no-op, gated by SDK ceiling so a newer SDK lights up the modern path with
  no code change.

A new `.m` in `source/shared/` that imports UIKit/AppKit without being one of
these is a layering violation; put it in the platform target instead.

#### The C ↔ Objective-C boundary

Objective-C files must not reach directly into the project's **C model
layer** — the hand-written `enil_*` headers or the bundled C dependencies
(`cJSON`, `sqlite`, `qrcodegen`). UI code talks to the model through
Objective-C APIs. There are exactly **three sanctioned bridge files** that are
allowed to touch that layer directly, because bridging *is* their job:

- `source/shared/ENILAccount.m` — the model facade; owns the C session,
  DB, sync, and LINE API surface and exposes it to the UI as Cocoa objects.
- `source/shared/ENILDictionary.m` — wraps a C `enil_field_t` struct as an
  `NSDictionary` (`ENILStructDict`); cannot exist without `cJSON`/`strcmp`.
- `source/shared/enil_cocoa.m` — the Apple-frameworks bridge and the single
  shared translation unit that imports a Mac framework. It implements five
  C-callable surfaces, each behind its own narrow pure-C header named
  `enil_cocoa_*` so the include line advertises where the code lives:
  `enil_cocoa_image.h` (ImageIO transcode), `enil_cocoa_date.h`
  (CFDateFormatter timestamps), `enil_cocoa_collate.h` (locale-aware sqlite
  collation), `enil_cocoa_log.h` (`enil_log` — the C↔`NSLog` bridge, for code
  that can't call `NSLog`; the Objective-C `ENILLog` prefix macro is a pure
  `NSLog` passthrough and lives in `XPFoundation.h`, not here), and
  `enil_cocoa_progress.h`
  (`enil_progress_post` / `enil_status_post`, fanned out as `NSNotification`s
  on the main thread).

Any other `.m` reaching into the model layer is a layering violation: add a
method on `ENILAccount` (or one of the bridges) and have the UI call that
instead.

One sanctioned exception to the above: `source/macOS/AboutWindowController.m`
and `source/iOS/PreferencesViewController.m` may call `cJSON_Version()` and
`sqlite3_libversion()` directly. These are read-only, side-effect-free version
strings displayed in the About box (macOS) or the Settings "Linked Libraries"
section (iOS); routing them through `ENILAccount` buys nothing, so they stay at
the call site.

**True system libraries are not the model layer and may be imported directly,
in a limited way.** The boundary exists to keep the `enil_*` model out of the
UI — not to wall off the toolchain's own C. An `.m` file may `#include` the
Objective-C runtime (`<objc/runtime.h>`, `<objc/message.h>`) and POSIX/libc
headers (`<pthread.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<time.h>`,
and the like) when there is no Cocoa expression of what's needed. Keep it
limited: reach for the narrowest header, and only when the
Cocoa/`ENILAccount` route genuinely doesn't exist. Established uses:

- `XPAppKit.m`, `MessageSendViewController.m` —
  `<objc/message.h>` for typed `objc_msgSend` casts (the deprecation-shim
  idiom; see "Cross-platform code & deprecated APIs" above).
- `PreferencesWindowController.m`, `ENILKeychain.m` — `<stdlib.h>` /
  `<string.h>` for `getenv` and string handling.

These are allowances, not new bridge files: importing a system header is
fine, but importing `enil_*` / `cJSON` / `sqlite` / `qrcodegen` from any
non-bridge `.m` remains a layering violation.

Symmetrically, **no `.c` file may import a Cocoa/Mac framework** — with no
exceptions. All shared framework code (ImageIO/CoreFoundation image decoding,
CFDateFormatter timestamps, the sqlite collation, plus the `NSLog`/
`NSNotification` logging and progress bridges) lives in the single
Objective-C file `source/shared/enil_cocoa.m`, reached from portable C only
through its narrow pure-C headers (the `enil_cocoa_*.h` family:
`enil_cocoa_image.h`, `enil_cocoa_date.h`, `enil_cocoa_collate.h`,
`enil_cocoa_log.h`, `enil_cocoa_progress.h`).

### Logging

All logging — C and Objective-C — goes through `NSLog` via two macros so
every line carries a consistent `[<Module>.<function>] ` prefix. The two
macros live in different headers because they solve different problems: C
code *can't* call `NSLog`, so it needs the bridge; Objective-C code already
has `NSLog` and only wants the prefix convention.

- **C / `.c` files**: `#include "enil_cocoa_log.h"` and call
  `ENIL_LOG("Module.functionName", "fmt", args…)`. This routes through the
  `enil_log()` C↔`NSLog` bridge implemented in `enil_cocoa.m`. Per-file files
  may keep a `#define LOG(fn, msg) enil_log("Module." fn, "%s", msg)`
  shorthand for fixed-string call sites (string-literal concatenation
  requires the first argument to be a literal).
- **Objective-C / `.m` files**: `#import "XPFoundation.h"` and call
  `ENILLog(@"ClassName.methodName", @"fmt", args…)` — a pure `NSLog`
  passthrough macro, no C bridge involved. Cocoa format
  specifiers (`%@`, `%lld`, etc.) work as in `NSLog` directly.

Do **not** use `fprintf(stderr, …)` or bare `NSLog(...)` in new code —
both bypass the unified prefix discipline. Every network request site
(LINE, Worker, OBS, raw HTTP) emits at least one request line and one
outcome line (status + URL/path); follow that pattern when adding new
network call paths.

### Sync status notifications and the mini view queue

`enil_progress_post()` and `enil_status_post()` both fan out
`ENILSyncStatusNotification` whose userInfo carries (alongside the
existing `step`/`total`/`unitCount`/`unitTotal`/`message`/`error`) two
optional fields the `SyncMiniViewController` queue consumes:

- `key` (`NSString`, optional) — opaque coalesce identifier. A follow-up
  post with the same `key` **updates** the displayed item in place
  instead of enqueuing a new one. `enil_sync.c` tags each work-phase post
  with a stable key (e.g. `"sync.messages"`) so the bar progresses
  smoothly within a phase; cross-phase transitions are distinct items
  subject to the 3-second minimum dwell.
- `indeterminate` (`NSNumber` BOOL) — `YES` renders as a barber-pole
  spinner / label-only row; `NO` drives the percentage bar from
  `unitCount`/`unitTotal`.

`enil_status_post()` differs from `enil_progress_post()` only in that it
NEVER emits `ENILSyncDidFinishNotification` — it is reserved for
transient out-of-band events (e.g. SSE-driven activity) that should
appear in the queue but must not be mistaken for the canonical sync
ending. Items reaching 100% (`unitCount >= unitTotal` on a determinate
item) bypass the 3-second dwell so the next queued phase can take over
immediately.

## Cloudflare Worker Role

The worker handles all crypto. The Cocoa sync engine calls it via libcurl.
Every LINE API request follows this flow:

1. **Cocoa → Worker** `POST /sign` — worker returns `X-Hmac` for the request
2. **Cocoa → LINE API** — send the request with the `X-Hmac` header
3. *(if response contains E2EE content)*
   **Cocoa → Worker** `POST /decrypt` — pass `workerRestoreState` + ciphertext,
   worker returns plaintext and an updated `workerRestoreState`
4. **sqlite** — write both raw (encrypted) and decrypted columns; write updated
   `workerRestoreState` back to `session.json`

HMAC signing (step 1) is required for every LINE API call, not just crypto
ones.

The worker is stateful via `workerRestoreState`. This blob encodes the worker's
E2EE key handles and must be stored in `session.json` and passed back to the
worker on every E2EE call (key generation, keychain unwrap, message decrypt).

Worker operations:
- `HMAC signing` — computes `X-Hmac` for each LINE API request
- `Curve25519 key generation` — used during QR login to embed a public key in the QR URL
- `E2EE keychain unwrap` — decrypts the key bundle returned by LINE after login
- `E2EE message decryption` — decrypts individual encrypted messages

## E2EE Message Payload and the REPLACE Field

**The LINE API raw message response never contains `REPLACE` in plaintext.**
This has caused confusion before and is worth documenting explicitly.

For an E2EE sticon (inline emoji) message, the raw LINE API response looks like:

```json
{
  "contentType": 0,
  "contentMetadata": {
    "e2eeVersion": "2",
    "STICON_OWNERSHIP": "[\"<packageId>\"]"
  },
  "chunks": ["<iv>", "<ciphertext>", "<counter>", "<senderKeyId>", "<receiverKeyId>"]
}
```

`STICON_OWNERSHIP` is intentionally in plaintext — it tells the client which
sticon packages to prefetch for label resolution, without leaking which
specific sticons are used.

The `REPLACE` field lives **inside `chunks[1]`** (the E2EE ciphertext). Once
the worker decrypts it, the plaintext payload is a JSON object:

```json
{
  "text": "Hello $ and (label)",
  "REPLACE": {
    "sticon": {
      "resources": [
        { "productId": "<pkgId>", "sticonId": "<id>" },
        { "productId": "<pkgId>", "sticonId": "<id>" }
      ]
    }
  }
}
```

`resources` is a 1-to-1 ordered list mapping each sticon position marker in
`text` to its `productId` + `sticonId`. Sticon positions are encoded as either
`$` (no alt text available) or `(altText)` (human-readable label).

After decryption the sync engine injects REPLACE into `contentMetadata` and
stores the updated `raw_json` in sqlite. This is the only source of exact
sticon identity for `$`-marker messages.

**Re-decryption is safe and idempotent.** The channel is pure ECDH: static
private key + static peer public key → shared secret. The same ciphertext
always decrypts to the same plaintext. `workerRestoreState` is a key-handle
cache, not a ratchet counter. Old messages can be re-decrypted at any time to
recover a REPLACE that was not stored during the original sync.

## Threading Model

The sync engine runs entirely in C using pthreads (available on Tiger and
iOS 4.3).

**Sync thread:** A single persistent background pthread runs a serial work
queue. Network requests (worker + LINE API) are enqueued and processed one at
a time in order. This naturally serialises the sign → request → decrypt →
write sequence with no extra coordination.

**UI thread:** A thin Objective-C wrapper posts an `NSNotification` after each
sqlite write. The UI reacts to notifications on the main thread to refresh
its views.

**sqlite concurrency:** WAL mode is preferred but needs to be verified against
Tiger's bundled sqlite version before relying on it.

## Sync Engine Strategy

**Login:** writes `session.json` only. No sync is triggered.

**Sync button:** enqueues a full sync on the background pthread in this order:
1. Profile → upsert `accounts` row
2. Friends list → upsert `contacts_v2` table
3. Chat list → upsert `chats_v2` table
4. Recent messages per chat (last N) → upsert `messages_v2` table
5. Media/sticker downloads → lowest priority, runs last

After each step the sync thread posts an `NSNotification` so the UI can show
progress. If any LINE API call returns an auth error, the queue pauses,
refreshes the token via the worker, then retries before continuing.

## Media Storage

Images and binary media are never stored in sqlite. Instead, files are saved
under the per-account folder and the database stores a path relative to
`Application Support/ENIL/`. The UI resolves the full path at display time by
prepending the Application Support root.

Example: a message image is saved to
`Application Support/ENIL/<accountId>/media/<messageId>.jpg` and the
`messages_v2` row stores `media/<messageId>.jpg`.

## sqlite Schema

Schema matches the LINE API JSON as closely as possible:

- Scalar fields that are queried, sorted, displayed, or joined on get real
  typed columns.
- Complex nested objects (E2EE metadata, configMap, sticker descriptors, etc.)
  are stored as a `raw_json TEXT` column alongside the scalar columns. Nothing
  from LINE is ever discarded.
- For E2EE messages, store both the encrypted raw chunk data and the decrypted
  result as separate columns on the `messages_v2` table.

### Database JSON read exceptions

Database read paths should use typed columns or exploded child tables, not read
stored JSON text back out of sqlite and parse it with cJSON. There are two
documented exceptions:

- **E2EE raw message recovery** — `messages_v2.raw_json` may be read and parsed
  by the decrypt / REPLACE backfill paths. This preserves the original
  ciphertext chunks and lets the worker re-decrypt old messages when plaintext
  or exact sticon identity was not captured during the first sync.
- **Message reactions** — `messages_v2.reactions_json` may be read and parsed
  by `enil_db_message_reactions_update()` when applying SSE reaction deltas, and
  by `enil_html.c` when rendering reaction pills. Reaction state is a compact,
  per-message JSON array because each SSE delta updates one reactor entry and
  the renderer only needs grouped predefined reaction counts.

### Tables in use

All current tables follow a `_v2` naming convention. The bare-name predecessors
(`chats`, `messages`, `sticker_packages`, …) are intentionally left on disk
and unused; `_v2` schemas are created fresh on first run and not migrated
from the legacy ones (see "GO migration recipe" below for the reasoning).

| Table | Purpose |
|---|---|
| `accounts_v2`         | logged-in user profile |
| `contacts_v2`         | per-mid contact roster |
| `chats_v2`            | group/room metadata (not 1:1) |
| `message_boxes_v2`    | per-chat unread/last-seen state for every mid type |
| `messages_v2`         | every message; includes typed media/sticker fields + `raw_json` |
| `message_sticons`     | per-message exploded sticon occurrences (ordinal-keyed) |
| `message_sticon_packages` | packs a message refs (for prefetch) |
| `sticker_packages_v2` | stickershop catalog (owned + discovered) |
| `stickers_v2`         | per-sticker rows (image_path on disk, dims) |
| `sticon_packages_v2`  | sticonshop catalog (owned + discovered) |
| `sticons_v2`          | per-sticon rows (image_path + alt_text) |

Owned packs are populated from `ShopService/getOwnedProductSummaries`;
discovered packs (referenced by a received message we don't own) are written
with `id` + `isPurchased=0` and otherwise NULL. CDN-served meta.json fills in
sticon rows for discovered packs at sync time.

## Sticker & Sticon CDN URLs

- v1 (no hash): `https://stickershop.line-scdn.net/stickershop/v1/sticker/{stkId}/android/sticker.png`
- v2 (hash):    `https://stickershop.line-scdn.net/stickershop/v2/sticker/{stkId}/{hash}/android/sticker.png`
- Sticon:       `https://stickershop.line-scdn.net/sticonshop/v1/sticon/{pkgId}/android/{sticonId}.png`

## Message Sending

`enil_html_js_append` and `enil_html_js_update` generate JS calls
(`appendMessage(…)` / `updateMessage(old_id, …)`) that inject or replace a
message bubble in the WebView without a full page reload. The UI calls these
via `stringByEvaluatingJavaScriptFromString:`.

### Full send flow

1. **Worker encrypt** — call `/e2ee/encrypt-user-v2` (1:1) or
   `/e2ee/encrypt-group-v2` (group) to get ciphertext + updated
   `workerRestoreState`.
2. **LINE API send** — HMAC-sign via `/sign`, then POST to
   `/api/talk/thrift/Talk/TalkService/sendMessage` with `[reqSeq, message]`.
3. **DB upsert** — write the sent message with a `local-NNN` temp id, then
   re-upsert with the real id from the LINE API response.
4. **UI optimistic append** — immediately after the user hits Send, build an
   `ENILHTMLMessage` and call `enil_html_js_append` → inject into WebView.
5. **UI confirm update** — once LINE responds with the real id, call
   `enil_html_js_update` to swap the temp bubble for the real one.

### E2EE send: 1:1 vs group

**1:1 (`u`-prefix mid):**
- `GET negotiateE2EEPublicKey([peerMid])` → peer public key + keyId
- Worker `/e2ee/encrypt-user-v2` with private key, peer public key,
  sender/receiver keyIds
- `reqSeq` and `e2eeSequenceNumber` both increment; persist to `session.json`

**Group (`c`-prefix mid):**
- `GET getLastE2EEGroupSharedKey([1, groupMid])` → group key bundle
- `GET getE2EEPublicKey` for creator and sender public keys
- Worker `/e2ee/encrypt-group-v2` with group key bundle + public keys
- Only `reqSeq` increments (no per-message E2EE sequence for groups)

## LINE Push — SSE at `/api/operation/receive`

The extension does **not** use long-polling. It uses Server-Sent Events.
Source: `LINE_LOGIN_MESSAGE.har` + Chrome extension `main.js` (v3.7.2).

### Endpoint

```
GET https://line-chrome-gw.line-apps.com/api/operation/receive
```

### Query Parameters

| Param | Example | Notes |
|-------|---------|-------|
| `version` | `3.7.2` | extension version |
| `localRev` | `349281` | seeded from `getLastOpRevision`; updated in-band |
| `language` | `en_US` | optional |
| `legyHost` | *(legy host override)* | optional |
| `lastPartialFullSyncs` | `"{}"` | JSON string of last partial sync timestamps |

`localRev` starts from `getLastOpRevision` and advances per event: every
`message` SSE event carries a `revision` field; the client sets
`localRev = data.revision` before reconnecting.

### Auth

The extension uses browser session cookies (`withCredentials: true`). ENIL-Cocoa
uses the same `X-Line-Access` + `X-Hmac` headers as every other TalkService
call. The SSE is a GET with no body; matrix-line sends unsigned GETs and the
gateway accepts them — we currently sign anyway.

### SSE Custom Event Types

| `event:` field | Payload shape | Action |
|---|---|---|
| `message` | `{revision, type, ...params}` | dispatch LINE op by `type`; update `localRev` |
| `fullSync` | `{reasons, nextRevision}` | set `localRev = nextRevision`, run full re-sync |
| `partialFullSync` | `{targetCategories}` | partial re-sync of listed categories |
| `ping` | *(none)* | keep-alive every 20 s; no action required |
| `reconnect` | *(none)* | close and reopen the SSE connection |
| `connInfoRevision` | number | refresh `ci.line-apps.com/R4` if local revision is stale |
| `talkException` | `{code, reason, parameterMap}` | handle auth error / kick |

Implementation status per event-type lives in [PLAN.md](../PLAN.md).

### LINE Operation `type` Values (inside `message` events)

The op-type integers are listed in `AGENTS.md` in the optional matrix-line reference checkout
(sourced from matrix-line's `pkg/connector/consts.go`). Currently
`enil_sync_process_sse_event` only acts on ops carrying an embedded `message`
field (25, 26); the rest are logged via `enil_op_type_name` but dropped on
the floor. Status table in [PLAN.md](../PLAN.md).

### Server Timeout

`polling_timeout: 140000` ms (from `ci.line-apps.com/R4`). The server closes
the stream after ~140 s. The client must reconnect immediately using the
current `localRev`.

### SSE Op Payload — Message Events Are Self-Contained

The SSE `message` event embeds a **complete message object** — the same
structure returned by `getRecentMessagesV2`. No follow-up API call is needed
to fetch the message content.

Op payload shape:

```json
{
  "revision": "349282",
  "type": 26,
  "createdTime": 1234567890,
  "reqSeq": 0,
  "message": { ...full LINE message object... }
}
```

For E2EE messages `message.chunks` is present and must be decrypted via the
worker (same path as the existing sync decrypt flow). For plaintext messages
it is ready to upsert directly.

### Per-Op sqlite Actions (target behaviour)

Simpler op types use `param1`/`param2`/`param3` instead of a full `message`
field.

| Op type | Params | sqlite action |
|---|---|---|
| `RECEIVE_MESSAGE` | `message` object | upsert `messages_v2`; bump `message_boxes_v2.unreadCount` |
| `SEND_MESSAGE` | `message` object | confirm / upsert our own sent message |
| `SEND_CHAT_CHECKED` | `param1`=chatMid, `param2`=msgId | `enil_db_message_box_mark_read` |
| `DESTROY_MESSAGE` / `NOTIFIED_DESTROY_MESSAGE` | `param1`=msgId | `enil_db_message_mark_deleted` |
| `NOTIFIED_UPDATE_CHAT` / `UPDATE_CHAT` | `param1`=chatMid | re-fetch via `getChats` |
| `NOTIFIED_UPDATE_PROFILE` | *(none)* | re-fetch via `getContactsV2` |
| `END_OF_OPERATION` | *(none)* | no-op; just advances `localRev` |

## GO migration recipe (porting structs from matrix-line)

When porting one Go struct from
`pkg/line/structs.go` in the optional matrix-line reference checkout into the typed C layer,
work one struct at a time. Each becomes four things:

- **C struct** in `enil_api_types.h` — mirror the fields: `string`→`char *`,
  `bool`/`int`→`int`, stringy numbers→`long long` (`ENIL_F_I64`), nested
  objects/maps→`cJSON *` (`ENIL_F_JSON`); always end with `cJSON *raw`. Declare
  `xxx_free`, `xxx_fields[]`, `xxx_fields_count`.
- **Typed fetch** in `enil_talkserv.c` — `xxx_build`/`xxx_parse`/`xxx_fetch`
  over `enil_api_call()`. Array responses skip `enil_api_call`, batch manually,
  and return a malloc'd `xxx_t[]` the caller frees (`xxx_free` each, then
  `free`).
- **Typed DB upsert + read** in `enil_db.c` — table `xxx_v2`, created fresh
  (never migrate the legacy table), columns named like the LINE JSON keys.
  Single-row → `enil_db_xxx_get`; multi-row → `enil_db_xxx_get_all`. Put
  `xxx_free`/`xxx_fields[]`/`xxx_fields_count` here for DB-layer structs.
- **Clean `ENILAccount` method** wrapping the row(s) in `ENILStructDict`.

Then update `enil_sync.c` callers (wrap array upserts in a `BEGIN`/`COMMIT`
transaction) and rebuild clean — zero warnings across all four arches.

**Gotcha — `dictionaryWithDictionary:` crashes on nil.** `ENILStructDict`
returns `nil` for absent optional fields (e.g. `displayNameOverridden`), so
`[NSMutableDictionary dictionaryWithDictionary:row]` throws "attempt to insert
nil object". Copy manually into a fresh dictionary and skip nil values instead.

## Static analysis (`make analyze`)

Run `make analyze` from the repository root for both native targets, or
`make macOS-analyze` / `make iOS-analyze` for one. The existing Altivec analyzer
rules resolve app and shared sources from their child working directories,
including Objective-C and C translation units. Reports are written to
`build/macOS/analyze/analyze.txt` and `build/iOS/analyze/analyze.txt` (or under
`BUILD_ROOT` when overridden). Each run replaces its report.

An empty report means no emitted diagnostics. Check the command output and
exit status as well; a cross-build or analyzer run does not test device launch.

## Build entry points and output layout

The root Makefile forwards native commands to platform Makefiles. These
wrappers use `source/make/native-targets.mk`; the original compiler settings
and source lists live in `source/macOS/build.mk` and `source/iOS/build.mk`.
The installed `/altivec` build engine remains unchanged.

Both root and child invocations resolve the output root beside ENIL's root
Makefile. `BUILD_ROOT` may select a different location; relative values are
relative to the repository. macOS and iOS debug/release configurations have
separate `Intermediates` directories, app bundles, symbols, and ZIP/IPA files.
The iOS wrapper packages the already signed app using an absolute IPA path.

Use `make clean && make -j2 debug release` for all four clean builds.
`make macOS-clean` and `make iOS-clean` clean just one platform.
`make test-host` runs the Linux-hosted build-system tests and Worker suite;
these tests do not produce a native debug/release test binary.

It also runs `make test-client-identity`, which compiles the native session and
network code against host libraries and redirects all HTTP traffic to a local
fixture server. Install `libcjson-dev`, `libcurl4-openssl-dev`, `libssl-dev`,
`pkg-config`, and a host C compiler for this check. It uses synthetic tokens and
crypto results; it does not contact LINE or verify live Windows compatibility.

Client identity is an exact snapshot in `session.json` (`clientIdentity`).
`profileId` identifies the client kind; `transport` selects its complete protocol.
Missing identity resolves to the frozen Chrome profile; unknown identities fail.
New Windows sessions use `native-thrift`. Existing `chrome-gateway` snapshots,
including the old Windows-header experiment, keep their original behavior.

`enil_native.c` adapts the existing named-JSON Talk call sites to Compact Thrift
using `enil_thrift.c` and its checked-in schema. Talk `/S4` and sync `/SYNC4` use
LEGY encryption and `https://gf.line.naver.jp/enc`. Shop `/TSHOP4` and refresh
`/EXT/auth/tokenrefresh/v1` use `https://legy.line-apps.com`. The native transport
sends no Chrome Origin, cookies, HMAC, or Chrome-version headers. OBS already uses
the per-session application and User-Agent. E2EE bytes are base64 at the JSON
boundary; i64 revisions remain exact decimal strings.

The Worker adds `/transport/legy/encode` and `/transport/legy/decode`. Deploy it
before the native app. It performs crypto only; LINE requests remain in ENIL.
`enil_sse.c` branches to native sync polling for Windows, then dispatches the same
message/full-sync/partial-sync/error callbacks. Global and individual cursors are
saved only after successful handling. Idle poll timeouts reconnect without
invalidating the session; permanent errors use the existing account error flow.

`enil_windows_probe.c` also supplies the production native QR flow. Both UIs use
`ENILAccount` to keep pending Windows logins outside `.staging-*` cleanup.
The successful historical `.windows-login-probe` is importable without another
QR. Metadata field 10 supplies offline E2EE keychain recovery. Raw login replies
and private QR state are saved before optional unwrap. An uncertain token request
is never automatically repeated. Active sessions never restore older pending
credentials over rotated tokens.

`enil_session_save` merges only fields changed from a normalized loaded snapshot,
so a stale sync save cannot undo token rotation. Unknown native fields survive.
`enil_session_write` uses a unique 0600 temporary file, fsync, and atomic rename.
Refresh responses have a private recovery journal until the session commit
succeeds. `x-line-next-access` inside LEGY responses is also persisted, and native
calls reload the bound account's access token.

Host protocol tests require the dependencies in
`source/tools/windows-login/requirements.txt`, plus the normal C host libraries:

```sh
python -m pip install -r source/tools/windows-login/requirements.txt
python -B -m unittest discover -s source/tests -p 'test_native*.py' -v
python -B -m unittest discover -s source/tests -p test_windows_login_probe.py -v
```

These use synthetic tokens and loopback traffic. Apache Thrift independently
checks message bytes and i64 handling. Tests cover native routes/headers, malformed
replies, polling success/failure cursors, stale-save token preservation, and
interrupted refresh/login recovery. Live read-only validation of the saved
Windows session covered profile, contacts, chats/history, E2EE negotiation,
purchases, and sync; sending still needs a user-driven device test.

Account operations bind a copied identity to their thread before network work,
alongside the existing health binding. `enil_line_post` requires that binding;
it never silently defaults to Chrome. New account operations must bind from
their session before calling token-only Talk helpers. SSE and parallel asset
downloads also carry the snapshot across their pthread boundaries. Public
LINE assets use its User-Agent; authenticated API/media/event requests use
both the application header and User-Agent. Worker requests keep their own
transport behavior. Changes to defaults affect new logins, not saved snapshots.
