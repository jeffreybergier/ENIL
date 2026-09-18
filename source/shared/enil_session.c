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
static int session_write_locked(const char *path, cJSON *root);

/* Flushing file contents does not persist a rename. Flush the containing
 * directory too, and report failure even if the new name is already visible.
 * Use O_RDONLY for the older Apple SDKs, which lack O_DIRECTORY. */
static int sync_parent_directory(const char *path) {
  char *directory = strdup(path), *slash;
  int fd, ok;
  if (!directory) return 0;
  slash = strrchr(directory, '/');
  if (slash) {
    if (slash == directory) slash[1] = '\0';
    else *slash = '\0';
  } else {
    free(directory);
    directory = strdup(".");
    if (!directory) return 0;
  }
  fd = open(directory, O_RDONLY);
  free(directory);
  if (fd < 0) return 0;
  do {
    ok = fsync(fd);
  } while (ok != 0 && errno == EINTR);
  close(fd);
  return ok == 0;
}

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
  if (id && !existed && !session_write_locked(path, root)) {
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
  else ok = sync_parent_directory(path);
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

typedef struct {
  char *path;
  char *login_id;
} SessionBinding;
static pthread_key_t binding_key;
static pthread_once_t binding_once = PTHREAD_ONCE_INIT;
static int binding_ready;
static void free_binding(void *value) {
  SessionBinding *binding = value;
  if (!binding) return;
  free(binding->path);
  free(binding->login_id);
  free(binding);
}
static void make_binding_key(void) {
  binding_ready = pthread_key_create(&binding_key, free_binding) == 0;
}
static SessionBinding *current_binding(void) {
  pthread_once(&binding_once, make_binding_key);
  return binding_ready ? pthread_getspecific(binding_key) : NULL;
}
static int set_binding(SessionBinding *binding) {
  SessionBinding *old = current_binding();
  if (!binding_ready || pthread_setspecific(binding_key, binding)) return 0;
  free_binding(old);
  return 1;
}
static int binding_matches(const SessionBinding *binding, cJSON *root) {
  const enil_identity_t *identity = enil_identity_current();
  enil_identity_t saved;
  cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "loginId");
  if (!binding || !identity || !cJSON_IsObject(root)) return 0;
  if (binding->login_id || id) {
    if (!binding->login_id || !cJSON_IsString(id) ||
        strcmp(binding->login_id, id->valuestring)) return 0;
  }
  return enil_identity_parse(cJSON_GetObjectItemCaseSensitive(root, "clientIdentity"), &saved) &&
         !memcmp(identity, &saved, sizeof(saved));
}

/* Load a snapshot before the calling account operation touches the network. */
int enil_session_bind_identity(const char *path) {
  cJSON *root, *id;
  SessionBinding *binding = NULL;
  enil_identity_t identity;
  int ok;
  if (!set_binding(NULL) || !enil_identity_bind(NULL)) return 0;
  root = enil_session_read(path);
  if (!cJSON_IsObject(root)) { cJSON_Delete(root); return 0; }
  ok = enil_identity_parse(cJSON_GetObjectItemCaseSensitive(root, "clientIdentity"),
                           &identity);
  id = cJSON_GetObjectItemCaseSensitive(root, "loginId");
  if (id && (!cJSON_IsString(id) || !id->valuestring[0])) ok = 0;
  if (ok) {
    binding = calloc(1, sizeof(*binding));
    if (binding) {
      binding->path = strdup(path);
      binding->login_id = id ? strdup(id->valuestring) : NULL;
    }
    ok = binding && binding->path && (!id || binding->login_id);
  }
  cJSON_Delete(root);
  if (ok && enil_identity_bind(&identity) && set_binding(binding)) return 1;
  free_binding(binding);
  enil_identity_bind(NULL);
  return 0;
}

int enil_session_continue_identity(const char *path) {
  SessionBinding *binding = current_binding();
  cJSON *root;
  int ok;
  if (!path) return 0;
  if (!binding) return enil_session_bind_identity(path);
  /* Nested calls must never turn an old operation into a new login. Keep a
   * failed binding too, so subsequent calls cannot silently bind afresh. */
  if (strcmp(binding->path, path)) return 0;
  root = enil_session_read(path);
  ok = binding_matches(binding, root);
  cJSON_Delete(root);
  return ok;
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
static int session_write_locked(const char *path, cJSON *root) {
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
  if (!sync_parent_directory(path)) {
    LOG("write", "directory sync failed; replacement may already be visible");
    return 0;
  }
  return 1;
}

int enil_session_write(const char *path, cJSON *root) {
  int ok;
  pthread_mutex_lock(&session_mutex);
  ok = session_write_locked(path, root);
  pthread_mutex_unlock(&session_mutex);
  return ok;
}

int enil_session_activate(const char *path, cJSON *root) {
  char *journal;
  int ok;
  if (!path || !cJSON_IsObject(root)) return 0;
  journal = malloc(strlen(path) + sizeof(".refresh-pending"));
  if (!journal) return 0;
  sprintf(journal, "%s.refresh-pending", path);
  pthread_mutex_lock(&session_mutex);
  ok = session_write_locked(path, root);
  /* Retire the prior journal before any new-login refresh can write one. */
  if (ok && !enil_session_retire_file(journal))
    LOG("activate", "Could not retire old refresh journal");
  pthread_mutex_unlock(&session_mutex);
  free(journal);
  return ok;
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

/* Monotonic per-category timestamps; never acknowledge work at receipt time. */
static int merge_partial_timestamp(cJSON *map, const char *key, long long value) {
  char text[32];
  cJSON *item;
  if (value <= enil_json_coerce_int64(cJSON_GetObjectItemCaseSensitive(map, key)))
    return 0;
  snprintf(text, sizeof(text), "%lld", value);
  item = cJSON_CreateString(text);
  if (!item) return -1;
  if (cJSON_HasObjectItem(map, key)) {
    if (cJSON_ReplaceItemInObjectCaseSensitive(map, key, item)) return 1;
  } else if (cJSON_AddItemToObject(map, key, item)) return 1;
  cJSON_Delete(item);
  return -1;
}

static cJSON *partial_map(cJSON *root, const char *name) {
  cJSON *map = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!map) map = cJSON_AddObjectToObject(root, name);
  return cJSON_IsObject(map) ? map : NULL;
}

int enil_session_update_partial_full_syncs(const char *path, cJSON *target_categories) {
  cJSON *root, *pending, *completed, *item;
  SessionBinding *binding = current_binding();
  int needed = 0, changed = 0, result = -1;
  if (!path || !cJSON_IsObject(target_categories)) return -1;
  pthread_mutex_lock(&session_mutex);
  root = enil_session_read(path);
  if (!cJSON_IsObject(root) || (binding &&
      (strcmp(binding->path, path) || !binding_matches(binding, root)))) goto done;
  pending = partial_map(root, "pendingPartialFullSyncs");
  completed = partial_map(root, "lastPartialFullSyncs");
  if (!pending || !completed) goto done;
  for (item = target_categories->child; item; item = item->next) {
    long long incoming = enil_json_coerce_int64(item);
    int merged;
    if (!item->string || incoming <= enil_json_coerce_int64(
          cJSON_GetObjectItemCaseSensitive(completed, item->string))) continue;
    needed = 1;
    merged = merge_partial_timestamp(pending, item->string, incoming);
    if (merged < 0) goto done;
    changed |= merged;
  }
  if (changed && !session_write_locked(path, root)) goto done;
  result = needed;
done:
  cJSON_Delete(root);
  pthread_mutex_unlock(&session_mutex);
  return result;
}

int enil_session_complete_partial_full_syncs(const char *path, const cJSON *snapshot) {
  cJSON *root, *pending, *completed, *item, *before, *now;
  cJSON *requests = cJSON_GetObjectItemCaseSensitive(snapshot, "pendingPartialFullSyncs");
  int result = 0;
  if (!path || !cJSON_IsObject(snapshot)) return 0;
  pthread_mutex_lock(&session_mutex);
  root = enil_session_read(path);
  before = cJSON_GetObjectItemCaseSensitive(snapshot, "loginId");
  now = cJSON_GetObjectItemCaseSensitive(root, "loginId");
  if (!cJSON_IsObject(root) || ((before || now) &&
      (!cJSON_IsString(before) || !cJSON_IsString(now) || !cJSON_Compare(before, now, 1))))
    goto done;
  if (!requests || (cJSON_IsObject(requests) && !requests->child)) {
    result = 1;
    goto done;
  }
  if (!cJSON_IsObject(requests)) goto done;
  pending = partial_map(root, "pendingPartialFullSyncs");
  completed = partial_map(root, "lastPartialFullSyncs");
  if (!pending || !completed) goto done;
  for (item = requests->child; item; item = item->next) {
    long long value = enil_json_coerce_int64(item);
    cJSON *queued;
    if (!item->string || merge_partial_timestamp(completed, item->string, value) < 0)
      goto done;
    queued = cJSON_GetObjectItemCaseSensitive(pending, item->string);
    if (queued && enil_json_coerce_int64(queued) <= value)
      cJSON_DeleteItemFromObjectCaseSensitive(pending, item->string);
  }
  if (!pending->child) cJSON_DeleteItemFromObjectCaseSensitive(root, "pendingPartialFullSyncs");
  result = session_write_locked(path, root);
done:
  cJSON_Delete(root);
  pthread_mutex_unlock(&session_mutex);
  return result;
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
  /* A changed login must never inherit a previous login's refresh reply,
   * keys, or sync state. Compare under the same lock used by activation.
   * Legacy snapshots without IDs may only update another ID-less session.
   * Also refuse to recreate an account removed while work was in flight. */
  if (!cJSON_IsObject(current) || !baseline)
    goto done;
  {
    cJSON *before = cJSON_GetObjectItemCaseSensitive(baseline, "loginId");
    cJSON *now = cJSON_GetObjectItemCaseSensitive(current, "loginId");
    if ((before || now) &&
        (!cJSON_IsString(before) || !before->valuestring[0] ||
         !cJSON_IsString(now) || !cJSON_Compare(before, now, 1))) {
      LOG("save", "discarding update from a superseded login");
      goto done;
    }
  }
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
    ok = session_write_locked(path, current);
done:
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
    ok = session_write_locked(path, root);
  }
  cJSON_Delete(root);
  pthread_mutex_unlock(&session_mutex);
  return ok;
}

int enil_session_bound_access_token(char **out) {
  SessionBinding *binding = current_binding();
  cJSON *root, *token;
  *out = NULL;
  if (!binding) return 0;
  root = enil_session_read(binding->path);
  if (binding_matches(binding, root)) {
    token = cJSON_GetObjectItemCaseSensitive(root, "accessToken");
    if (cJSON_IsString(token) && token->valuestring[0])
      *out = strdup(token->valuestring);
  }
  cJSON_Delete(root);
  return *out ? 1 : -1;
}
int enil_session_accept_next_access(const char *previous, const char *next) {
  SessionBinding *binding = current_binding();
  cJSON *root, *token;
  int ok = 0;
  if (!binding || !previous || !next || !*next || strpbrk(next, "\r\n"))
    return 0;
  pthread_mutex_lock(&session_mutex);
  root = enil_session_read(binding->path);
  if (!binding_matches(binding, root)) goto done;
  token = cJSON_GetObjectItemCaseSensitive(root, "accessToken");
  if (cJSON_IsString(token) && !strcmp(token->valuestring, previous)) {
    cJSON_ReplaceItemInObjectCaseSensitive(root, "accessToken", cJSON_CreateString(next));
    ok = session_write_locked(binding->path, root);
  } else if (cJSON_IsString(token))
    ok = 1; /* another request already rotated it */
done:
  cJSON_Delete(root);
  pthread_mutex_unlock(&session_mutex);
  return ok;
}
