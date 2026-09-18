/* ============================================================================
 * session.json load/save and session_t parser — tokens, workerRestoreState,
 * E2EE keys, and per-account QR login state.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <openssl/rand.h>
#include "enil_session.h"
#include "enil_api_json.h"
#include "enil_cocoa_log.h"

#define LOG(fn, msg) enil_log("Session." fn, "%s", msg)

static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;

char *enil_session_new_id(void) {
  unsigned char bytes[16];
  char value[33];
  const char hex[] = "0123456789abcdef";
  int i;
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) return NULL;
  for (i = 0; i < 16; i++) {
    value[i * 2] = hex[bytes[i] >> 4];
    value[i * 2 + 1] = hex[bytes[i] & 15];
  }
  value[32] = 0;
  return strdup(value);
}

static char *add_login_id(cJSON *root) {
  char *id;
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "loginId");
  if (item) return cJSON_IsString(item) && item->valuestring[0]
    ? strdup(item->valuestring) : NULL;
  if (!cJSON_IsObject(root)) return NULL;
  id = enil_session_new_id();
  if (id && !cJSON_AddStringToObject(root, "loginId", id)) {
    free(id);
    id = NULL;
  }
  return id;
}

char *enil_session_login_id(const char *path) {
  cJSON *root;
  char *id = NULL;
  int existed;
  if (!path) return NULL;
  pthread_mutex_lock(&session_mutex);
  root = enil_session_read(path);
  existed = cJSON_HasObjectItem(root, "loginId");
  id = add_login_id(root);
  if (id && !existed && !enil_session_write(path, root)) {
    free(id);
    id = NULL;
  }
  cJSON_Delete(root);
  pthread_mutex_unlock(&session_mutex);
  return id;
}

int enil_session_retire_file(const char *path) {
  char *retired;
  int fd, ok;
  if (!path) return 0;
  if (access(path, F_OK) != 0) return errno == ENOENT;
  retired = malloc(strlen(path) + sizeof(".retired.XXXXXX"));
  if (!retired) return 0;
  sprintf(retired, "%s.retired.XXXXXX", path);
  fd = mkstemp(retired);
  if (fd < 0) { free(retired); return 0; }
  close(fd);
  ok = rename(path, retired) == 0;
  if (!ok) unlink(retired);
  free(retired);
  return ok;
}

/* ============================================================================
 * Read and parse session.json from disk. Returns owned cJSON or NULL.
 * ==========================================================================*/
cJSON *enil_session_read(const char *path) {
  FILE *f;
  long sz;
  size_t file_size;
  size_t bytes_read;
  char *buf;
  cJSON *root;
  if (!path) { LOG("read", "NULL path"); return NULL; }
  f = fopen(path, "rb");
  if (!f) { LOG("read", "cannot open file"); return NULL; }
  if (fseek(f, 0, SEEK_END) != 0) {
    LOG("read", "cannot seek to end of file");
    fclose(f);
    return NULL;
  }
  sz = ftell(f);
  if (sz < 0) {
    LOG("read", "cannot determine file size");
    fclose(f);
    return NULL;
  }
  file_size = (size_t)sz;
  if (file_size > ((size_t)-1) - 1U) {
    LOG("read", "file is too large");
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    LOG("read", "cannot seek to start of file");
    fclose(f);
    return NULL;
  }
  buf = (char *)malloc(file_size + 1U);
  if (!buf) { fclose(f); return NULL; }
  bytes_read = fread(buf, 1, file_size, f);
  fclose(f);
  if (bytes_read != file_size) {
    LOG("read", "cannot read complete file");
    free(buf);
    return NULL;
  }
  buf[file_size] = '\0';
  root = cJSON_Parse(buf);
  free(buf);
  if (!root) { LOG("read", "invalid JSON"); return NULL; }
  return root;
}

static pthread_key_t bound_path_key;
static pthread_once_t bound_path_once = PTHREAD_ONCE_INIT;
static int bound_path_ready;
static void make_bound_path_key(void) { bound_path_ready = pthread_key_create(&bound_path_key, free) == 0; }
static int bind_path(const char *path) {
  char *copy = path ? strdup(path) : NULL; void *old;
  pthread_once(&bound_path_once, make_bound_path_key);
  if (!bound_path_ready) { free(copy); return 0; }
  old = pthread_getspecific(bound_path_key);
  if (pthread_setspecific(bound_path_key, copy)) { free(copy); return 0; }
  free(old); return !path || copy != NULL;
}
static const char *bound_path(void) {
  pthread_once(&bound_path_once, make_bound_path_key);
  return bound_path_ready ? pthread_getspecific(bound_path_key) : NULL;
}

/* Load a snapshot before the calling account operation touches the network. */
int enil_session_bind_identity(const char *path) {
  cJSON *root;
  enil_identity_t identity;
  int ok;
  if (!bind_path(NULL) || !enil_identity_bind(NULL)) return 0;
  root = enil_session_read(path);
  if (!cJSON_IsObject(root)) { cJSON_Delete(root); return 0; }
  ok = enil_identity_parse(cJSON_GetObjectItemCaseSensitive(root, "clientIdentity"),
                           &identity);
  cJSON_Delete(root);
  return ok && enil_identity_bind(&identity) && bind_path(path);
}

int enil_session_prepare_login(const char *path, const char *profile_id,
                                const char *reauth_session_path) {
  enil_identity_t identity;
  cJSON *root, *json;
  int ok;
  /* Staging is fresh. Refuse to overwrite QR state or an authenticated token. */
  if (!path || access(path, F_OK) == 0) return 0;
  if (reauth_session_path) {
    root = enil_session_read(reauth_session_path);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return 0; }
    ok = enil_identity_parse(cJSON_GetObjectItemCaseSensitive(root, "clientIdentity"),
                             &identity);
    cJSON_Delete(root);
  } else {
    ok = enil_identity_default(profile_id, &identity);
  }
  if (!ok) return 0;
  json = enil_identity_to_json(&identity);
  root = cJSON_CreateObject();
  if (!json || !root) { cJSON_Delete(json); cJSON_Delete(root); return 0; }
  cJSON_AddItemToObject(root, "clientIdentity", json);
  {
    char *id = add_login_id(root);
    ok = id && enil_session_write(path, root);
    free(id);
  }
  cJSON_Delete(root);
  return ok;
}

/* Load a session and return malloc'd accessToken (and optional mid). */
int enil_session_validate(const char *path, char **access_token_out, char **mid_out) {
  session_t s;
  if (!path || !access_token_out) { LOG("validate", "NULL argument"); return 0; }
  *access_token_out = NULL;
  if (mid_out) *mid_out = NULL;
  if (!enil_session_load(path, &s)) return 0;
  if (!s.accessToken || !s.accessToken[0]) {
    LOG("validate", "missing or empty accessToken");
    enil_session_free(&s);
    return 0;
  }
  *access_token_out = s.accessToken; s.accessToken = NULL;
  if (mid_out && s.mid && s.mid[0]) { *mid_out = s.mid; s.mid = NULL; }
  enil_session_free(&s);
  return 1;
}

/* ============================================================================
 * Serialise root to pretty-printed JSON and overwrite session.json.
 * ==========================================================================*/
int enil_session_write(const char *path, cJSON *root) {
  char *text, *tmp;
  size_t plen, tlen;
  FILE *f;
  int fd;
  if (!path || !root) { LOG("write", "NULL argument"); return 0; }
  text = cJSON_Print(root);
  if (!text) { LOG("write", "cJSON_Print failed"); return 0; }
  tlen = strlen(text);
  plen = strlen(path);
  tmp = (char *)malloc(plen + 12);
  if (!tmp) { LOG("write", "alloc failed"); free(text); return 0; }
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmp.XXXXXX", 12);
  fd = mkstemp(tmp);
  if (fd < 0) { free(tmp); free(text); return 0; }
  f = fdopen(fd, "wb");
  if (!f) { close(fd); unlink(tmp); free(tmp); free(text); return 0; }
  if (fwrite(text, 1, tlen, f) != tlen || fflush(f) != 0 || fsync(fileno(f)) != 0) {
    LOG("write", "write failed");
    fclose(f); unlink(tmp); free(tmp); free(text); return 0;
  }
  if (fclose(f) != 0) { unlink(tmp); free(tmp); free(text); return 0; }
  if (rename(tmp, path) != 0) {
    LOG("write", "rename failed");
    unlink(tmp);
    free(tmp); free(text);
    return 0;
  }
  free(tmp);
  free(text);
  return 1;
}

/* ============================================================================
 * Returns the absolute epoch second at which the access token should be
 * refreshed (issued + duration - 30s safety), or 0 if fields are unset.
 * ==========================================================================*/
long long enil_session_get_token_refresh_eta(const char *path) {
  session_t s;
  long long issued, duration, eta;
  if (!path) return 0;
  if (!enil_session_load(path, &s)) return 0;
  issued   = s.tokenIssueTimeEpochSec;
  duration = s.durationUntilRefreshInSec;
  enil_session_free(&s);
  if (issued <= 0 || duration <= 0) return 0;
  eta = issued + duration - 30;
  return eta > 0 ? eta : 0;
}

static double policy_double(cJSON *obj, const char *key, double fallback) {
  cJSON *item = cJSON_GetObjectItem(obj, key);
  if (cJSON_IsNumber(item)) return item->valuedouble;
  if (cJSON_IsString(item) && item->valuestring) return strtod(item->valuestring, NULL);
  return fallback;
}

/* ============================================================================
 * Extract the token-refresh backoff parameters. Falls back to sensible
 * defaults when the policy block is missing. Returns 1 if found in JSON.
 * ==========================================================================*/
int enil_session_get_refresh_retry_policy(const char *path,
                                          long long  *initial_ms_out,
                                          long long  *max_ms_out,
                                          double     *multiplier_out,
                                          double     *jitter_out) {
  session_t s;
  long long initial_ms = 1000, max_ms = 32000;
  double mult = 2.0, jitter = 0.2;
  int present = 0;
  if (!path) return 0;
  if (!enil_session_load(path, &s)) return 0;
  if (cJSON_IsObject(s.refreshApiRetryPolicy)) {
    initial_ms = enil_json_coerce_int64(cJSON_GetObjectItem(s.refreshApiRetryPolicy, "initialDelayInMillis"));
    max_ms     = enil_json_coerce_int64(cJSON_GetObjectItem(s.refreshApiRetryPolicy, "maxDelayInMillis"));
    if (initial_ms <= 0) initial_ms = 1000;
    if (max_ms     <= 0) max_ms     = 32000;
    mult   = policy_double(s.refreshApiRetryPolicy, "multiplier", 2.0);
    jitter = policy_double(s.refreshApiRetryPolicy, "jitterRate", 0.2);
    present = 1;
  }
  enil_session_free(&s);
  if (initial_ms_out) *initial_ms_out = initial_ms;
  if (max_ms_out)     *max_ms_out     = max_ms;
  if (multiplier_out) *multiplier_out = mult;
  if (jitter_out)     *jitter_out     = jitter;
  return present;
}

static char *dup_str_field(cJSON *root, const char *key) {
  cJSON *item = cJSON_GetObjectItem(root, key);
  if (!cJSON_IsString(item) || !item->valuestring) return NULL;
  return strdup(item->valuestring);
}

static void set_str_field(cJSON *root, const char *key, const char *value) {
  cJSON_DeleteItemFromObject(root, key);
  if (value) cJSON_AddStringToObject(root, key, value);
}

static void set_i64_str_field(cJSON *root, const char *key, long long value) {
  char buf[32];
  cJSON_DeleteItemFromObject(root, key);
  snprintf(buf, sizeof(buf), "%lld", value);
  cJSON_AddStringToObject(root, key, buf);
}

static void set_i64_num_field(cJSON *root, const char *key, long long value) {
  cJSON_DeleteItemFromObject(root, key);
  cJSON_AddNumberToObject(root, key, (double)value);
}

static void set_obj_field(cJSON *root, const char *key, cJSON *value) {
  cJSON_DeleteItemFromObject(root, key);
  if (value) cJSON_AddItemToObject(root, key, cJSON_Duplicate(value, 1));
}

/* ============================================================================
 * Populate an owning session_t from a cJSON tree. Every field is copied.
 * Returns 1 on success.
 * ==========================================================================*/
int enil_session_parse(cJSON *root, session_t *out) {
  if (!root || !out) { LOG("parse", "NULL argument"); return 0; }
  memset(out, 0, sizeof(*out));
  if (!enil_identity_parse(cJSON_GetObjectItemCaseSensitive(root, "clientIdentity"),
                           &out->clientIdentity)) {
    LOG("parse", "invalid clientIdentity");
    return 0;
  }
  out->accessToken           = dup_str_field(root, "accessToken");
  if (!out->accessToken) { LOG("parse", "missing accessToken"); return 0; }
  out->snapshot = cJSON_Duplicate(root, 1);
  out->refreshToken          = dup_str_field(root, "refreshToken");
  out->refreshJournalId      = dup_str_field(root, "refreshJournalId");
  out->mid                   = dup_str_field(root, "mid");
  out->displayName           = dup_str_field(root, "displayName");
  out->regionCode            = dup_str_field(root, "regionCode");
  out->certificate           = dup_str_field(root, "certificate");
  out->e2eeLoginPublicKey    = dup_str_field(root, "e2eeLoginPublicKey");
  out->e2eeVersion           = dup_str_field(root, "e2eeVersion");
  out->e2eeHashKeyChain      = dup_str_field(root, "e2eeHashKeyChain");
  out->durationUntilRefreshInSec = enil_json_coerce_int64(cJSON_GetObjectItem(root, "durationUntilRefreshInSec"));
  out->tokenIssueTimeEpochSec    = enil_json_coerce_int64(cJSON_GetObjectItem(root, "tokenIssueTimeEpochSec"));
  out->e2eeLatestKeyId           = enil_json_coerce_int64(cJSON_GetObjectItem(root, "e2eeLatestKeyId"));
  out->e2eeSequenceNumber        = enil_json_coerce_int64(cJSON_GetObjectItem(root, "e2eeSequenceNumber"));
  out->reqSeq                    = enil_json_coerce_int64(cJSON_GetObjectItem(root, "reqSeq"));
  out->needsReauth = cJSON_IsTrue(cJSON_GetObjectItem(root, "needsReauth"));
  out->e2eeKeys              = cJSON_GetObjectItem(root, "e2eeKeys");
  out->e2eeLoginMetaData     = cJSON_GetObjectItem(root, "e2eeLoginMetaData");
  out->encryptedAccessTokens = cJSON_GetObjectItem(root, "encryptedAccessTokens");
  out->workerRestoreState    = cJSON_GetObjectItem(root, "workerRestoreState");
  out->refreshApiRetryPolicy = cJSON_GetObjectItem(root, "refreshApiRetryPolicy");
  out->lastPartialFullSyncs  = cJSON_GetObjectItem(root, "lastPartialFullSyncs");
  /* Deep-copy nested cJSON so the session_t owns them once root is freed. */
  out->e2eeKeys              = out->e2eeKeys              ? cJSON_Duplicate(out->e2eeKeys, 1)              : NULL;
  out->e2eeLoginMetaData     = out->e2eeLoginMetaData     ? cJSON_Duplicate(out->e2eeLoginMetaData, 1)     : NULL;
  out->encryptedAccessTokens = out->encryptedAccessTokens ? cJSON_Duplicate(out->encryptedAccessTokens, 1) : NULL;
  out->workerRestoreState    = out->workerRestoreState    ? cJSON_Duplicate(out->workerRestoreState, 1)    : NULL;
  out->refreshApiRetryPolicy = out->refreshApiRetryPolicy ? cJSON_Duplicate(out->refreshApiRetryPolicy, 1) : NULL;
  out->lastPartialFullSyncs  = out->lastPartialFullSyncs  ? cJSON_Duplicate(out->lastPartialFullSyncs, 1)  : NULL;
  return 1;
}

/* ============================================================================
 * Read session.json and produce an owning session_t whose lifetime is
 * independent of any source cJSON tree. Returns 1 on success.
 * ==========================================================================*/
int enil_session_load(const char *path, session_t *out) {
  cJSON *root;
  if (!path || !out) { LOG("load", "NULL argument"); return 0; }
  root = enil_session_read(path);
  if (!root) return 0;
  if (!enil_session_parse(root, out)) { cJSON_Delete(root); return 0; }
  cJSON_Delete(root);
  return 1;
}

/* ============================================================================
 * Release every owned field inside a session_t and zero the struct.
 * ==========================================================================*/
void enil_session_free(session_t *s) {
  if (!s) return;
  cJSON_Delete(s->snapshot);
  free(s->accessToken);
  free(s->refreshToken);
  free(s->refreshJournalId);
  free(s->mid);
  free(s->displayName);
  free(s->regionCode);
  free(s->certificate);
  free(s->e2eeLoginPublicKey);
  free(s->e2eeVersion);
  free(s->e2eeHashKeyChain);
  if (s->e2eeKeys)              cJSON_Delete(s->e2eeKeys);
  if (s->e2eeLoginMetaData)     cJSON_Delete(s->e2eeLoginMetaData);
  if (s->encryptedAccessTokens) cJSON_Delete(s->encryptedAccessTokens);
  if (s->workerRestoreState)    cJSON_Delete(s->workerRestoreState);
  if (s->refreshApiRetryPolicy) cJSON_Delete(s->refreshApiRetryPolicy);
  if (s->lastPartialFullSyncs)  cJSON_Delete(s->lastPartialFullSyncs);
  memset(s, 0, sizeof(*s));
}

/* ============================================================================
 * Serialise a session_t back to the on-disk JSON layout. Caller owns result.
 * ==========================================================================*/
cJSON *enil_session_to_json(const session_t *s) {
  cJSON *root;
  if (!s) { LOG("to_json", "NULL session"); return NULL; }
  root = s->snapshot ? cJSON_Duplicate(s->snapshot, 1) : cJSON_CreateObject();
  if (!root) return NULL;
  {
    cJSON *identity = enil_identity_to_json(&s->clientIdentity);
    if (!identity) { cJSON_Delete(root); return NULL; }
    cJSON_DeleteItemFromObject(root, "clientIdentity");
    cJSON_AddItemToObject(root, "clientIdentity", identity);
  }
  set_str_field(root, "certificate",         s->certificate);
  set_obj_field(root, "e2eeKeys",            s->e2eeKeys);
  set_i64_num_field(root, "e2eeLatestKeyId", s->e2eeLatestKeyId);
  set_obj_field(root, "e2eeLoginMetaData",   s->e2eeLoginMetaData);
  set_str_field(root, "e2eeLoginPublicKey",  s->e2eeLoginPublicKey);
  set_str_field(root, "e2eeVersion",         s->e2eeVersion);
  set_str_field(root, "e2eeHashKeyChain",    s->e2eeHashKeyChain);
  set_str_field(root, "mid",                 s->mid);
  set_str_field(root, "displayName",         s->displayName);
  set_str_field(root, "regionCode",          s->regionCode);
  set_obj_field(root, "encryptedAccessTokens", s->encryptedAccessTokens);
  set_str_field(root, "accessToken",         s->accessToken);
  set_str_field(root, "refreshToken",        s->refreshToken);
  set_str_field(root, "refreshJournalId",    s->refreshJournalId);
  set_i64_str_field(root, "durationUntilRefreshInSec", s->durationUntilRefreshInSec);
  set_i64_str_field(root, "tokenIssueTimeEpochSec",    s->tokenIssueTimeEpochSec);
  set_i64_num_field(root, "reqSeq",            s->reqSeq);
  set_i64_num_field(root, "e2eeSequenceNumber",s->e2eeSequenceNumber);
  set_obj_field(root, "workerRestoreState",  s->workerRestoreState);
  set_obj_field(root, "refreshApiRetryPolicy", s->refreshApiRetryPolicy);
  set_obj_field(root, "lastPartialFullSyncs",  s->lastPartialFullSyncs);
  cJSON_DeleteItemFromObject(root, "needsReauth");
  if (s->needsReauth) cJSON_AddBoolToObject(root, "needsReauth", 1);
  return root;
}

/* ============================================================================
 * Return the persisted lastPartialFullSyncs map as a JSON string suitable for
 * the SSE query parameter. Always returns malloc'd memory; "{}" when absent.
 * ==========================================================================*/
char *enil_session_get_partial_full_syncs_json(const char *path) {
  session_t s;
  char *out;
  if (!path) return strdup("{}");
  if (!enil_session_load(path, &s)) return strdup("{}");
  if (!cJSON_IsObject(s.lastPartialFullSyncs)) {
    enil_session_free(&s);
    return strdup("{}");
  }
  out = cJSON_PrintUnformatted(s.lastPartialFullSyncs);
  enil_session_free(&s);
  return out ? out : strdup("{}");
}

/* ============================================================================
 * Merge target_categories (from a partialFullSync event) into the persisted
 * lastPartialFullSyncs map, keeping the larger numeric timestamp per key.
 * Returns 1 if any category advanced (caller should trigger a sync), 0 if
 * every incoming timestamp was <= the persisted one. Persists session.json
 * when a change is made.
 * ==========================================================================*/
int enil_session_update_partial_full_syncs(const char *path,
                                           cJSON      *target_categories) {
  session_t s;
  cJSON    *child;
  int       changed = 0;
  if (!path || !cJSON_IsObject(target_categories)) return 0;
  if (!enil_session_load(path, &s)) return 0;
  if (!cJSON_IsObject(s.lastPartialFullSyncs)) {
    if (s.lastPartialFullSyncs) cJSON_Delete(s.lastPartialFullSyncs);
    s.lastPartialFullSyncs = cJSON_CreateObject();
  }
  if (!s.lastPartialFullSyncs) { enil_session_free(&s); return 0; }

  for (child = target_categories->child; child; child = child->next) {
    long long incoming, existing;
    cJSON    *cur;
    char      buf[32];
    if (!child->string) continue;
    incoming = enil_json_coerce_int64(child);
    cur      = cJSON_GetObjectItem(s.lastPartialFullSyncs, child->string);
    existing = cur ? enil_json_coerce_int64(cur) : 0;
    if (incoming <= existing) continue;
    snprintf(buf, sizeof(buf), "%lld", incoming);
    cJSON_DeleteItemFromObject(s.lastPartialFullSyncs, child->string);
    cJSON_AddStringToObject(s.lastPartialFullSyncs, child->string, buf);
    changed = 1;
  }

  if (!changed) { enil_session_free(&s); return 0; }
  changed = enil_session_save(path, &s);
  enil_session_free(&s);
  return changed;
}

/* ============================================================================
 * Clear the persisted lastPartialFullSyncs map — called when a fullSync event
 * arrives, since a full re-sync supersedes any pending partial syncs.
 * ==========================================================================*/
int enil_session_reset_partial_full_syncs(const char *path) {
  session_t s;
  int ok;
  if (!path) return 0;
  if (!enil_session_load(path, &s)) return 0;
  if (!s.lastPartialFullSyncs ||
      (cJSON_IsObject(s.lastPartialFullSyncs) &&
       s.lastPartialFullSyncs->child == NULL)) {
    enil_session_free(&s);
    return 0;
  }
  if (s.lastPartialFullSyncs) cJSON_Delete(s.lastPartialFullSyncs);
  s.lastPartialFullSyncs = cJSON_CreateObject();
  ok = enil_session_save(path, &s);
  enil_session_free(&s);
  return ok;
}

/* ============================================================================
 * Mark this session as needing reauthentication (SSE talkException 117 /
 * V3_TOKEN_CLIENT_LOGGED_OUT). Survives relaunch; cleared implicitly when
 * reauth replaces session.json wholesale. Returns 1 on success.
 * ==========================================================================*/
int enil_session_set_reauth_needed(const char *path) {
  session_t s;
  int ok;
  if (!path) { LOG("set_reauth_needed", "NULL path"); return 0; }
  if (!enil_session_load(path, &s)) return 0;
  s.needsReauth = 1;
  ok = enil_session_save(path, &s);
  enil_session_free(&s);
  return ok;
}

/* Returns 1 if session.json is flagged needsReauth, 0 otherwise. */
int enil_session_has_reauth_needed(const char *path) {
  session_t s;
  int flag;
  if (!path) { LOG("has_reauth_needed", "NULL path"); return 0; }
  if (!enil_session_load(path, &s)) return 0;
  flag = s.needsReauth;
  enil_session_free(&s);
  return flag;
}

/* SSE preference is read/written directly on the JSON tree (not via session_t)
 * so it touches no struct field, parse, or serializer path. Absent key -> 1. */
int enil_session_get_sse_enabled(const char *path) {
  cJSON *root, *item;
  int enabled = 1;
  if (!path) { LOG("get_sse_enabled", "NULL path"); return 1; }
  root = enil_session_read(path);
  if (!root) return 1;
  item = cJSON_GetObjectItem(root, "sseEnabled");
  if (cJSON_IsBool(item)) enabled = cJSON_IsTrue(item) ? 1 : 0;
  cJSON_Delete(root);
  return enabled;
}

int enil_session_set_sse_enabled(const char *path, int enabled) {
  cJSON *patch;
  int ok;
  if (!path)
    return 0;
  patch = cJSON_CreateObject();
  cJSON_AddBoolToObject(patch, "sseEnabled", enabled ? 1 : 0);
  ok = enil_session_patch(path, patch);
  cJSON_Delete(patch);
  return ok;
}

/* Serialize account updates without replaying stale credentials from a reader.
 * Network operations occur outside this mutex. Only the local merge is locked. */
int enil_session_save(const char *path, const session_t *session) {
  cJSON *next = enil_session_to_json(session), *current, *item, *baseline = NULL;
  session_t original;
  int ok = 0;
  if (!next)
    return 0;
  if (session->snapshot && enil_session_parse(session->snapshot, &original)) {
    baseline = enil_session_to_json(&original);
    enil_session_free(&original);
    if (!baseline) {
      cJSON_Delete(next);
      return 0;
    }
  }
  pthread_mutex_lock(&session_mutex);
  current = enil_session_read(path);
  if (!current)
    current = cJSON_CreateObject();
  for (item = next->child; item; item = item->next) {
    cJSON *old = cJSON_GetObjectItemCaseSensitive(baseline, item->string);
    if (old && cJSON_Compare(old, item, 1))
      continue;
    cJSON_DeleteItemFromObjectCaseSensitive(current, item->string);
    cJSON_AddItemToObject(current, item->string, cJSON_Duplicate(item, 1));
  }
  for (item = baseline ? baseline->child : NULL; item; item = item->next)
    if (!cJSON_HasObjectItem(next, item->string))
      cJSON_DeleteItemFromObjectCaseSensitive(current, item->string);
  if (current)
    ok = enil_session_write(path, current);
  cJSON_Delete(current);
  pthread_mutex_unlock(&session_mutex);
  cJSON_Delete(next);
  cJSON_Delete(baseline);
  return ok;
}
int enil_session_patch(const char *path, cJSON *patch) {
  cJSON *root, *item;
  int ok = 0;
  pthread_mutex_lock(&session_mutex);
  root = enil_session_read(path);
  if (root) {
    for (item = patch ? patch->child : NULL; item; item = item->next) {
      cJSON_DeleteItemFromObjectCaseSensitive(root, item->string);
      cJSON_AddItemToObject(root, item->string, cJSON_Duplicate(item, 1));
    }
    ok = enil_session_write(path, root);
  }
  cJSON_Delete(root);
  pthread_mutex_unlock(&session_mutex);
  return ok;
}

char *enil_session_bound_access_token(void) {
  const char *path = bound_path();
  const enil_identity_t *current = enil_identity_current();
  cJSON *root, *token;
  enil_identity_t saved;
  char *copy = NULL;
  if (!path || !current)
    return NULL;
  root = enil_session_read(path);
  if (enil_identity_parse(cJSON_GetObjectItemCaseSensitive(root, "clientIdentity"), &saved) &&
      !memcmp(current, &saved, sizeof(saved))) {
    token = cJSON_GetObjectItemCaseSensitive(root, "accessToken");
    if (cJSON_IsString(token))
      copy = strdup(token->valuestring);
  }
  cJSON_Delete(root);
  return copy;
}
int enil_session_accept_next_access(const char *previous, const char *next) {
  const char *path = bound_path();
  cJSON *root, *token;
  int ok = 0;
  if (!path || !previous || !next || !*next || strpbrk(next, "\r\n"))
    return 0;
  pthread_mutex_lock(&session_mutex);
  root = enil_session_read(path);
  token = cJSON_GetObjectItemCaseSensitive(root, "accessToken");
  if (cJSON_IsString(token) && !strcmp(token->valuestring, previous)) {
    cJSON_ReplaceItemInObjectCaseSensitive(root, "accessToken", cJSON_CreateString(next));
    ok = enil_session_write(path, root);
  } else if (cJSON_IsString(token))
    ok = 1; /* another request already rotated it */
  cJSON_Delete(root);
  pthread_mutex_unlock(&session_mutex);
  return ok;
}
