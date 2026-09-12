# ENIL — Outstanding Work

This is the working todo list for ENIL. Items are derived from comparing the
current `source/shared/` (Core engine) + `source/macOS/` + `source/iOS/` tree
against the optional `beeper/matrix-line-messenger` Go reference implementation
(revision `85db773e1be024fce676562cffaa6e1185a94d49`).
Architectural background lives in [docs/native-development.md](docs/native-development.md).
Source-location pointers are in that reference checkout's `AGENTS.md`; it is
not an ENIL build dependency.

Items are bucketed by where the work lives:

- **Core** — the platform-agnostic shared engine in `source/shared/` (the C
  protocol / DB / sync / SSE code plus the `ENILAccount` Objective-C facade),
  used by both targets.
- **macOS** — AppKit UI in `source/macOS/`.
- **iOS** — UIKit UI in `source/iOS/`. The read-and-send client already shipped
  via Kaizen milestones 1–9 (2026-05-30…05-31); the iOS architecture reference
  is the appendix at the end of this document.

Some items straddle buckets (an RPC in Core + an affordance in the UI); those
are split and cross-referenced. Checked items are landed in the current tree
(not necessarily pushed).

---

## Core — shared engine (`source/shared/`)

- [ ] **Granular `talkException.code` handling (SSE dispatch).** Act on
      `talkException.code` more granularly: 89 → mark account LSOFF; 117 →
      force re-login; non-119/10051 → drop SSE + surface to UI. Defer until the
      `talk_exceptions` table shows we hit these codes in practice.

- [ ] **Re-fetch `getMessageReadRange` on chat-open** with `count=1` for the
      opened chat. Mirrors Chrome's per-chat-on-open pattern; gives receipts a
      freshness boost between syncs. The engine RPC
      (`talk_get_message_read_range`) already exists from the eager-batched
      full-sync path; the remaining work is exposing a per-chat refetch the UI
      calls when a chat opens.

  Note: SSE reconnect does NOT need a refetch — `localRev` query param + per-event
  `revision` field give LINE's `/api/operation/receive` Kafka-style resume
  semantics, so op 55s that fired during an offline gap get replayed in order
  through the existing dispatch path when the client reconnects with its
  last-seen revision.

- [ ] **Op 140 reactions — send RPCs.** `TalkService/react` and
      `TalkService/cancelReaction`. The per-platform bubble affordance that
      invokes these is tracked under macOS (and, later, iOS).

- [ ] **`BuddyService/getBuddyProfile`** — LINE Official Accounts (`@`-prefix
      mids). Demoted 2026-05-28: no observed issues in practice. OAs that appear
      in chats render as bare mids today, which is ugly but not blocking;
      revisit only if an actual workflow depends on resolving an OA's display
      name / picture.

- [ ] **LSOFF (no-Letter-Sealing) accounts — detect `code:89` at QR login.**
      Detection is engine-side; surfacing a clear message to the user is
      per-platform UI.

- [ ] **Sanity-check `enil_line_acquire_obs_token`** splits the `\x1e`
      separator in `acquireEncryptedAccessToken` response.

- [ ] **Notification coalescing layer.** Right now every SSE event posts
      `ENILSSEEventNotification`, and every `message`-typed event triggers
      `[chatsController_ reloadData]` + (often) other controllers reloading too.
      Effects: chat list flickers during reaction bursts; the open chat header
      doesn't refresh on op 49/121/122; some notifications fire too often,
      others not at all. Want a smarter coalescing layer that posts targeted
      notifications (e.g. `ENILChatChangedNotification:<chatMid>`,
      `ENILContactChangedNotification:<mid>`,
      `ENILMessageReactionsChangedNotification:<messageId>`) and lets
      controllers subscribe only to what they care about. Also: debounce so a
      10-reaction burst becomes one reload, not ten. The targeted-notification +
      debounce machinery is shared (posted from the engine / `ENILAccount`);
      each platform's controllers then subscribe selectively.

- [ ] **URL previews (link unfurl).** `GET legy-jp.line-apps.com/sc/api/v2/`
      `pageinfo/get?url=<url>&caller=LINE_CHROME`. Unsigned (only
      `x-line-access` + `Cookie: lct=…`, **no `x-hmac`** — matrix-line
      `client.go:GetPageInfo`). Maps to matrix-line's `GetPageInfo`; only
      caller there is `handlers/text.go` (enrich an incoming text message
      that contains a URL). Receive-side display only, no E2EE dimension
      (the URL is already plaintext in the message text).

      Ranked lowest because the *fetch* is trivial but doing it correctly
      is real work: the WebView must do **zero networking** (Tiger PPC
      WebKit can't be trusted with modern TLS/SNI/cert chains — the same
      reason every image in ENIL is already a local `file://` URL: message
      media, stickers, sticons, avatars, even the FA OTFs). Design
      constraint when picked up:

      - Response is a **fixed 6-field struct** (`url`, `domain`, `title`,
        `summary`, `image`, `obs.cdn`) — not dynamic JSON; trivial cJSON
        parse, simpler than the structs in the GO migration recipe.
      - Build the GET **outside `enil_line_post`** so it stays unsigned —
        routing it through the signed helper would re-introduce the
        over-signing just removed from SSE (2026-05-17).
      - Sync engine (libcurl, off the WebView) prefetches the preview
        image via the **`obs.cdn`** field, NOT the raw `image` field.
        `obs.cdn` is LINE's CDN-cached copy on the same `line-scdn`/OBS
        host family `enil_obs.c` already talks to with the bundled CA;
        `image` is an arbitrary third-party host that even libcurl may
        choke on.
      - Store the thumbnail under the per-account folder (same pattern as
        `media/`); persist `title`/`summary`/`domain` as columns or a
        small `raw_json` on the message row; `enil_html.c` emits a card
        built entirely from local `file://` + already-fetched text →
        WebView does exactly zero networking, same guarantee as every
        other surface.
      - Best-effort, layered *after* the message renders: if
        `pageinfo/get` or the CDN prefetch fails (common on a flaky
        retro-device link), fall back to a text-only card (`domain` +
        `title`) or the bare link. Must never block or fail the message
        itself — mirrors the 202-retry-then-give-up degradation in
        `enil_obs.c`.
        
[x] Fix Group message picture selection. Right now it looks like we select
    a random photo from the avatars list and display it. But its not even a
    member of the group chat. Its truly just any avatar on disk. LINE has an
    API for group messages because they have custom photos. We should try to 
    use this instead of picking the avatar of a member.
    DONE (2026-06-05): `getChats` already syncs `chats_v2.picturePath`;
    `download_avatars` now downloads it to `avatars/<chatMid>.jpg` (skip-if-
    present) and op 121/122 force-refreshes it. Both ChatListViewControllers key
    the cell avatar by `chatMid` (1:1 → peer u-mid file; group → its own photo;
    no custom photo → placeholder, never a random member). `firstContactMid`
    fallback removed.
    
[x] Animated STICONS do not appear to send correctly. On real line devices they
    show as either static images or just as (emoji text)
    DONE (2026-06-06): Root cause — the outgoing REPLACE block hardcoded
    `resourceType:"STATIC"` + `version:""` for every sticon, so real LINE
    clients always fetched the static frame (we only send metadata; LINE
    fetches the image from its own CDN by resourceType). Confirmed against
    gomadango's "Jeff" DB: the same heart pack `61ea246c…` we sent as STATIC/""
    arrives from the real LINE app as ANIMATION/version 3. Fix plumbs each
    pack's `sticonResourceType` (2 → "ANIMATION") + `latestVersion` from
    `sticon_packages_v2` into the REPLACE builder (new
    `enil_db_get_sticon_pack_send_meta`), and emits `version` as a JSON number
    to match LINE's wire format. Build-verified (4 arches, clean analyze);
    still needs a live send → real-LINE-client check to confirm animation.

### Cryptography — in-process Letter Sealing

**The worker stays. All Letter Sealing crypto (encrypt/decrypt user, group,
keychain unwrap) remains on the Cloudflare Worker.** See "Won't Do" for the
reasoning.

---

## macOS — AppKit UI (`source/macOS/`)

- [ ] **Instant wake recovery via `NSWorkspaceDidWakeNotification`.** The SSE
      idle watchdog already self-heals within ~45 s; this makes wake recovery
      near-instant. Must live in `AppDelegate` (AppKit `NSWorkspace` can't
      go in shared `ENILAccount`): observe wake on
      `[[NSWorkspace sharedWorkspace] notificationCenter]`, iterate
      `_accounts`, call `-forceReconnectSSE` on each engine. The API hook is
      already in place and currently has no caller.

- [ ] **Op 140 reactions — bubble affordance.** Right-click on a bubble →
      React (and cancel), driving the Core `react` / `cancelReaction` RPCs
      above.

---

## iOS — UIKit UI (`source/iOS/`)

The Kaizen read-and-send client shipped (milestones 1–9, 2026-05-30…05-31).
Remaining UI work:

- [ ] **10 — (LOW PRIORITY) Image-accurate inline compose via `contenteditable`
      `UIWebView`.** Step 8 shipped working inline compose but with a *placeholder*
      paperclip glyph standing in for every sticon (a `UITextView` can't render
      arbitrary downloaded PNGs inline pre-iOS-7 — see the constraints in *"Step 8
      research"*). This step replaces the compose `UITextView` with a
      **`contenteditable` `UIWebView`** (the existing *candidate B*) so picked
      sticons render as their **actual `<img>`** interleaved with text, with
      WebKit handling caret / selection / backspace-deletes-image natively. The
      app already lives in `UIWebView` and owns the sticon HTML/JS asset pipeline,
      so the image source and `file://` origin plumbing are reuse, not new work.
      Sketch: insert a sticon by `document.execCommand`-ing an `<img>` tagged with
      `data-package-id` / `data-sticon-id` / `data-alt`; on Send, DOM-walk the
      editable region in JS to emit the **same ordered `(markered text, resources)`
      pair** the engine already consumes — so `-sendInlineSticons:text:toChatId:`
      and the whole send path are untouched; only the *editor surface* and its
      serializer change. The controller-side `sticonRuns` model and the
      paperclip-codepoint bookkeeping from Step 8 retire entirely (WebKit becomes
      the source of truth). **Risks to scout first:** old-WebKit
      `contenteditable` quirks on the 4.3–6 floor, and `inputAccessoryView` /
      keyboard / first-responder plumbing for a web-based field (a `UITextView`
      gets this for free). Keep Step 8's `UITextView` path as the fallback if
      WebKit `contenteditable` proves too quirky on the floor. *(Proves:
      image-accurate inline sticons in compose.)*

The iOS architecture reference — locked decisions, navigation structure,
class-by-class mapping, the `ENILRootCoordinator` design, the 4.3 floor, and the
Step 8 inline-sticon research that backs Step 10 — is the **appendix** at the
end of this document.

---

## Recommended ordering (impact-per-effort)

1. Op 140 reactions — Core `react` / `cancelReaction` RPCs + the macOS bubble
   affordance.
2. Granular `talkException.code` handling (89 / 117 / others).
3. Notification coalescing layer (Core).
4. URL previews (Core).

---

## Won't Do

Explicitly out of scope. These are documented so future-you doesn't
re-litigate them.

- **Peer-delivered receipts (delivery state).** Investigation (HAR +
  matrix-line audit): LINE does not expose peer-delivery as a protocol
  concept. There is no `getDelivered…`/`notifyDelivered…` RPC and no SSE op
  type for delivery (matrix-line's exhaustive `consts.go` lists 25/26
  send/receive, 55 read, plus chat/contact/reaction ops — no delivered).
  `MessageBox` carries a `lastDeliveredMessageId` field which we already
  persist via `getMessageBoxes` (`message_boxes_v2.lastDeliveredMessageId` +
  `lastDeliveredTime`), but it is **MY-side** state ("server has pushed
  through this id to my account on some device"), not peer-side — proven by
  the self-chat case in the HAR where `lastDelivered.messageId` is strictly
  greater than `lastSeenMessageId` despite there being no peer. Currently
  consumed only as the watermark by `ENILAccount.markChatReadAtServerHead`'s
  `sendChatChecked` call. LINE's own UI has never shown a "delivered"
  indicator (only 既読 / Read), and the protocol matches that.
- **Video send/receive (contentType=2).** No transcoding pipeline, no
  `emv` upload path, no video HMAC-over-128KB-chunk-hashes encryption
  branch (`pkg/connector/media.go`'s `encryptFileData` video case).
  Confirmed scope on 2026-05-14.
- **GIF send.** PNG/JPEG inputs only. PNG is transcoded to JPEG via
  ImageIO before the wire payload is built; GIF is rejected at the
  `NSOpenPanel` filter. No animated-image preservation.
- **The `emv` / `ema` SID branches** of `determineMediaMessageFlow`.
  We only branch on `flowMap["1"]` (image). Video/audio flow values are
  observed and discarded.
- **Op 25 `reqSeq` dedup (port of `sentReqSeqs` from matrix-line
  `sync.go:602-610`).** matrix-line stashes the outgoing `reqSeq` before
  each send and drops the op-25 echo entirely when it returns — needed
  because Matrix has already locally echoed the message and a second
  render would duplicate it. ENIL's situation is different: `is_outgoing`
  is computed from `from_mid == my_mid` so phone-sent messages render
  correctly as outgoing, and ENIL's own sends already dedup via
  `pendingSends_` keyed on message id. The only race `reqSeq` would
  cover (SSE op 25 arriving before the `sendMessage` HTTP response) has
  not been observed in practice — the HTTP response consistently beats
  the SSE echo. Not worth porting until we see actual duplicate bubbles.
  Confirmed scope on 2026-05-14.
- **LSOFF / non-E2EE peer fallback for sticons.** matrix-line's
  `send_message.go:43` branches to a plaintext send (text with positional
  `(altText)` markers, no REPLACE, no `e2eeVersion`) when
  `negotiateE2EEPublicKey` returns `ErrNoUsableE2EEPublicKey`. ENIL's
  `enil_line_send_inline_sticon` always takes the E2EE path. The peers
  we actually test against are all E2EE-enabled, so this gap is not
  exercised; the scenarios that need it (LINE Official Accounts, peers
  with Letter Sealing disabled in settings, accounts with expired keys)
  are out of scope for the current target. Revisit if/when a real LSOFF
  peer becomes part of the test matrix.
- **STICON_OWNERSHIP normalization beyond what the send path already
  does.** `enil_line.c:1535-1543` already builds a deduped
  `["<packageId>", …]` array in the order the sticons appear in the
  message. matrix-line normalizes (sorts) for testability, but ENIL has
  no equivalent test harness comparing wire bytes against the real
  client, so the cost of changing the order would be matched by no
  benefit. The receive side already treats the field as a set, not an
  ordered list. Reopen only if we find LINE actually rejects/penalizes
  certain orderings.
- **STKTXT on incoming stickers — render as alt/title.** STKTXT is a
  per-message label the *sender* picks from a small popup ("Cute",
  "Tasty", "OK!") when sending a sticker; it isn't a property of the
  sticker itself and is often empty. The `stickers_v2.alt_text` column
  exists but populating it from STKTXT would conflate "this sticker
  always means X" with "the sender said X this one time," which is
  wrong. Per-sticker labels just don't exist in LINE's data model —
  `productInfo.meta` has only `{id, width, height}` per sticker. The
  LINE Chrome extension itself doesn't render any per-sticker tooltip
  for the same reason. Confirmed scope on 2026-05-14.
- **Porting Letter Sealing to in-process OpenSSL.** The private keys in
  `session.json:e2eeKeys[].exportedKey` are SKB-wrapped (whitebox AES,
  ~204 bytes — not raw 32-byte X25519). Unwrapping requires running
  LINE's SKB whitebox. matrix-line itself does **not** reimplement this;
  it transpiles `ltsm.wasm` via `wasm2c`+`c2go`
  (`pkg/ltsm/wbc_generated.go`) and runs the WASM in-process. For ENIL
  the equivalent would be shipping `wasm2c` output + an embind shim +
  the initial memory image — closer to redistributing LINE's compiled
  code than reverse engineering. The Cloudflare Worker stays as the SKB
  host; all `/e2ee/encrypt-*`, `/e2ee/decrypt-*`,
  `/e2ee/create-group-key`, and `/sign` endpoints remain load-bearing.
  Confirmed scope on 2026-05-14.
- **`AuthService/confirmE2EELogin`.** Only needed if QR login stops
  going through the Cloudflare Worker for Curve25519 keygen / E2EE
  keychain unwrap. The worker is load-bearing and staying (see "Porting
  Letter Sealing to in-process OpenSSL" above), so this RPC has no
  caller. Reopen only if the worker is ever dropped from the QR login
  path.
- **Outbound group ops: `createGroup`, `inviteIntoGroup`,
  `accept/reject/cancelGroupInvitation`, `kickoutFromGroup`,
  `leaveGroup`, `updateGroup`.** ENIL is a clean-room *companion*
  client: it reads and participates in chats that already exist on the
  paired phone. Group lifecycle management is owned by the full LINE
  app on the phone. Leaving a chat is already covered locally by the
  Leave/Delete-chat toolbar buttons (`sendChatRemoved`); the rest add a
  large outbound RPC surface for a workflow the phone already handles
  well. Reopen only if a real use case needs group creation/admin from
  the retro client itself.
- **"Unsend" affordance on outgoing bubbles.** The `unsendMessage` RPC
  is landed (`-[ENILAccount unsendMessageId:]`) and the inbound side is
  fully handled — ops 64/65 mark `messages_v2.is_deleted=1` and the
  renderer honors it. What's dropped is the *outbound* UI: a
  right-click / context affordance on our own bubbles to retract a sent
  message. ENIL is a clean-room companion client; unsending is a
  low-frequency action the paired phone handles, and a per-bubble
  context menu in the WebView is a disproportionate amount of
  JS/AppKit plumbing for it. Reopen only if retract-from-the-retro-client
  becomes a real workflow.
- **Fetching `ci.line-apps.com/R4` on `connInfoRevision`.** Investigated
  2026-05-28: matrix-line treats `connInfoRevision` as a keep-alive
  (`pkg/connector/sync.go:569` — same `return` as `ping`) and never
  fetches R4. ENIL already drops the event implicitly — the
  `event_type != "message"` early return in
  `enil_sync_process_sse_event` (`enil_sync.c:1267`) skips dispatch, and
  `record_sse_event` already persists the embedded revision. The R4
  payload's `polling_timeout` and `legyHost` fields would need new
  consumers (libcurl timeouts aren't currently dynamic, `SSE_HOST` is
  hardcoded), so fetching it would produce data nobody uses. Reopen
  only if we observe SSE-disconnect symptoms that point back to a
  stale revision.

### Message types to not support
- Audio (contentType=3)
- Contact / vCard (contentType=13 / `ORGCONTP=CONTACT`)
- File (contentType=14)
- Location (contentType=15;
   `message.location.{title,address,lat,lng}`)
- Call (`ORGCONTP=CALL`)
- Replies (`relatedMessageId` + `messageRelationType ∈ {0,3}`) — struct
   field exists, UI/JS handling still missing
- Group system messages (`contentType=18, LOC_KEY="C_PN"` rename;
    member add/remove/leave)

---

# Appendix — iOS UI Port Reference

Background for the iOS UI port (the `source/iOS/` UIKit layer that mirrors the
macOS UI). The shared engine (`source/shared/`) is cross-platform and already
compiles into the iOS target; this appendix covers **only** the UI layer. The
macOS UI it mirrors lives in `source/macOS/`.

## Locked decisions (2026-05-30)

| Decision | Choice | Consequence |
|---|---|---|
| Top-level structure | **Navigation drill-down** | One `UINavigationController`: Chats → Thread. Settings / Accounts / QR are modals. No tab bar. |
| Multi-account | **One active account + switcher** | Exactly one live `ENILAccount` at a time. The switcher lists *other* accounts from disk without starting an engine. |
| Device scope | **iPhone first** | Compact-width layout now (matches the current `Info.plist` / bootstrap). iPad reuses the same VCs under a `UISplitViewController` later. |

---

## What ports unchanged vs. what is rewritten

**Ports near-unchanged — the entire `source/shared/` layer.** `ENILAccount`, the
C engine, sync, SSE, the worker client, the DB, and the **rendered HTML +
incremental JS** are UI-framework-agnostic — designed cross-platform, though
never built against an iOS SDK until Milestone 1 wired the engine into the iOS
Makefile. That first cold compile needed exactly two small guard fixes
(Font Awesome's ATSUI path, now in AltivecCocoa, plus `XPFoundation.m` Tiger
file ops — 32-bit fallbacks that wrongly caught iOS armv7; see Milestone 1);
every other shared file compiled untouched. The UI binds to the model through
five `NSNotification`s and a set of
`ENILAccount` methods, nothing more:

- `ENILSyncStatusNotification` / `ENILSyncDidFinishNotification` — sync progress
- `ENILMessageJSNotification` — carries `appendMessage(…)` / `updateMessage(…)` /
  `prependMessages(…)` JS strings for incremental DOM updates
- `ENILSSEEventNotification` — raw SSE event passthrough
- `ENILSyncStateChangedNotification` — `ENILSyncState` transitions

Two whole subsystems therefore port with **zero UI rewrite** because they live
in the rendered HTML, not in AppKit:

1. **Incremental message updates.** `ENILMessageJSNotification.js` is evaluated
   via `UIWebView`'s `-stringByEvaluatingJavaScriptFromString:` — the *identical*
   selector the macOS WebView uses. Optimistic sends, SSE arrivals, and
   pagination all light up by forwarding that payload.
2. **Custom-scheme actions.** Sticker taps and "load more" come back as
   `enil-sticker:` / `enil-sticon:` URLs. macOS intercepts them in its nav/policy
   delegate; iOS intercepts the same URLs in
   `-webView:shouldStartLoadWithRequest:navigationType:`. Same protocol,
   different delegate method.

**Rewritten — everything in `source/macOS/`.** The `AI*` cookie-cutter base
classes (`AICookieCutterWindowController`, `AIViewController`,
`AIWebViewController`) are a hand-rolled NSViewController/NSSplitView system
built because Tiger lacks `NSViewController`. They are pure AppKit and do **not**
port. iOS uses native `UIViewController` + `UINavigationController` instead.

---

## Navigation structure

```
UIWindow
└─ UINavigationController                    ← the single stack
   └─ ChatListViewController (root)          ←  was: sidebar pane
      nav bar:  [Account ▾]        [Sync] [+]
      │
      └─push→ MessageListViewController     ←  was: detail pane
              UIWebView  ← loads the SAME chats/<chatId>.html
              ├ inputAccessoryView: MessageSendViewController  ←  was: child VC of the same name
              │     [photo] [ text…………… ] [⌨/stickers/emoji] [Send]
              └ inputView (swaps the keyboard): StickerViewController  ←  was: inspector pane
              nav bar:  [‹ Chats]   Name   [⋯ mark-read/leave/delete]

   Presented modally over the stack:
     • QRLoginViewController        (Add Account / Reauthenticate)
     • PreferencesViewController    (worker URL/secret)
     • AboutViewController          (exists)
     • AccountSwitcherViewController (from [Account ▾])
```

The macOS three-pane window maps onto three *different* iOS containment idioms,
not three sibling views:

- **sidebar → nav-stack root.** The chat list is a screen, not a pane.
- **detail → pushed screen.** The thread is the next screen.
- **inspector + send bar → keyboard furniture.** The send bar is an
  `inputAccessoryView` (bar pinned above the keyboard); the sticker picker is an
  `inputView` (replaces the keyboard). The text view stays first responder;
  tapping "stickers" swaps its `inputView` and calls `-reloadInputViews`. This
  is the macOS intent ("inspector lives next to the messages, toggled from the
  bar") expressed in native iOS grammar — and it removes the deep child-VC
  nesting entirely.

---

## Class-by-class mapping

| macOS (AppKit) | iOS (UIKit) | Notes |
|---|---|---|
| `AppDelegate` (heavy) | `AppDelegate` (thin) + `ENILRootCoordinator` (new) | Account discovery / credential gate / QR orchestration moves into the coordinator. |
| `MainMenu` (5 menus) | nav-bar buttons, action sheets, swipe actions, Settings | Menu commands redistribute (see below). |
| `AICookieCutterWindowController` | `UINavigationController` | The 3-pane base dissolves into the stack. |
| `AIViewController` / `AIWebViewController` | native `UIViewController` / optional `ENILWebViewController` (UIWebView base) | iOS has the real thing. |
| `ChatWindowController` (per-account orchestrator) | *(no peer)* — its job splits between the coordinator + each VC's nav bar | |
| `ChatListViewController` | `ChatListViewController : UITableViewController` | chats are a table on both. |
| `MessageListViewController` | `MessageListViewController` (UIViewController + UIWebView) | loads the **same** rendered HTML; names aligned. |
| `MessageSendViewController` (child VC) | `MessageSendViewController` (a `UIView`, the `inputAccessoryView`) | a view, not a VC; name aligned. |
| `StickerViewController` (inspector) | `StickerViewController` (a `UIView`, the `inputView`, hosts a UIWebView) | swaps the keyboard; macOS renamed from `ShinStickerViewController`. |
| `StatusViewController` + `SyncMiniViewController` | table header view + nav-bar Sync button | no child VC needed initially. |
| `PreferencesWindowController` | `PreferencesViewController : UITableViewController` | worker URL + secret. |
| `QRLoginWindowController` | `QRLoginViewController` (modal) | QR image + PIN + status; macOS renamed from `ENILQRLoginWindowController`. |
| `AboutWindowController` | `AboutViewController` | **already done.** |
| *(none — macOS uses N windows)* | `AccountSwitcherViewController` (new) | multi-account on one screen. |

**Menu commands redistribute:** Send → input-bar button · Mark-as-Read / Leave /
Delete → `⋯` action sheet or swipe-to-delete in the list · Load-More → the
in-HTML control already posts an `enil-` URL (ports for free) · Show
Stickers/Emoji → input-bar toggle · Attach Photo → `UIImagePickerController` ·
Add Account / Log Out / Reauthenticate → account switcher.

---

## `ENILRootCoordinator` — the old `AppDelegate` brain, minus windows

A plain `NSObject` the `AppDelegate` creates and retains. Owns the
`UINavigationController` and the **one** live `ENILAccount`. Responsibilities,
all ported from the macOS `AppDelegate`:

1. **Credential gate.** *(landed 2026-05-30.)* No worker credentials → make
   `PreferencesViewController` the nav root; observe
   `ENILKeychainDidChangeNotification` → `continueAfterCredentials`.
   `PreferencesViewController` (`UITableViewController`, grouped) is the port of the
   macOS `PreferencesWindowController` worker pane: a URL field + a secure secret
   field prefilled from `ENILKeychain`, an env-override footer hint (mirrors
   macOS, via `getenv`), Save gated on both-non-empty, and inline footer errors
   instead of an alert (`UIAlertView`/`UIAlertController` both warn against the
   8.4 SDK on the 4.3 floor — same trap as the modal API). Save →
   `-saveWorkerURL:secret:`, whose notification lets the coordinator swap the
   editor off the root.
2. **Discover accounts.** Same `~/Library/Application Support/ENIL/<mid>/` scan
   and `name == mid` invariant as macOS `-discoverAccountPaths`.
3. **Activate one.** Start its engine (`-startWithError:` + `-startSSE`), set the
   nav root to its `ChatListViewController`. Zero accounts → present
   `QRLoginViewController`.
4. **Switch.** `-stopSSE` + release the old engine, instantiate + start the new
   one, swap the nav root.
5. **Present modals.** QR, Settings, About, switcher.

The switcher needs **no** extra engines: it lists the other accounts by reading
each `session.json` through `+[ENILAccount validatedMidForSessionAtPath:]` and
`+displayNameForSessionAtPath:` — no engine, no DB open, no SSE. So macOS's
`ENILAccountContext` (`{path, engine, controller}`) collapses to a list of
`{path, displayName}` for the switcher plus a single live `engine`.

---

## iOS 4.3 floor — the constraints that shaped this

The deployment floor is iOS 4.3 (`Info.plist` `MinimumOSVersion`), which rules
out the modern conveniences and echoes the Tiger constraints the macOS side
already lives with:

- **`addChildViewController:` is iOS 5.0** → child-VC containment is unavailable,
  the same gap Tiger has with `NSViewController` (the reason `AIViewController`
  exists). The iOS fix is cleaner: the send bar and picker become
  `inputAccessoryView` / `inputView` (no containment), and sync status becomes a
  header view — so **no** hand-rolled containment is needed.
- **`WKWebView` is iOS 8.0** → use `UIWebView`. Bonus: UIWebView gives `file://`
  pages a real file origin, so the `loadFileURL:allowingReadAccessToURL:` dance
  from the macOS WKWebView path is unnecessary — `-loadRequest:` on the file URL
  just works.
- **Auto Layout and `UIRefreshControl` are iOS 6.0** → springs-and-struts
  (`autoresizingMask`) layout, and a nav-bar Sync button rather than
  pull-to-refresh.
- **Blocks and GCD are available (iOS 4.0)** — unlike the Tiger PPC target, iOS
  UI code may use `^{}` and `dispatch_async`, simplifying the async / `try…catch`
  patterns the global rules ask for.

Every new `.m` lands in the platform `build.mk` `SOURCES` list, inheriting ARC via the
existing `$(SOURCES:.m=.o): IOS_FLAGS += -fobjc-arc` rule. Portable C stays in
`EXTRA_SOURCES` (never ARC), per [docs/native-development.md](docs/native-development.md).

---

## Step 8 research — inline sticons in an editable compose field (pre-`NSTextAttachment` iOS)

### The problem, precisely

The compose field must let the user **interleave typed text and tapped sticons
in one editable line** and, on Send, produce the pair the engine already wants:

- `text` — a string with one **position marker** per sticon, in order: `$` when
  the sticon has no alt text, or `(altText)` when it does (the exact
  `REPLACE.sticon` convention documented in [docs/native-development.md](docs/native-development.md)).
- `resources` — an **ordered** `NSArray` of `{productId, sticonId}`, one per
  marker.

That pair is fed to the **already-existing** engine entry point
`-[ENILAccount sendInlineSticons:resources:text:toChatId:]`. So the *data model*
is a solved problem; **only the on-screen inline rendering is hard.**

### How macOS does it (the reference we can't copy)

`source/macOS/MessageSendViewController.m` puts a `SticonAttachment :
NSTextAttachment` (subclass carrying `packageId` + `sticonId`, with a
baseline-shifted `SticonAttachmentCell` resized to 18pt) **directly into an
editable `NSTextView`'s text storage** at the caret via
`+[NSAttributedString attributedStringWithAttachment:]`. At send time it
enumerates the attributed string's attachments → builds `resources` + the marker
text. Each attachment occupies exactly **one `U+FFFC` OBJECT REPLACEMENT
CHARACTER** in the backing string, so caret/selection/backspace and
character-index↔resource mapping all fall out of normal text editing.

**iOS has no equivalent below iOS 7.** `NSTextAttachment`, image rendering of
attachments in `UITextView`, and TextKit (`NSLayoutManager` /`NSTextContainer`
/`NSTextStorage`) are **all iOS 7.0+**. iOS 6 gives `UITextView.attributedText`
but it ignores attachments, and crucially **pre-iOS-7 `UITextView` does not let
you control per-glyph advances** (it lays out via WebKit, not a Core Text stack
you own), so you cannot cleanly *reserve exact image width* inside it.

### iOS availability matrix (the constraints that shape the choice)

| API / capability | Available since | Verdict for iOS 6 target |
|---|---|---|
| `NSTextAttachment` image in `UITextView` (TextKit) | **iOS 7.0** | ❌ out |
| `UITextView.attributedText` (no attachments) | iOS 6.0 | ⚠️ width reservation only via tuned spaces |
| Core Text: `CTRunDelegate` + `kCTRunDelegateAttributeName` + `U+FFFC` reserve/draw | iOS 3.2 | ✅ exact width, **display-only** |
| `UITextInput` geometry (`-firstRectForRange:`, `-caretRectForPosition:`, `-positionFromPosition:offset:`) | iOS 3.2 | ✅ position overlays over a `UITextView` |
| `UIWebView` `contenteditable` + `document.execCommand` | iOS 5 (in practice) | ✅ native editable inline images |
| Color glyph fonts (`sbix`/PUA-emoji-as-font) | iOS 7 (color) | ❌ no color pre-7; can't encode arbitrary **downloaded** PNGs as a static font |

### How this was actually done before iOS 7 (the research)

Inline images mixed with text on iOS 3–6 were universally done with **Core Text
run delegates**, not text attachments. You insert a `U+FFFC` into a
`CFAttributedString`, attach a `CTRunDelegate` (via `kCTRunDelegateAttributeName`)
whose `getWidth`/`getAscent`/`getDescent` callbacks **reserve the image box**,
run `CTFramesetter`/`CTFrameDraw`, then walk `CTLineGetGlyphRuns` + line origins
to find each reserved rect and **draw the image into it yourself**. Every
shipping library of the era did exactly this — **DTCoreText** (Cocoanetics),
**OHAttributedLabel**, **TTTAttributedLabel**, **RTLabel**, **Nimbus
`NIAttributedLabel`**, **CoreTextLabel**. They are all **display-only** (labels):
Core Text gives you a render surface, not a caret/selection/keyboard.

For *editable* inline images (emoticon composers in the WeChat/QQ/Weibo era), the
shipped patterns were:

1. **`U+FFFC` placeholders in a `UITextView` + overlaid `UIImageView`s**,
   repositioned via `UITextInput` geometry on every edit/scroll. One `U+FFFC` per
   sticon ⇒ backspace deletes exactly one sticon (caught in
   `-textView:shouldChangeTextInRange:replacementText:`) and indices map 1:1 to
   `resources`. Width is reserved with spaces tuned to the ~square sticon size
   (imprecise, since pre-7 `UITextView` won't honor a custom advance).
2. **Custom Core Text render view + a hidden first-responder** for the keyboard,
   with an **append/backspace** editing model (no arbitrary mid-string caret).
   Exact width via run delegate; you re-implement any selection you want.
3. **`contenteditable` `UIWebView`** — WebKit reflows text around inline
   `<img>` and handles caret/selection/backspace-deletes-image natively; read the
   model back by DOM-walking in JS.

**Rejected outright:** `NSTextAttachment` (7+); a PUA **emoji font** (no pre-7
color fonts, and sticons are arbitrary per-package **downloaded** PNGs, not a
fixed glyph set).

### Candidate approaches for ENIL (tradeoffs)

| # | Approach | Editing fidelity | Inline precision | Fit to this codebase | Risk |
|---|---|---|---|---|---|
| **A** | `UITextView` + sized `U+FFFC` + overlay `UIImageView`s (`UITextInput` geometry) | Native caret/selection/backspace | Width via tuned spaces (imprecise) | Medium — new overlay-sync code | Overlay drift on scroll/reflow/rotation |
| **B** | `contenteditable` `UIWebView` compose field; model via DOM-walk JS | Native (WebKit) | Exact (WebKit reflow) | **High — app already lives in `UIWebView` + has the sticon HTML/JS pipeline** | Old-WebKit `contenteditable` quirks; keyboard / `inputAccessoryView` plumbing |
| **C** | Core Text render view + `CTRunDelegate` + hidden responder | Append/backspace only | Exact (run delegate) | Low — bespoke text engine | Re-implements caret/selection |

### Recommendation (decision pending — for the user to confirm)

- **Ship 8a first (no inline):** picker (reuse `StickerViewController`,
  sticon-mode) + a **compose tray** of pending sticon thumbnails above a plain
  `UITextField`; Send serializes `(markered text, ordered resources)` →
  `-sendInlineSticons:resources:text:toChatId:`. This delivers the *feature* (the
  optimistic bubble already rides `ENILMessageJSNotification`) while the inline
  rendering is chosen and built. Pure Kaizen.
- **For 8b, lead candidate is B (`contenteditable` `UIWebView`)** on engineering
  merit: it's the only option that gives *exact* inline reflow **and** native
  editing without overlay-sync or a bespoke text engine, and it reuses the app's
  existing web competence and the same sticon image assets. **A is the literal
  answer to "images in a `UITextView`"** and stays native-feeling, at the cost of
  overlay bookkeeping and imprecise width; keep it as the fallback if WebKit
  `contenteditable` proves too quirky on the 4.3–6 floor.

### Backing model (shared by every approach)

Keep an ordered **segment list** — `{ text:"…" } | { sticon: productId,
sticonId, altText }` — as the source of truth. Serialize for send: each `sticon`
→ `(` + `altText` + `)` if alt present else `$`; concatenate text verbatim;
`resources` in segment order. This is the inverse of the receive-side
`REPLACE.sticon.resources` expansion already in the engine, so a future
**round-trip test** (model → markers → engine → rendered HTML) can validate both
directions. M6 already added the sticker button + `messageSendDidTapStickers:`
delegate hook on `MessageSendViewController`; 8 adds a sticon-mode sibling that
appends to this model instead of one-shot sending.
