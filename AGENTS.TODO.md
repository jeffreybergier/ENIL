# ENIL — Outstanding Work

Checked against the current source on 2026-09-26. These are the remaining
tasks in this file; partial implementations are called out below.

- [ ] **Handle SSE `talkException` code 89 as LSOFF, if observed.** The SSE
      path already records exceptions, stops the stream, marks code 117 for
      reauthentication, refreshes/reconnects on 119 or 10051, treats code 8
      as sync-needed, and surfaces other codes as an error. Code 89 currently
      takes that generic error path. Confirm its meaning from a fixture or an
      observed `talk_exceptions` row before adding account LSOFF state and
      recovery behavior. QR-login detection is a separate task below.

- [ ] **Refresh read ranges when a chat opens.** Full sync already calls
      `talk_get_message_read_range` in batches and applies the result to sqlite;
      SSE op 55 also updates read state. Add a one-chat call on chat-open for
      both platforms, apply its result to sqlite, and refresh the visible
      receipt markers. No chat-open call exists today.

- [ ] **Send and cancel reactions.** Incoming reaction ops 139/140 already
      update sqlite and the open message view. Add the `TalkService/react` and
      `TalkService/cancelReaction` RPCs, account methods, and a bubble control
      that invokes them. Neither platform currently exposes a reaction control.

- [ ] **Resolve LINE Official Account profiles when needed.** No
      `BuddyService/getBuddyProfile` call exists. Messages fall back to the
      sender MID when no contact name is stored, so an unresolved Official
      Account can display as a bare MID. Add the lookup and picture handling
      when this affects a real workflow; no observed case is recorded here.

- [ ] **Detect no-Letter-Sealing accounts during QR login.** Neither the
      Chrome-gateway nor native QR-login path identifies an LSOFF result or
      persists that account capability. Confirm the login response/error shape
      (historically described here as `code:89`), then detect it in the shared
      engine and present a clear result in both UIs. SSE code 89 is tracked
      separately above.

- [ ] **Validate OBS token response parsing.** `enil_line_acquire_obs_token`
      already splits the first `\x1e` row separator and stops the token at
      `\x1f` or the next row. Add a focused response fixture for this format
      and compare it with a permitted captured response if available. Existing
      tests mock or reject the call; they do not exercise a successful parse.

- [ ] **Finish targeted SSE UI updates.** `ENILAccount` already buffers SSE UI
      work with a resetting 1-second debounce (8-second maximum) and combines
      incremental message JavaScript once per chat. For ordinary send/receive
      events, both chat lists update one row instead of rebuilding the roster.
      The buffered raw `ENILSSEEventNotification`s are still replayed one by
      one, however, and no targeted chat/contact/reaction notification API
      exists. Carry affected entities out of SSE processing (including contact
      op 49 and chat ops 121/122), coalesce duplicate UI invalidations within
      a batch, and refresh visible chat metadata/header when those ops arrive.
      Keep per-message events needed by user notifications distinct from UI
      invalidation, and let each platform subscribe only to affected data.

- [ ] **URL previews (link unfurl).** No page-info request, stored preview
      metadata, or message-card renderer exists. The earlier research points
      to LINE's `pageinfo/get` endpoint; verify its current request/response
      format and client-identity requirements before implementing it. Fetch
      metadata and any thumbnail in native code, store the result under the
      account, and render from local data so the WebView makes no network
      request. Preview failures must leave the message and bare URL usable.

- [ ] **Reconnect SSE promptly after macOS wake.** `ENILAccount` exposes
      `forceReconnectSSE`, but the macOS app does not observe
      `NSWorkspaceDidWakeNotification` or call it. Add the observer in the
      AppKit app and reconnect each active account after wake.

- [ ] **Show actual sticon images in the iOS compose field.** The current
      `UITextView` uses marker characters and `sticonRuns` to send inline
      sticons but does not display their images while editing. An editable
      `UIWebView` is a candidate; check its behavior on the iOS 4.3 floor.
