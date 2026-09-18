#ifndef ENIL_SESSION_H
#define ENIL_SESSION_H

#include "cJSON.h"
#include "enil_api_types.h"

/* Parses an existing cJSON tree into an owning session_t. All fields are
 * copied; release with enil_session_free. Returns 1 on success. */
int  enil_session_parse(cJSON *root, session_t *out);

/* Loads session.json from disk, parses, and copies/dups all fields so the
 * resulting session_t outlives the JSON tree. Returns 1 on success. */
int  enil_session_load(const char *path, session_t *out);

/* Frees all owned memory in *s. Safe on a zero-initialised session_t. */
void enil_session_free(session_t *s);

/* Serialises *s to a fresh cJSON object (owned by caller — cJSON_Delete it).
 * Nested cJSON fields are deep-duplicated so the result is independent of *s. */
cJSON *enil_session_to_json(const session_t *s);

/* Reads and parses a JSON file from disk. Returns a new cJSON root (caller
 * must cJSON_Delete), or NULL on failure. */
cJSON *enil_session_read(const char *path);

/* Serialises root and atomically replaces path under the session write lock.
 * Returns 1 on success. */
int enil_session_write(const char *path, cJSON *root);
/* Activate a login and retire the prior refresh journal under that same lock. */
int enil_session_activate(const char *path, cJSON *root);
/* Merge changes relative to the loaded snapshot, preserving concurrent token
 * rotation and unknown fields. Rejects superseded logins and missing accounts.
 * Atomic, private, fsynced replacement; requires a loaded/parsed snapshot. */
int enil_session_save(const char *path, const session_t *session);
int enil_session_patch(const char *path, cJSON *patch);
/* A stable identifier for one login, unchanged by token rotation. Ensures it
 * exists on legacy sessions too. Caller frees the returned string. */
char *enil_session_login_id(const char *path);
char *enil_session_new_id(void);
/* Preserve an obsolete recovery file under a unique .retired.* name. */
int enil_session_retire_file(const char *path);

/* Bind the saved identity for the calling account operation. Clears any old
 * binding on failure. Also works for a staged session without an access token. */
int enil_session_bind_identity(const char *path);
/* Native APIs reload rotating tokens from the bound account. */
char *enil_session_bound_access_token(void);
int enil_session_accept_next_access(const char *previous, const char *next);
/* Seed a fresh QR session. Reauthentication copies only the old identity, not
 * credentials/QR keys. Never overwrites an existing staged session. */
int enil_session_prepare_login(const char *path, const char *profile_id,
                                const char *reauth_session_path);

/* Validates session.json and extracts required fields.
 * Returns 1 on success; *access_token_out is malloc'd, *mid_out is malloc'd or NULL.
 * Returns 0 on any failure. Caller must free non-NULL outputs. */
int enil_session_validate(const char *path, char **access_token_out, char **mid_out);

int       enil_session_set_reauth_needed(const char *path);
int       enil_session_has_reauth_needed(const char *path);

/* SSE auto-connect preference. A missing key defaults to enabled (1) so every
 * pre-existing session keeps its always-on behavior. set returns 1 on success. */
int       enil_session_get_sse_enabled(const char *path);
int       enil_session_set_sse_enabled(const char *path, int enabled);

/* Returns the persisted lastPartialFullSyncs map as a JSON string suitable
 * for the SSE query parameter. Always malloc'd; "{}" if absent. */
char *enil_session_get_partial_full_syncs_json(const char *path);

/* Merge target_categories (from a partialFullSync SSE event) into the
 * persisted map. Returns 1 if any category timestamp advanced — caller
 * should kick off a re-sync. Persists session.json if changed. */
int enil_session_update_partial_full_syncs(const char *path,
                                           cJSON      *target_categories);

/* Clear the persisted map (called on fullSync). Returns 1 if a change
 * was actually written. */
int enil_session_reset_partial_full_syncs(const char *path);

/* Returns the wall-clock epoch second at which the access token should be
 * proactively refreshed (= tokenIssueTimeEpochSec + durationUntilRefreshInSec
 * minus a 30s safety margin). Returns 0 when the fields are absent — the
 * caller should treat that as "refresh immediately on next opportunity". */
long long enil_session_get_token_refresh_eta(const char *path);

/* Reads refreshApiRetryPolicy.* fields. Any output pointer may be NULL.
 * Returns 1 on success (object present), 0 otherwise. Defaults are filled in
 * when the policy is missing: 1s / 32s / 2.0x / 0.2 jitter. */
int enil_session_get_refresh_retry_policy(const char *path,
                                          long long  *initial_ms_out,
                                          long long  *max_ms_out,
                                          double     *multiplier_out,
                                          double     *jitter_out);

#endif
