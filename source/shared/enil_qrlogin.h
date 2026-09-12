#ifndef ENIL_QRLOGIN_H
#define ENIL_QRLOGIN_H

/* ============================================================================
 * QR-code login flow (clean-room port of cloud/client/src/login.js +
 * postlogin.js). Runs the LINE SecondaryQrCodeLogin handshake synchronously:
 *
 *   createSession -> createQrCode -> worker keygen -> (emit QR url) ->
 *   long-poll checkQrCodeVerified -> verifyCertificate
 *     -> [on cert reject] createPinCode -> (emit PIN) -> checkPinCodeVerified
 *   -> qrCodeLoginV2 -> unwrap E2EE keychain -> post-login handshake ->
 *   write session.json
 *
 * This call BLOCKS (the poll steps long-poll for tens of seconds each), so it
 * must run on a background thread — never the UI runloop. Progress and the
 * two user-facing artifacts (QR URL, PIN code) are surfaced through the
 * callback struct; those callbacks fire on the calling (background) thread,
 * so the UI layer must marshal to the main thread itself.
 * ==========================================================================*/

typedef struct {
  /* The QR URL the user scans with the LINE phone app. Fired once, then the
   * flow immediately proceeds to the scan long-poll (non-blocking). */
  void (*on_qr_url)(const char *qr_url, void *ctx);
  /* The 6-digit PIN to confirm in the phone app — only fired when the
   * certificate path is rejected (always on a first-ever login). */
  void (*on_pin)(const char *pin_code, void *ctx);
  /* Optional human-readable progress, e.g. "waiting for scan". May be NULL. */
  void (*on_status)(const char *msg, void *ctx);
  void *ctx;
  /* Optional cooperative-cancel flag. If non-NULL and it becomes non-zero
   * (e.g. the user closed the login window), the flow abandons at the next
   * step / poll boundary and returns 0. The flag is also threaded into the
   * scan and PIN long-poll HTTP requests, so an in-flight connection is torn
   * down within ~1s rather than holding the socket until the server answers. */
  const volatile int *cancel;
} enil_qrlogin_callbacks_t;

/* Runs the full flow. account_dir is the per-account folder; session.json is
 * written as <account_dir>/session.json on success. Returns 1 on success,
 * 0 on any failure (details logged to stderr / surfaced via on_status).
 * If a usable accessToken is already present, returns 1 immediately. */
int enil_qrlogin_run(const char *account_dir,
                     const enil_qrlogin_callbacks_t *cb);

/* Offline E2EE keychain recovery for an already-logged-in account whose
 * capture_e2ee_keys failed but persisted the raw metaData. No network to
 * LINE, no re-login: re-runs only the worker unwrap against the preserved
 * e2eeLoginMetaData. Returns 1 if e2eeKeys are present afterwards (already
 * had them, or recovered now), 0 if a full re-login is unavoidable.
 * Idempotent and a cheap no-op when keys already exist — safe to call on
 * every account open. */
int enil_qrlogin_recover_e2ee(const char *account_dir);

#endif
