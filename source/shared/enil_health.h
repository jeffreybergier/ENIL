#ifndef ENIL_HEALTH_H
#define ENIL_HEALTH_H

/*
 * Sticky failure gate. Two sources can fail independently:
 *   ENIL_ERR_WORKER — the Cloudflare worker rejected us, timed out, etc.
 *   ENIL_ERR_LINE   — the LINE gateway rejected us, timed out, etc.
 *
 * When a source is failed, every subsequent network call in that funnel
 * (worker_post / enil_line_post_ex) short-circuits to NULL without hitting the
 * wire. This protects against pinging LINE for hours with broken credentials
 * (account-lockout risk) and floods the worker with doomed calls.
 *
 * SCOPE — the two sources differ:
 *
 *   WORKER is process-global. There is one worker URL + secret for the whole
 *   app, so "the worker is down" is genuinely app-wide. Worker failures clear
 *   when the user re-saves credentials in Preferences or explicitly starts
 *   another login attempt (including Retry saved login).
 *
 *   LINE is PER-ACCOUNT. Each signed-in account has its own session/token, so
 *   one account's auth/transport failure must not stop another account's
 *   traffic. LINE state lives in a per-account enil_health_t (owned by the
 *   account's ENILAccount). The LINE-source set/clear/is_failed calls below
 *   operate on the health bound to the CALLING THREAD via enil_health_bind().
 *   A thread with no health bound (e.g. the QR-login flow, which runs on its
 *   own thread and does its own error handling) is inert for the LINE source:
 *   is_failed returns 0 and set_failure is a no-op, so a transient login
 *   failure neither blocks nor poisons any logged-in account.
 *
 * Recovery is user-driven — no auto-retry. A LINE failure clears when the user
 * re-logs in via QR, which tears down the dead account's engine (and its
 * enil_health_t) and builds a fresh one — so the new session starts clean with
 * no explicit clear needed.
 *
 * set_failure also posts a sticky red row to SyncMiniViewController via
 * enil_status_post_error so the failure is visible to the user. clear_failure
 * posts a non-error status with the same key, coalescing away the red row.
 */

typedef enum {
  ENIL_ERR_NONE   = 0,
  ENIL_ERR_WORKER = 1,
  ENIL_ERR_LINE   = 2
} enil_err_source_t;

/* Opaque per-account LINE health object. Created/owned by ENILAccount. */
typedef struct enil_health enil_health_t;

/* Create a per-account health object. account_id is copied for logging only
 * (may be NULL). Returns NULL on allocation failure. */
enil_health_t *enil_health_create(const char *account_id);

/* Destroy a health object. If the calling thread currently has it bound, the
 * binding is cleared first to avoid a dangling reference. */
void enil_health_destroy(enil_health_t *h);

/* Bind h as the calling thread's current account health. While bound, the
 * LINE-source set/clear/is_failed calls (and any_failed) act on h. Pass NULL
 * to unbind. Thread-local: affects only the calling thread. Each per-account
 * background thread (sync, token refresh, sends, SSE) binds its own account's
 * health at thread entry. */
void enil_health_bind(enil_health_t *h);

/* Set src to the failed state. WORKER is global; LINE acts on the thread's
 * bound account (no-op if none bound). The UI shows a fixed short label
 * ("Worker Error" / "LINE Error"); user_message is logged for triage but NOT
 * shown in the bottom bar. Safe to call from any thread. */
void enil_health_set_failure(enil_err_source_t src, const char *user_message);

/* Clear src's failed state and post a non-error "Reconnecting" status that
 * supersedes the red row. No-op (no UI churn) if src wasn't failed, or for
 * LINE if no health is bound. Safe to call from any thread. */
void enil_health_clear_failure(enil_err_source_t src);

/* Returns 1 if either the global worker gate or the calling thread's bound
 * account LINE gate is failed. Use from operation entry points that shouldn't
 * start new work while anything is broken (SSE reconnect loop). */
int enil_health_any_failed(void);

/* Returns 1 if the specific source is failed. WORKER is global; LINE reads the
 * thread's bound account (0 if none bound). */
int enil_health_is_failed(enil_err_source_t src);

#endif
