# Security & Memory Safety Review — `enil_line.c`

**File:** `source/shared/enil_line.c` (1762 lines)  
**Date:** 2026-05-16

---

## Overall Assessment

No critical memory-safety vulnerabilities (buffer overflows, use-after-free in active code paths, format-string bugs). The code follows consistent `goto done` cleanup, uses `snprintf` for bounded writes, and NULL-checks all public API parameters. The findings below range from medium-robustness to informational.

---

## Finding 1 — Dangling `out->message` on partial failure in `overlay_send_result` (Medium)

**Lines 526–541:**

```c
out->message    = local;                          // ownership transferred
out->message_id = strdup(local->id);               // may fail → NULL
out->created_at = local->createdTime;
return out->message_id ? 1 : 0;                    // return 0 on strdup fail
```

If `strdup` for `out->message_id` fails, `out->message` is already set to `local`, but the function returns 0. The outer caller (`send_1on1` / `send_group`) sees failure and goes to `done:`. The *top-level* caller (e.g., `enil_line_send_text`) then frees `local`:

```c
ok = send_1on1(...);
if (ok) local = NULL;          // ok==0, so local is still set
done:
  if (local) { talk_message_free(local); free(local); }
```

`out->message` now dangles. Currently safe because all callers check the return value and never touch `*out` on failure — but any future caller that inspects `out->message` after a 0-return would hit a use-after-free.

**Fix:** Move `out->message = local` to after `out->message_id` succeeds, or clear `out->message` on the failure path.

---

## Finding 2 — `session_apply_token_v3` loses refreshToken on alloc failure (Low)

**Lines 136–156:**

```c
if (t->refreshToken) {
  free(session->refreshToken);          // old token destroyed
  session->refreshToken = strdup(t->refreshToken);  // may be NULL
}
```

If `strdup` fails, the old valid `refreshToken` is gone and the field is NULL. The next token refresh will fail permanently with "missing refreshToken" (line 187), making the session unusable until re-login. The old value should be preserved on allocation failure.

**Fix:** Swap pattern — `strdup` into a temp, check it, then `free(old)`.

---

## Finding 3 — `enil_line_token_refresh` persists session before final alloc check (Low)

**Lines 225–228:**

```c
new_access = strdup(parsed.accessToken);              // may fail → NULL
session_apply_token_v3(&session, &parsed);             // already updated session fields
session_persist(session_path, &session);               // writes to disk
```

If `strdup(parsed.accessToken)` on line 225 fails, `new_access` is NULL but `session_apply_token_v3` already set `session.accessToken = dup_or_null(t->accessToken)` (its own internal strdup), and `session_persist` wrote the new state to disk. The caller gets NULL and thinks the refresh failed, but the session file now contains the new tokens. On next read the tokens will work, but the caller's error-handling code may take a different (wrong) path.

**Fix:** Check `new_access` before `session_apply_token_v3`, or restructure so `session_apply_token_v3` doesn't run until all allocations succeed.

---

## Finding 4 — Integer overflow in `b64_decode_alloc` on 32-bit (Low, theoretical)

**Lines 378–402:**

```c
out_len = in_len / 4 * 3;          // can overflow 32-bit size_t
buf = (unsigned char *)malloc(out_len + 1);
```

On 32-bit targets (PPC, x86), if `in_len ≥ 0x55555558` (≈1.43 GB base64 input), `out_len` wraps to a small value, `malloc` returns a tiny buffer, and the decode loop writes past it. In practice this data comes from worker crypto responses (a few hundred bytes), so it's not exploitable — but worth hardening.

**Fix:** Check `in_len > (SIZE_MAX / 3) * 4` before computing `out_len`.

---

## Finding 5 — `next_req_seq` negative on `time()` error (Low)

**Lines 347–350:**

```c
long long cur = (session && session->reqSeq) ? session->reqSeq
  : ((long long)time(NULL) % 2147483648LL) - 1;
return (cur + 1) % 2147483648LL;
```

If `time(NULL)` returns `(time_t)-1` (clock unavailable), C99 `%` with negative left operand yields a negative value, so `cur` can be negative, and `(cur + 1) % 2147483648LL` stays negative. The LINE API receives a negative `reqSeq`. Harmless in practice (server will reject it), but sloppy.

**Fix:** Guard `time()` return with `if (t == (time_t)-1) t = 0`.

---

## Finding 6 — `make_header` has no NULL-guard (Informational)

**Lines 40–46:**

```c
static char *make_header(const char *name, const char *value) {
  size_t len = strlen(name) + 2 + strlen(value) + 1;
```

Both `name` and `value` are dereferenced by `strlen` with no NULL check. All callers pass string literals or already-checked malloc'd strings, so this is safe — but adding `if (!name || !value) return NULL;` would be defensive.

---

## Finding 7 — `build_sticon_resources` overlapping alt-text bug (Informational)

**Lines 1640–1701:**

The scanner walks the text left-to-right looking for each sticon's marker. If one sticon's alt text is a prefix of another's (e.g., `"(hi)"` and `"(hi there)"`), the shorter marker matches inside the longer one, assigning wrong byte offsets. This is a logic bug, not a memory-safety issue. It only manifests when two sticon resources in the same message have conflicting alt texts — likely rare in practice.

---

## What's Done Well

- **Consistent `goto done` cleanup** in every non-trivial function — all allocations are freed on every path.
- **`snprintf` everywhere** for buffer writes — no `sprintf` or `strcpy` into fixed buffers.
- **NULL-arg validation** at every public API entry point (`enil_line_post`, `enil_line_send_text`, etc.).
- **Ownership clarity**: `talk_message_free` + `free()` pattern is consistent; `ENILLineSendResult` ownership transfers are documented in the header.
- **No use of unsafe functions** (`gets`, `strcat`, unbounded `scanf`, etc.).
- **Format strings are literals** — no user-controlled format strings.
