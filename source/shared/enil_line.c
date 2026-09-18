/* ============================================================================
 * LINE auth & request plumbing — tokenRefresh, X-Line-Access/X-Hmac headers,
 * and the low-level ENILLineRequest/Response transport used by TalkService.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include "cJSON.h"
#include "enil_b64.h"
#include "enil_http.h"
#include "enil_worker.h"
#include "enil_line.h"
#include "enil_native.h"
#include "enil_session.h"
#include "enil_obs.h"
#include "enil_api_json.h"
#include "enil_crypto.h"
#include "enil_talkserv.h"
#include "enil_cocoa_log.h"
#include "enil_health.h"

/* Thin wrapper: routes fixed-string call sites through NSLog with a
 * "Line.<fn>" tag. Formatted sites should call ENIL_LOG directly. */
#define LOG(fn, msg) enil_log("Line." fn, "%s", msg)

/* Pre-formatted language headers — written once by enil_line_set_language()
 * from AppDelegate at launch, read by every request. Defaults match the
 * original hard-coded values so the C side still works if no one ever calls
 * the setter (e.g. from a non-Cocoa client). */
static char accept_lang_hdr[64] = "Accept-Language: en-US";
static char x_lal_hdr[32]       = "X-LAL: en_US";

void enil_line_set_language(const char *accept_lang, const char *x_lal) {
  if (accept_lang && *accept_lang)
    snprintf(accept_lang_hdr, sizeof(accept_lang_hdr),
             "Accept-Language: %s", accept_lang);
  if (x_lal && *x_lal)
    snprintf(x_lal_hdr, sizeof(x_lal_hdr), "X-LAL: %s", x_lal);
  ENIL_LOG("Line.set_language", "%s | %s", accept_lang_hdr, x_lal_hdr);
}

static char *make_header(const char *name, const char *value) {
  size_t len = strlen(name) + 2 + strlen(value) + 1;
  char *h = malloc(len);
  if (h) snprintf(h, len, "%s: %s", name, value);
  return h;
}

/* ============================================================================
 * Release the response body buffer and reset the status code.
 * ==========================================================================*/
void enil_line_response_free(ENILLineResponse *r) {
  if (!r) return;
  free(r->body);
  r->body   = NULL;
  r->status = 0;
}

/* ============================================================================
 * Signed POST against the LINE chrome gateway — requests an HMAC from the
 * worker, attaches X-Line-Access/X-Hmac, and returns the raw response.
 * ==========================================================================*/
ENILLineResponse enil_line_post(
  const char *path,
  const char *body,
  const char *access_token)
{
  return enil_line_post_ex(path, body, access_token, NULL, 0, 0, NULL);
}

/* libcurl transfer-info callback: returning non-zero aborts the transfer with
 * CURLE_ABORTED_BY_CALLBACK. libcurl invokes this roughly once per second even
 * while a long-poll sits idle waiting on the server, so a cancel flipped by the
 * UI (closed QR window) tears the connection down within ~1s. clientp is the
 * caller's cancel flag. */
static int cancel_xferinfo(void *clientp,
                           curl_off_t dltotal, curl_off_t dlnow,
                           curl_off_t ultotal, curl_off_t ulnow)
{
  const volatile int *cancel = (const volatile int *)clientp;
  (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
  return (cancel && *cancel) ? 1 : 0;
}

ENILLineResponse enil_line_post_ex(
  const char *path,
  const char *body,
  const char *access_token,
  const char *const *extra_headers,
  int                extra_header_count,
  long               timeout_ms,
  const volatile int *cancel)
{
  ENILLineResponse result = {0, NULL};
  char url[512];
  char *hmac, *hmac_hdr, *access_hdr, *cookie_hdr = NULL;
  ENILBuf buf = {NULL, 0};
  CURL *curl;
  struct curl_slist *hdrs = NULL;
  CURLcode rc;
  int i;
  const enil_identity_t *identity = enil_identity_current();
  char application_header[192], version_header[64];

  if (!identity) { LOG("post", "no client identity bound"); return result; }
  if (!path || !body || !access_token) { LOG("post", "NULL argument"); return result; }

  /* Sticky-failure gate, scoped to the calling thread's bound account (see
   * enil_health.h). Once this account's LINE gateway has rejected us (auth,
   * transport, or 5xx), its subsequent LINE calls short-circuit — without
   * affecting any other account. A thread with no account bound (the QR-login
   * flow) is inert here: is_failed returns 0, so the call proceeds, and the
   * failure branches below likewise no-op. The gate clears when the user
   * re-logs in via QR (which rebuilds the account's health). This protects
   * against pinging LINE for hours with broken auth. */
  if (enil_health_is_failed(ENIL_ERR_LINE)) {
    ENIL_LOG("Line.post",
             "skipping %s — LINE is in failed state", path);
    return result;
  }

  if (strcmp(identity->transport, "native-thrift") == 0)
    return enil_native_post(path, body, access_token, timeout_ms, cancel);

  hmac = enil_worker_sign(path, body, access_token);
  if (!hmac) { LOG("post", "sign failed"); return result; }

  snprintf(url, sizeof(url), "%s%s", ENIL_LINE_GATEWAY, path);
  curl = enil_curl_new(&buf);
  if (!curl) { free(hmac); LOG("post", "enil_curl_new failed"); return result; }

  hdrs = curl_slist_append(hdrs, "Accept: application/json, text/plain, */*");
  hdrs = curl_slist_append(hdrs, accept_lang_hdr);
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  snprintf(version_header, sizeof(version_header), "X-Line-Chrome-Version: %s",
           identity->gateway_version);
  snprintf(application_header, sizeof(application_header), "X-Line-Application: %s",
           identity->application);
  hdrs = curl_slist_append(hdrs, version_header);
  hdrs = curl_slist_append(hdrs, application_header);
  hdrs = curl_slist_append(hdrs, x_lal_hdr);
  hdrs = curl_slist_append(hdrs, "Origin: " ENIL_LINE_ORIGIN);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, identity->user_agent);

  hmac_hdr   = make_header("X-Hmac", hmac);
  access_hdr = access_token[0] ? make_header("X-Line-Access", access_token) : NULL;
  free(hmac);

  if (access_token[0]) {
    size_t n = strlen("Cookie: lct=") + strlen(access_token) + 1;
    cookie_hdr = (char *)malloc(n);
    if (cookie_hdr) snprintf(cookie_hdr, n, "Cookie: lct=%s", access_token);
  }

  if (hmac_hdr)   hdrs = curl_slist_append(hdrs, hmac_hdr);
  if (access_hdr) hdrs = curl_slist_append(hdrs, access_hdr);
  if (cookie_hdr) hdrs = curl_slist_append(hdrs, cookie_hdr);

  for (i = 0; i < extra_header_count; i++) {
    if (extra_headers && extra_headers[i])
      hdrs = curl_slist_append(hdrs, extra_headers[i]);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  if (timeout_ms > 0)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  if (cancel) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
  }

  ENIL_LOG("Line.post", "POST %s (%lu bytes)",
           path, (unsigned long)strlen(body));

  rc = curl_easy_perform(curl);
  if (rc == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  free(hmac_hdr);
  free(access_hdr);
  free(cookie_hdr);

  /* User-initiated cancel (e.g. the QR login window was closed mid long-poll):
   * the transfer-info callback returned non-zero. This is not a fault, so it
   * must NOT trip the sticky LINE health gate — doing so would short-circuit
   * the next login attempt's first call. Return an empty result quietly. */
  if (rc == CURLE_ABORTED_BY_CALLBACK) {
    ENIL_LOG("Line.post", "aborted by cancel: %s", path);
    enil_buf_free(&buf);
    return result;
  }

  if (rc != CURLE_OK) {
    char msg[160];
    ENIL_LOG("Line.post", "transport error %s: %s",
             curl_easy_strerror(rc), path);
    snprintf(msg, sizeof(msg),
             "cannot reach LINE (%s). Restart the app or sign in again to retry.",
             curl_easy_strerror(rc));
    enil_health_set_failure(ENIL_ERR_LINE, msg);
    enil_buf_free(&buf);
    return result;
  }

  ENIL_LOG("Line.post", "-> HTTP %ld: %s", result.status, path);

  /* Categorise the HTTP outcome. Account/auth and gateway failures set the
   * sticky LINE gate — the account-lockout risk from repeated bad-auth pings
   * outweighs the UX cost of a manual reconnect. The one message-local
   * exception (an unavailable historical E2EE public key) is handled below.
   * The /api/auth/tokenRefresh call uses the same plumbing, so an expired
   * refresh-token shows up here as a 401 too. */
  if (result.status == 401 || result.status == 403) {
    enil_health_set_failure(ENIL_ERR_LINE,
      "rejected access token. Sign in again via QR to recover.");
    free(buf.data);
    return result;
  }
  if (result.status >= 500 && result.status < 600) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "server error (HTTP %ld). Restart the app or sign in again to retry.",
             result.status);
    enil_health_set_failure(ENIL_ERR_LINE, msg);
    free(buf.data);
    return result;
  }
  /* A public-key lookup can legitimately fail for one historical message
   * after its peer has rotated that key out of LINE's lookup service.  This is
   * message-local, not evidence that the account or gateway is unhealthy.
   * Return the 400 to the decrypt caller (which leaves that row failed and
   * advances to the next message) without poisoning later LINE requests. */
  if (result.status == 400 &&
      strcmp(path,
             "/api/talk/thrift/Talk/TalkService/getE2EEPublicKey") == 0) {
    ENIL_LOG("Line.post",
             "public key unavailable (HTTP 400); continuing sync");
    free(buf.data);
    return result;
  }
  if (result.status != 0 && (result.status < 200 || result.status >= 300)) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "unexpected HTTP %ld. Restart the app or sign in again to retry.",
             result.status);
    enil_health_set_failure(ENIL_ERR_LINE, msg);
    free(buf.data);
    return result;
  }
  result.body = buf.data;
  return result;
}

static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }

/* Apply a parsed TokenV3IssueResult to session.json. Persists all fields needed
 * for proactive refresh; numeric fields are stored as strings to round-trip
 * with the server's json.Number encoding. */
static void session_apply_token_v3(session_t *session, const talk_token_v3_issue_result_t *t) {
  free(session->accessToken);
  session->accessToken = dup_or_null(t->accessToken);
  if (t->refreshToken) {
    free(session->refreshToken);
    session->refreshToken = strdup(t->refreshToken);
  }
  session->durationUntilRefreshInSec = t->durationUntilRefreshInSec;
  session->tokenIssueTimeEpochSec    = t->tokenIssueTimeEpochSec;
  if (session->refreshApiRetryPolicy) {
    cJSON_Delete(session->refreshApiRetryPolicy);
    session->refreshApiRetryPolicy = NULL;
  }
  if (t->raw) {
    cJSON *policy = cJSON_GetObjectItemCaseSensitive(t->raw, "refreshApiRetryPolicy");
    if (cJSON_IsObject(policy))
      session->refreshApiRetryPolicy = cJSON_Duplicate(policy, 1);
  }
}

/* Serialize a session_t and write it back to session_path. */
static int session_persist(const char *path, const session_t *session) {
  return enil_session_save(path, session);
}

/* ============================================================================
 * Refresh the LINE access token via /api/auth/tokenRefresh and persist the
 * new credentials back to session.json. Returns malloc'd new access token.
 * ==========================================================================*/
static const char *refresh_string(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* -1: keep the journal and stop, 0: make a fresh request, 1: replay the reply,
 * 2: the reply was already committed. Login IDs reject superseded journals;
 * refresh IDs also detect a committed reply after inline access-token rotation. */
static int recover_refresh(const char *path, const session_t *session, char **body,
                           char **refresh_id) {
  cJSON *pending = NULL, *reply = NULL, *tokens;
  const char *owner, *current, *previous, *raw, *id, *issued_access, *issued_refresh;
  int result = -1;
  if (access(path, F_OK) != 0)
    return errno == ENOENT ? 0 : -1;
  pending = enil_session_read(path);
  owner = refresh_string(pending, "loginId");
  current = refresh_string(session->snapshot, "loginId");
  if (owner && current && strcmp(owner, current)) {
    result = enil_session_retire_file(path) ? 0 : -1;
    goto done;
  }
  id = refresh_string(pending, "refreshId");
  if (id && session->refreshJournalId && !strcmp(id, session->refreshJournalId)) {
    result = enil_session_retire_file(path) ? 2 : -1;
    goto done;
  }
  previous = refresh_string(pending, "previousRefreshToken");
  raw = refresh_string(pending, "responseBody");
  if (!previous || !raw)
    goto done;
  if (!strcmp(previous, session->refreshToken)) {
    *body = strdup(raw);
    *refresh_id = id ? strdup(id) : enil_session_new_id();
    if (*body && *refresh_id)
      result = 1;
    goto done;
  }
  /* Legacy journals have no generation/commit ID. A changed refresh token
   * proves the old request must not be replayed. Preserve the evidence and
   * distinguish an already committed response from a superseded login. */
  reply = cJSON_Parse(raw);
  tokens = cJSON_GetObjectItemCaseSensitive(reply, "data");
  if (cJSON_HasObjectItem(tokens, "tokenV3IssueResult"))
    tokens = cJSON_GetObjectItemCaseSensitive(tokens, "tokenV3IssueResult");
  issued_access = refresh_string(tokens, "accessToken");
  issued_refresh = refresh_string(tokens, "refreshToken");
  if (enil_session_retire_file(path))
    result = ((issued_access && !strcmp(issued_access, session->accessToken)) ||
              (issued_refresh && !strcmp(issued_refresh, session->refreshToken)))
                 ? 2
                 : 0;
done:
  cJSON_Delete(reply);
  cJSON_Delete(pending);
  return result;
}

static char *token_refresh_locked(const char *session_path, int *out_line_code) {
  session_t session;
  cJSON *req = NULL, *root = NULL, *issued, *pending = NULL;
  char *body = NULL, *new_access = NULL, *login_id = NULL, *refresh_id = NULL;
  ENILLineResponse resp = {0, NULL};
  talk_token_v3_issue_result_t parsed;
  char pending_path[4096];
  int recovery, parsed_ok = 0;
  memset(&session, 0, sizeof(session));
  memset(&parsed, 0, sizeof(parsed));
  if (out_line_code)
    *out_line_code = 0;
  if (!session_path || !enil_session_bind_identity(session_path))
    return NULL;
  login_id = enil_session_login_id(session_path);
  if (!login_id || !enil_session_load(session_path, &session) || !session.accessToken ||
      !session.refreshToken)
    goto done;
  if (snprintf(pending_path, sizeof(pending_path), "%s.refresh-pending", session_path) >=
      (int)sizeof(pending_path))
    goto done;

  recovery = recover_refresh(pending_path, &session, &resp.body, &refresh_id);
  if (recovery < 0)
    goto done;
  if (recovery == 2) {
    new_access = strdup(session.accessToken);
    goto done;
  }
  if (!recovery) {
    refresh_id = enil_session_new_id();
    req = cJSON_CreateObject();
    if (!refresh_id || !req || !cJSON_AddStringToObject(req, "refreshToken", session.refreshToken))
      goto done;
    body = cJSON_PrintUnformatted(req);
    if (!body)
      goto done;
    resp = enil_line_post("/api/auth/tokenRefresh", body, session.accessToken);
    if (!resp.body || resp.status != 200) {
      ENIL_LOG("Line.token_refresh", "LINE API request failed: HTTP %ld", resp.status);
      root = resp.body ? cJSON_Parse(resp.body) : NULL;
      if (out_line_code) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsNumber(code))
          *out_line_code = code->valueint;
      }
      goto done;
    }
  }
  /* Upgrade legacy recovery journals too: the refresh ID is durable before
   * committing it with the tokens, so a subsequent replay is idempotent. */
  pending = cJSON_CreateObject();
  if (!pending || !cJSON_AddStringToObject(pending, "loginId", login_id) ||
      !cJSON_AddStringToObject(pending, "refreshId", refresh_id) ||
      !cJSON_AddStringToObject(pending, "previousRefreshToken", session.refreshToken) ||
      !cJSON_AddStringToObject(pending, "responseBody", resp.body) ||
      !enil_session_write(pending_path, pending)) {
    enil_health_set_failure(ENIL_ERR_LINE,
                            "Cannot save rotated credentials; stop before another refresh.");
    goto done;
  }
  root = cJSON_Parse(resp.body);
  issued = cJSON_GetObjectItemCaseSensitive(root, "data");
  if (cJSON_HasObjectItem(issued, "tokenV3IssueResult"))
    issued = cJSON_GetObjectItemCaseSensitive(issued, "tokenV3IssueResult");
  if (talk_token_v3_issue_result_parse(issued, &parsed) < 0) {
    LOG("token_refresh", "response missing accessToken; saved reply retained");
    goto done;
  }
  parsed_ok = 1;
  session_apply_token_v3(&session, &parsed);
  free(session.refreshJournalId);
  session.refreshJournalId = strdup(refresh_id);
  if (!session.refreshJournalId || !session_persist(session_path, &session)) {
    enil_health_set_failure(ENIL_ERR_LINE,
                            "Could not persist rotated credentials; recover the saved session.");
    goto done;
  }
  new_access = strdup(parsed.accessToken);
  if (new_access)
    unlink(pending_path);
  LOG("token_refresh", "access token refreshed");
done:
  if (parsed_ok)
    talk_token_v3_issue_result_free(&parsed);
  enil_session_free(&session);
  enil_line_response_free(&resp);
  cJSON_Delete(req);
  cJSON_Delete(root);
  cJSON_Delete(pending);
  free(body);
  free(login_id);
  free(refresh_id);
  return new_access;
}

/* Serialize refreshes while preserving ordinary API concurrency. */
char *enil_line_token_refresh(const char *session_path, int *out_line_code) {
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  char *token;
  pthread_mutex_lock(&mutex);
  token = token_refresh_locked(session_path, out_line_code);
  pthread_mutex_unlock(&mutex);
  return token;
}

/* ============================================================================
 * Decrypt the encryptedAccessTokens["2"] OBS token using the worker keychain
 * and return it as a malloc'd string. Used as Bearer credential for OBS GETs.
 * ==========================================================================*/
char *enil_line_acquire_obs_token(const char *session_path,
                                   const char *access_token)
{
  ENILLineResponse resp;
  cJSON *root, *msg_item, *data_item;
  const char *data_str, *sep, *field_end, *row_end, *token_end;
  char *obs_token;

  if (!session_path || !access_token) {
    LOG("acquire_obs_token", "NULL argument");
    return NULL;
  }

  if (!enil_session_bind_identity(session_path)) return NULL;

  /* Body is a JSON array [scope]; scope 2 = OBS general */
  resp = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/acquireEncryptedAccessToken",
    "[2]", access_token);

  if (!resp.body || resp.status != 200) {
    LOG("acquire_obs_token", "LINE API request failed");
    enil_line_response_free(&resp);
    return NULL;
  }

  root = cJSON_Parse(resp.body);
  enil_line_response_free(&resp);
  if (!root) { LOG("acquire_obs_token", "invalid JSON response"); return NULL; }

  msg_item  = cJSON_GetObjectItem(root, "message");
  data_item = cJSON_GetObjectItem(root, "data");
  if (!cJSON_IsString(msg_item) || strcmp(msg_item->valuestring, "OK") != 0 ||
      !cJSON_IsString(data_item)) {
    LOG("acquire_obs_token", "unexpected response format");
    cJSON_Delete(root);
    return NULL;
  }

  /* data is a thrift record: rows split by \x1e, fields by \x1f.
   * Token is at rows[1][0]: skip past first \x1e, read until \x1f or end. */
  data_str = data_item->valuestring;
  sep = strchr(data_str, '\x1e');
  if (!sep) {
    LOG("acquire_obs_token", "malformed thrift data");
    cJSON_Delete(root);
    return NULL;
  }
  sep++;
  field_end = strchr(sep, '\x1f');
  row_end   = strchr(sep, '\x1e');
  if (field_end && row_end)
    token_end = field_end < row_end ? field_end : row_end;
  else
    token_end = field_end ? field_end : row_end;
  if (token_end) {
    size_t len = (size_t)(token_end - sep);
    obs_token = (char *)malloc(len + 1);
    if (obs_token) { memcpy(obs_token, sep, len); obs_token[len] = '\0'; }
  } else {
    obs_token = strdup(sep);
  }
  cJSON_Delete(root);

  if (!obs_token || obs_token[0] == '\0') {
    LOG("acquire_obs_token", "empty token");
    free(obs_token);
    return NULL;
  }
  ENIL_LOG("Line.acquire_obs_token", "OBS token length %lu",
           (unsigned long)strlen(obs_token));

  /* Store as encryptedAccessTokens["2"] to match the Node.js client layout */
  {
    session_t session;
    memset(&session, 0, sizeof(session));
    if (enil_session_load(session_path, &session)) {
      if (!cJSON_IsObject(session.encryptedAccessTokens)) {
        if (session.encryptedAccessTokens) cJSON_Delete(session.encryptedAccessTokens);
        session.encryptedAccessTokens = cJSON_CreateObject();
      }
      cJSON_DeleteItemFromObject(session.encryptedAccessTokens, "2");
      cJSON_AddStringToObject(session.encryptedAccessTokens, "2", obs_token);
      session_persist(session_path, &session);
      enil_session_free(&session);
    }
  }

  LOG("acquire_obs_token", "OBS token acquired");
  return obs_token;
}

/* ---- send helpers ---- */

static const char *find_e2ee_key(const session_t *session, long long key_id) {
  cJSON *keys = session ? session->e2eeKeys : NULL;
  int i, n;
  if (!cJSON_IsArray(keys)) return NULL;
  n = cJSON_GetArraySize(keys);
  for (i = 0; i < n; i++) {
    cJSON *entry = cJSON_GetArrayItem(keys, i);
    cJSON *kid   = cJSON_GetObjectItem(entry, "keyId");
    cJSON *expk  = cJSON_GetObjectItem(entry, "exportedKey");
    if (enil_json_coerce_int64(kid) == key_id && cJSON_IsString(expk))
      return expk->valuestring;
  }
  return NULL;
}

static long long next_req_seq(const session_t *session) {
  long long cur = (session && session->reqSeq) ? session->reqSeq
    : ((long long)time(NULL) % 2147483648LL) - 1;
  return (cur + 1) % 2147483648LL;
}

static char *encode_plaintext(const char *text) {
  cJSON *obj = cJSON_CreateObject();
  char *json_str, *b64;
  if (!obj) return NULL;
  cJSON_AddStringToObject(obj, "text", text);
  json_str = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (!json_str) return NULL;
  b64 = enil_b64_encode((const unsigned char *)json_str, strlen(json_str));
  free(json_str);
  return b64;
}

/* Splits base64 ciphertext into 5 LINE E2EE chunks (all malloc'd).
 * Caller must free each chunks[i] on success. */
static int split_v2_ciphertext(const char *b64, int sender_key_id, int receiver_key_id,
                                char *chunks[5]) {
  unsigned char *raw = NULL, kb[4];
  int raw_len, i;
  for (i = 0; i < 5; i++) chunks[i] = NULL;
  raw_len = enil_b64_decode_alloc(b64, &raw);
  if (raw_len < 29) {
    LOG("split_v2_ciphertext", "ciphertext too short");
    free(raw);
    return 0;
  }
  chunks[0] = enil_b64_encode(raw, 16);
  chunks[1] = enil_b64_encode(raw + 28, (size_t)(raw_len - 28));
  chunks[2] = enil_b64_encode(raw + 16, 12);
  free(raw);
  kb[0] = (sender_key_id >> 24) & 0xFF; kb[1] = (sender_key_id >> 16) & 0xFF;
  kb[2] = (sender_key_id >>  8) & 0xFF; kb[3] =  sender_key_id        & 0xFF;
  chunks[3] = enil_b64_encode(kb, 4);
  kb[0] = (receiver_key_id >> 24) & 0xFF; kb[1] = (receiver_key_id >> 16) & 0xFF;
  kb[2] = (receiver_key_id >>  8) & 0xFF; kb[3] =  receiver_key_id        & 0xFF;
  chunks[4] = enil_b64_encode(kb, 4);
  for (i = 0; i < 5; i++) {
    if (!chunks[i]) {
      for (i = 0; i < 5; i++) { free(chunks[i]); chunks[i] = NULL; }
      return 0;
    }
  }
  return 1;
}

/* Unwraps {message:"OK", data:{...}} and returns data. Caller must cJSON_Delete root. */
static cJSON *line_response_data(cJSON *root, const char *fn) {
  cJSON *data;
  if (!root) return NULL;
  data = cJSON_GetObjectItem(root, "data");
  if (!data) ENIL_LOG("Line.response_data", "%s: missing data in response",
                      fn ? fn : "?");
  return data;
}

static char *fetch_public_key(const char *access_token, const char *mid, int key_id) {
  char body[512];
  ENILLineResponse resp;
  cJSON *root, *data, *kd;
  char *result;
  snprintf(body, sizeof(body), "[\"%s\",1,%d]", mid, key_id);
  resp = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getE2EEPublicKey",
    body, access_token);
  if (resp.status != 200) {
    LOG("fetch_public_key", "request failed");
    enil_line_response_free(&resp);
    return NULL;
  }
  root = cJSON_Parse(resp.body);
  enil_line_response_free(&resp);
  data = line_response_data(root, "fetch_public_key");
  if (!data) { cJSON_Delete(root); return NULL; }
  kd = cJSON_GetObjectItem(data, "keyData");
  if (!cJSON_IsString(kd)) {
    LOG("fetch_public_key", "missing keyData");
    cJSON_Delete(root);
    return NULL;
  }
  result = strdup(kd->valuestring);
  cJSON_Delete(root);
  return result;
}

static talk_e2ee_group_shared_key_t *fetch_group_key(const char *access_token,
                                                     const char *group_mid) {
  talk_e2ee_group_shared_key_t *gk = calloc(1, sizeof(*gk));
  if (!gk) return NULL;
  if (talk_get_last_e2ee_group_shared_key(access_token, group_mid, gk) != 0) {
    free(gk);
    return NULL;
  }
  return gk;
}

static cJSON *fetch_last_e2ee_public_keys(const char *access_token,
                                           const char *group_mid) {
  char body[512];
  ENILLineResponse resp;
  cJSON *root, *data, *detached;
  snprintf(body, sizeof(body), "[\"%s\"]", group_mid);
  resp = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getLastE2EEPublicKeys",
    body, access_token);
  if (resp.status != 200) {
    ENIL_LOG("Line.fetch_last_e2ee_public_keys",
             "HTTP %ld: %.256s",
             resp.status, resp.body ? resp.body : "(null)");
    enil_line_response_free(&resp);
    return NULL;
  }
  root = cJSON_Parse(resp.body);
  enil_line_response_free(&resp);
  data = line_response_data(root, "fetch_last_e2ee_public_keys");
  if (!data) { cJSON_Delete(root); return NULL; }
  detached = cJSON_DetachItemFromObject(root, "data");
  cJSON_Delete(root);
  return detached;
}

static talk_e2ee_group_shared_key_t *register_group_key(const char *access_token,
                                                        const char *group_mid,
                                                        cJSON *enc_keys) {
  talk_e2ee_group_shared_key_t *gk = calloc(1, sizeof(*gk));
  if (!gk) return NULL;
  if (talk_register_e2ee_group_key(access_token, group_mid, enc_keys, gk) != 0) {
    free(gk);
    return NULL;
  }
  return gk;
}

static talk_e2ee_group_shared_key_t *create_and_register_group_key(
    const char *session_path, session_t *session,
    const char *access_token, const char *my_mid,
    const char *group_mid) {
  cJSON *pub_keys, *members, *entry, *new_restore;
  ENILWorkerCreateGroupKeyResult cgk = {NULL, NULL};
  long long my_key_id;
  const char *my_private_key;
  char *restore_owned = NULL;
  talk_e2ee_group_shared_key_t *result = NULL;

  pub_keys = fetch_last_e2ee_public_keys(access_token, group_mid);
  if (!pub_keys) return NULL;

  my_key_id      = session->e2eeLatestKeyId;
  my_private_key = find_e2ee_key(session, my_key_id);
  if (!my_private_key) {
    LOG("create_and_register_group_key", "missing private key");
    cJSON_Delete(pub_keys);
    return NULL;
  }

  /* Build members array with self first, then others */
  members = cJSON_CreateArray();
  entry = pub_keys->child;
  while (entry) {
    cJSON *kid_j  = cJSON_GetObjectItem(entry, "keyId");
    cJSON *kdat_j = cJSON_GetObjectItem(entry, "keyData");
    if (cJSON_IsNumber(kid_j) && cJSON_IsString(kdat_j)) {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "mid",       entry->string);
      cJSON_AddNumberToObject(m, "keyId",     kid_j->valuedouble);
      cJSON_AddStringToObject(m, "publicKey", kdat_j->valuestring);
      /* Self goes first so response encryptedSharedKey is our copy */
      if (strcmp(entry->string, my_mid) == 0)
        cJSON_InsertItemInArray(members, 0, m);
      else
        cJSON_AddItemToArray(members, m);
    }
    entry = entry->next;
  }
  cJSON_Delete(pub_keys);

  if (cJSON_GetArraySize(members) == 0) {
    LOG("create_and_register_group_key", "no members");
    cJSON_Delete(members);
    return NULL;
  }

  restore_owned = session->workerRestoreState
    ? cJSON_PrintUnformatted(session->workerRestoreState) : NULL;

  if (!enil_worker_create_group_key(restore_owned, my_private_key, (int)my_key_id,
                                     members, &cgk)) {
    LOG("create_and_register_group_key", "worker call failed");
    cJSON_Delete(members);
    free(restore_owned);
    return NULL;
  }
  free(restore_owned);
  cJSON_Delete(members);

  if (cgk.worker_restore_state) {
    new_restore = cJSON_Parse(cgk.worker_restore_state);
    if (new_restore) {
      if (session->workerRestoreState) cJSON_Delete(session->workerRestoreState);
      session->workerRestoreState = new_restore;
    }
  }

  result = register_group_key(access_token, group_mid, cgk.enc_keys);
  enil_worker_create_group_key_free(&cgk);

  (void)session_path; /* used only if we add immediate session persist here */
  return result;
}

enil_mid_kind_t enil_mid_kind(const char *mid) {
  if (!mid || !mid[0]) return ENIL_MID_UNKNOWN;
  switch (tolower((unsigned char)mid[0])) {
    case 'u': return ENIL_MID_USER;
    case 'c': return ENIL_MID_GROUP;
    case 'r': return ENIL_MID_ROOM;
    case 's': return ENIL_MID_SQUARE;
    default:  return ENIL_MID_UNKNOWN;
  }
}

int enil_mid_is_one_to_one(const char *mid) {
  return enil_mid_kind(mid) == ENIL_MID_USER ? 1 : 0;
}

int enil_mid_to_type(const char *mid) {
  return enil_mid_is_one_to_one(mid) ? 0 : 2;
}

/* Build an owned, heap-allocated outgoing message with boilerplate header
 * fields (id="local-NNN", from, to, toType, createdTime, contentType=0,
 * hasContent=0). The caller adds content-specific fields (text,
 * contentMetadata, chunks) before sending. Returns NULL on alloc failure. */
static talk_message_t *build_local_outgoing(const char *from_mid,
                                             const char *to_mid,
                                             long long req_seq) {
  talk_message_t *m;
  char temp_id[64];
  m = (talk_message_t *)calloc(1, sizeof(*m));
  if (!m) return NULL;
  snprintf(temp_id, sizeof(temp_id), "local-%lld", req_seq);
  m->id          = strdup(temp_id);
  m->from_mid    = strdup(from_mid);
  m->to_mid      = strdup(to_mid);
  m->toType      = enil_mid_to_type(to_mid);
  m->createdTime = (long long)time(NULL) * 1000LL;
  m->contentType = 0;
  m->hasContent  = 0;
  if (!m->id || !m->from_mid || !m->to_mid) {
    talk_message_free(m); free(m); return NULL;
  }
  return m;
}

/* Serialize a sendMessage wire body from a local message. The wire form
 * differs from local in two ways:
 *   e2ee=1: text is dropped (encrypted in chunks) and contentMetadata.REPLACE
 *           is stripped (REPLACE lives inside the ciphertext, not on the wire).
 *   e2ee=0: chunks and contentMetadata are cleared — plaintext send carries
 *           text directly with an empty contentMetadata object. */
static char *build_wire_body(long long req_seq,
                              const talk_message_t *local, int e2ee) {
  talk_send_message_req_t req;
  talk_message_t wire = *local;            /* shallow alias */
  cJSON *wire_meta = NULL;
  char *result;

  if (e2ee) {
    wire.text = NULL;
    if (local->contentMetadata) {
      wire_meta = cJSON_Duplicate(local->contentMetadata, 1);
      cJSON_DeleteItemFromObject(wire_meta, "REPLACE");
      wire.contentMetadata = wire_meta;
    }
  } else {
    wire.chunks          = NULL;
    wire.contentMetadata = NULL;           /* serializer emits {} */
  }

  memset(&req, 0, sizeof(req));
  req.reqSeq  = req_seq;
  req.message = wire;
  result = talk_send_message_body_build(&req);
  cJSON_Delete(wire_meta);
  return result;
}

static cJSON *post_send_message(const char *access_token, const char *body) {
  ENILLineResponse resp = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/sendMessage",
    body, access_token);
  cJSON *root, *data, *detached;
  if (resp.status != 200) {
    ENIL_LOG("Line.post_send_message", "HTTP %ld: %.200s",
             resp.status, resp.body ? resp.body : "(null)");
    enil_line_response_free(&resp);
    return NULL;
  }
  root = cJSON_Parse(resp.body);
  enil_line_response_free(&resp);
  data = line_response_data(root, "post_send_message");
  if (!data) { cJSON_Delete(root); return NULL; }
  detached = cJSON_DetachItemFromObject(root, "data");
  cJSON_Delete(root);
  return detached;
}

static void update_send_state(const char *session_path, session_t *session,
                               long long req_seq, long long e2ee_seq,
                               int update_e2ee, const char *restore_state) {
  cJSON *new_restore;
  session->reqSeq = req_seq;
  if (update_e2ee) session->e2eeSequenceNumber = e2ee_seq;
  if (restore_state) {
    new_restore = cJSON_Parse(restore_state);
    if (new_restore) {
      if (session->workerRestoreState) cJSON_Delete(session->workerRestoreState);
      session->workerRestoreState = new_restore;
    }
  }
  session_persist(session_path, session);
}

/* Overlay the server-authoritative response fields onto a locally-built
 * outgoing message. The local message keeps content (text/contentMetadata/
 * chunks — what we sent); the server confirms id/createdTime/sessionId/
 * hasContent. On success, ownership of `local` moves into out->message and
 * the caller must NOT free it. On failure the caller still owns `local`. */
static int overlay_send_result(cJSON *resp, talk_message_t *local,
                                ENILLineSendResult *out, const char *fn) {
  talk_message_t server;
  memset(&server, 0, sizeof(server));
  if (talk_send_message_response_parse(resp, &server) != 0 || !server.id) {
    ENIL_LOG("Line.overlay_send_result",
             "%s: failed to parse sendMessage response", fn ? fn : "?");
    talk_message_free(&server);
    return 0;
  }
  free(local->id);
  local->id          = strdup(server.id);
  local->createdTime = server.createdTime;
  local->sessionId   = server.sessionId;
  local->hasContent  = server.hasContent;
  talk_message_free(&server);
  if (!local->id) return 0;

  out->message    = local;
  out->message_id = strdup(local->id);
  out->created_at = local->createdTime;
  return out->message_id ? 1 : 0;
}

/* Helper: copy chunks[5] into a fresh cJSON array and attach to local.
 * Ownership of the array transfers to local. */
static int attach_chunks_to_local(talk_message_t *local, char *chunks[5]) {
  cJSON *arr;
  int i;
  arr = cJSON_CreateArray();
  if (!arr) return 0;
  for (i = 0; i < 5; i++)
    cJSON_AddItemToArray(arr, cJSON_CreateString(chunks[i]));
  if (local->chunks) cJSON_Delete(local->chunks);
  local->chunks = arr;
  return 1;
}

/* Shared tail of the two E2EE send paths (1:1 and group): split the worker
 * ciphertext into the five LINE wire chunks, attach them to `local`, POST
 * sendMessage, overlay the server id onto `local`/`out`, and persist the
 * advanced send state. Owns the chunks, request body, and response it
 * allocates. `key_id_a`/`key_id_b` are embedded in chunks[3]/chunks[4];
 * `e2ee_seq_next` and `e2ee_used` are persisted by update_send_state alongside
 * `new_restore_state`. Returns 1 on success, 0 on any failure. */
static int finish_e2ee_send(const char *session_path, session_t *session,
                            const char *access_token, long long req_seq,
                            const char *ciphertext, int key_id_a, int key_id_b,
                            long long e2ee_seq_next, int e2ee_used,
                            const char *new_restore_state,
                            talk_message_t *local, ENILLineSendResult *out,
                            const char *tag) {
  char *chunks[5] = {NULL, NULL, NULL, NULL, NULL}, *send_body = NULL;
  cJSON *resp = NULL;
  int i, ok = 0;

  if (!split_v2_ciphertext(ciphertext, key_id_a, key_id_b, chunks)) goto done;
  if (!attach_chunks_to_local(local, chunks)) goto done;
  send_body = build_wire_body(req_seq, local, 1);
  if (!send_body) goto done;
  resp = post_send_message(access_token, send_body);
  if (!resp) goto done;
  if (!overlay_send_result(resp, local, out, tag)) goto done;
  update_send_state(session_path, session, req_seq, e2ee_seq_next, e2ee_used,
                    new_restore_state);
  ok = 1;

done:
  for (i = 0; i < 5; i++) free(chunks[i]);
  free(send_body);
  cJSON_Delete(resp);
  return ok;
}

static int send_1on1(const char *session_path, session_t *session,
                      const char *access_token, const char *my_mid,
                      const char *chat_mid, long long req_seq,
                      const char *plaintext_b64, int content_type,
                      talk_message_t *local, ENILLineSendResult *out) {
  long long latest_key_id, e2ee_seq;
  const char *private_key;
  char *restore_str = NULL;
  talk_e2ee_public_key_t peer_key;
  int have_peer_key = 0;
  ENILWorkerEncryptUserParams p;
  ENILWorkerEncryptResult enc = {NULL, NULL};
  int ok = 0;

  memset(&peer_key, 0, sizeof(peer_key));

  latest_key_id = session->e2eeLatestKeyId;
  private_key   = find_e2ee_key(session, latest_key_id);
  if (!private_key) { LOG("send_1on1", "missing private key"); return 0; }

  e2ee_seq = session->e2eeSequenceNumber;

  if (talk_negotiate_e2ee_public_key(access_token, chat_mid, &peer_key) != 0)
    goto done;
  have_peer_key = 1;

  restore_str = session->workerRestoreState
    ? cJSON_PrintUnformatted(session->workerRestoreState) : NULL;

  memset(&p, 0, sizeof(p));
  p.worker_restore_state = restore_str;
  p.private_key          = private_key;
  p.private_key_id       = (int)latest_key_id;
  p.peer_mid             = chat_mid;
  p.peer_public_key      = peer_key.publicKey;
  p.peer_key_id          = (int)peer_key.keyId;
  p.from_mid             = my_mid;
  p.to_mid               = chat_mid;
  p.sender_key_id        = (int)latest_key_id;
  p.content_type         = content_type;
  p.sequence_number      = e2ee_seq;
  p.plaintext            = plaintext_b64;

  memset(&enc, 0, sizeof(enc));
  if (!enil_worker_encrypt_user(&p, &enc)) goto done;

  ok = finish_e2ee_send(session_path, session, access_token, req_seq,
                        enc.ciphertext, (int)latest_key_id, (int)peer_key.keyId,
                        e2ee_seq + 1, 1, enc.worker_restore_state,
                        local, out, "send_1on1");

done:
  if (have_peer_key) talk_e2ee_public_key_free(&peer_key);
  free(restore_str);
  enil_worker_encrypt_free(&enc);
  return ok;
}

static int send_group(const char *session_path, session_t *session,
                       const char *access_token, const char *my_mid,
                       const char *chat_mid, long long req_seq,
                       const char *plaintext_b64, int content_type,
                       talk_message_t *local, ENILLineSendResult *out) {
  long long latest_key_id, e2ee_seq;
  long long receiver_key_id, creator_key_id, group_key_id;
  const char *private_key, *creator_mid;
  char *creator_pub = NULL, *sender_pub = NULL, *restore_str = NULL;
  char *send_body = NULL;  /* used by the plaintext-fallback path below */
  ENILWorkerEncryptGroupParams p;
  ENILWorkerEncryptResult enc = {NULL, NULL};
  talk_e2ee_group_shared_key_t *gk = NULL;
  cJSON *resp = NULL;       /* used by the plaintext-fallback path below */
  int ok = 0;

  latest_key_id = session->e2eeLatestKeyId;
  e2ee_seq      = session->e2eeSequenceNumber;

  gk = fetch_group_key(access_token, chat_mid);
  if (!gk)
    gk = create_and_register_group_key(session_path, session,
                                       access_token, my_mid, chat_mid);
  if (!gk) {
    /* Plaintext fallback: only viable when local has plain text content.
     * Sticon sends with REPLACE in contentMetadata won't render on the
     * recipient side without E2EE; we still emit the text portion. */
    if (!local->text) goto done;
    ENIL_LOG("Line.send_group",
             "E2EE unavailable, falling back to plaintext");
    send_body = build_wire_body(req_seq, local, 0);
    if (!send_body) goto done;
    resp = post_send_message(access_token, send_body);
    if (!resp) goto done;
    if (!overlay_send_result(resp, local, out, "send_group_plain")) goto done;
    /* Plaintext fallback: clear E2EE-only metadata locally too. */
    if (local->contentMetadata) {
      cJSON_DeleteItemFromObject(local->contentMetadata, "e2eeVersion");
      cJSON_DeleteItemFromObject(local->contentMetadata, "REPLACE");
    }
    update_send_state(session_path, session, req_seq, 0, 0, NULL);
    ok = 1;
    goto done;
  }

  receiver_key_id = gk->receiverKeyId;
  creator_key_id  = gk->creatorKeyId;
  group_key_id    = gk->groupKeyId;
  if (!gk->creator) { LOG("send_group", "missing creator"); goto done; }
  creator_mid = gk->creator;

  private_key = find_e2ee_key(session, receiver_key_id);
  if (!private_key) { LOG("send_group", "missing private key"); goto done; }

  creator_pub = fetch_public_key(access_token, creator_mid, (int)creator_key_id);
  if (!creator_pub) goto done;

  sender_pub = fetch_public_key(access_token, my_mid, (int)latest_key_id);
  if (!sender_pub) goto done;

  restore_str = session->workerRestoreState
    ? cJSON_PrintUnformatted(session->workerRestoreState) : NULL;

  memset(&p, 0, sizeof(p));
  p.worker_restore_state = restore_str;
  p.group_mid            = chat_mid;
  p.group_key            = gk->raw;
  p.my_private_key       = private_key;
  p.my_private_key_id    = (int)receiver_key_id;
  p.sender_mid           = my_mid;
  p.sender_key_id        = (int)latest_key_id;
  p.group_key_id         = (int)group_key_id;
  p.creator_public_key   = creator_pub;
  p.sender_public_key    = sender_pub;
  p.from_mid             = my_mid;
  p.to_mid               = chat_mid;
  p.content_type         = content_type;
  p.sequence_number      = e2ee_seq;
  p.plaintext            = plaintext_b64;

  memset(&enc, 0, sizeof(enc));
  if (!enil_worker_encrypt_group(&p, &enc)) goto done;

  ok = finish_e2ee_send(session_path, session, access_token, req_seq,
                        enc.ciphertext, (int)latest_key_id, (int)group_key_id,
                        e2ee_seq, 0, enc.worker_restore_state,
                        local, out, "send_group");

done:
  free(creator_pub); free(sender_pub); free(restore_str);
  free(send_body);
  enil_worker_encrypt_free(&enc);
  if (gk) { talk_e2ee_group_shared_key_free(gk); free(gk); }
  cJSON_Delete(resp);
  return ok;
}

/* ============================================================================
 * Release the temp/server message id and embedded message inside a send result.
 * ==========================================================================*/
void enil_line_send_result_free(ENILLineSendResult *r) {
  if (!r) return;
  free(r->message_id);
  r->message_id = NULL;
  r->created_at = 0;
  if (r->message) {
    talk_message_free(r->message);
    free(r->message);
    r->message = NULL;
  }
}

/* ============================================================================
 * Send a sticker message (contentType=7). Plaintext metadata — no E2EE path.
 * Fills *out with the temp local-id and the server-confirmed message.
 * ==========================================================================*/
int enil_line_send_sticker(const char *session_path, const char *chat_mid,
                            const ENILLineSendStickerParams *sticker,
                            ENILLineSendResult *out) {
  session_t session;
  cJSON *resp = NULL;
  long long req_seq;
  char pfx;
  talk_message_t *local = NULL;
  talk_send_message_req_t req;
  cJSON *meta;
  char *body = NULL;
  int ok = 0;
  int have_session = 0;

  if (!session_path || !chat_mid || !sticker || !out ||
      !sticker->package_id || !sticker->sticker_id) {
    LOG("send_sticker", "NULL argument");
    return 0;
  }
  pfx = (char)tolower((unsigned char)chat_mid[0]);
  if (pfx != 'u' && pfx != 'c') {
    ENIL_LOG("Line.send_sticker", "unsupported chat type: '%c'", chat_mid[0]);
    return 0;
  }

  memset(&session, 0, sizeof(session));
  if (!enil_session_load(session_path, &session)) {
    LOG("send_sticker", "cannot read session"); return 0;
  }
  have_session = 1;
  if (!session.accessToken || !session.mid) {
    LOG("send_sticker", "invalid session");
    enil_session_free(&session);
    return 0;
  }

  req_seq = next_req_seq(&session);

  /* Build an owned local message that survives past the POST. After the
   * server responds we overlay its id/createdTime onto this struct and hand
   * it back via out->message — no late-stage contentMetadata reconstruction
   * needed because we never threw the content away. */
  local = build_local_outgoing(session.mid, chat_mid, req_seq);
  if (!local) goto done;
  local->contentType = 7;
  local->hasContent  = 1;

  meta = cJSON_CreateObject();
  if (!meta) goto done;
  cJSON_AddStringToObject(meta, "STKPKGID", sticker->package_id);
  cJSON_AddStringToObject(meta, "STKID",    sticker->sticker_id);
  if (sticker->version && sticker->version[0]) cJSON_AddStringToObject(meta, "STKVER",  sticker->version);
  if (sticker->option  && sticker->option[0])  cJSON_AddStringToObject(meta, "STKOPT",  sticker->option);
  if (sticker->hash    && sticker->hash[0])    cJSON_AddStringToObject(meta, "STKHASH", sticker->hash);
  local->contentMetadata = meta;   /* owned by local from here on */

  /* req.message is a shallow alias of *local; the serializer reads-only and
   * Duplicates anything it embeds in the output JSON. Do not free req.message. */
  memset(&req, 0, sizeof(req));
  req.reqSeq  = req_seq;
  req.message = *local;
  body = talk_send_message_body_build(&req);
  if (!body) goto done;

  resp = post_send_message(session.accessToken, body);
  if (!resp) goto done;

  ok = overlay_send_result(resp, local, out, "send_sticker");
  if (ok) {
    local = NULL;                  /* ownership moved into out->message */
    update_send_state(session_path, &session, req_seq, 0, 0, NULL);
  }

done:
  cJSON_Delete(resp);
  free(body);
  if (local) { talk_message_free(local); free(local); }
  if (have_session) enil_session_free(&session);
  return ok;
}

/* ============================================================================
 * Build contentMetadata for a plaintext (flow=1, no E2EE) image message.
 * Matches matrix-line `send_message.go:155-174`.
 * ==========================================================================*/
static cJSON *build_plain_image_metadata(const ENILLineSendImageParams *p,
                                         const char *file_name) {
  cJSON *meta;
  char  size_buf[32];
  char  info_buf[160];

  meta = cJSON_CreateObject();
  if (!meta) return NULL;

  snprintf(size_buf, sizeof(size_buf), "%lu", (unsigned long)p->jpeg_len);
  cJSON_AddStringToObject(meta, "FILE_SIZE",   size_buf);
  cJSON_AddStringToObject(meta, "FILE_NAME",   file_name);
  cJSON_AddStringToObject(meta, "contentType", "1");

  snprintf(info_buf, sizeof(info_buf),
           "{\"category\":\"original\",\"fileSize\":%lu,\"extension\":\"jpg\"}",
           (unsigned long)p->jpeg_len);
  cJSON_AddStringToObject(meta, "MEDIA_CONTENT_INFO", info_buf);

  if (p->thumb_data && p->thumb_width > 0 && p->thumb_height > 0) {
    snprintf(info_buf, sizeof(info_buf),
             "{\"width\":%d,\"height\":%d}",
             p->thumb_width, p->thumb_height);
    cJSON_AddStringToObject(meta, "MEDIA_THUMB_INFO", info_buf);
  }

  return meta;
}

/* ============================================================================
 * Send a plaintext image (flow=1 / LSOFF). Builds the typed sendMessage call
 * with contentType=1 plus FILE_NAME / FILE_SIZE / MEDIA_CONTENT_INFO /
 * MEDIA_THUMB_INFO metadata, then post-uploads the JPEG bytes (and an optional
 * thumbnail) to OBS keyed by the server-assigned message ID. Mirror of
 * matrix-line send_message.go:664-691.
 * ==========================================================================*/
int enil_line_send_image_plain(const char                    *session_path,
                                const char                    *chat_mid,
                                const ENILLineSendImageParams *params,
                                ENILLineSendResult            *out) {
  session_t session;
  cJSON *resp = NULL;
  long long req_seq;
  char pfx;
  talk_message_t *local = NULL;
  talk_send_message_req_t req;
  const char *file_name;
  char *body = NULL;
  char *real_id = NULL;
  char *preview_oid = NULL;
  int ok = 0;
  int have_session = 0;

  if (!session_path || !chat_mid || !params || !out ||
      !params->jpeg_data || params->jpeg_len == 0) {
    LOG("send_image_plain", "NULL argument");
    return 0;
  }
  pfx = (char)tolower((unsigned char)chat_mid[0]);
  if (pfx != 'u' && pfx != 'c') {
    ENIL_LOG("Line.send_image_plain", "unsupported chat type: '%c'",
             chat_mid[0]);
    return 0;
  }

  memset(&session, 0, sizeof(session));
  if (!enil_session_load(session_path, &session)) {
    LOG("send_image_plain", "cannot read session"); return 0;
  }
  have_session = 1;
  if (!session.accessToken || !session.mid) {
    LOG("send_image_plain", "invalid session"); goto done;
  }

  req_seq = next_req_seq(&session);

  local = build_local_outgoing(session.mid, chat_mid, req_seq);
  if (!local) goto done;
  local->contentType = 1;
  local->hasContent  = 1;

  file_name = (params->file_name && params->file_name[0])
                ? params->file_name : "image.jpg";
  local->contentMetadata = build_plain_image_metadata(params, file_name);
  if (!local->contentMetadata) goto done;

  memset(&req, 0, sizeof(req));
  req.reqSeq  = req_seq;
  req.message = *local;
  body = talk_send_message_body_build(&req);
  if (!body) goto done;

  resp = post_send_message(session.accessToken, body);
  if (!resp) goto done;

  if (!overlay_send_result(resp, local, out, "send_image_plain")) goto done;
  local = NULL;  /* ownership moved into out->message */

  /* From here on, errors must NOT bypass update_send_state — the server
   * accepted the message and the user will see it on other devices. */
  update_send_state(session_path, &session, req_seq, 0, 0, NULL);

  real_id = strdup(out->message_id);
  if (!real_id) goto upload_failed;

  if (enil_obs_upload_with_oid(session_path,
                                params->jpeg_data, params->jpeg_len,
                                "m", real_id) != 0) {
    LOG("send_image_plain", "OBS main upload failed");
    goto upload_failed;
  }

  if (params->thumb_data && params->thumb_len > 0) {
    size_t n = strlen(real_id) + strlen("__ud-preview") + 1;
    preview_oid = (char *)malloc(n);
    if (preview_oid) {
      snprintf(preview_oid, n, "%s__ud-preview", real_id);
      if (enil_obs_upload_with_oid(session_path,
                                    params->thumb_data, params->thumb_len,
                                    "m", preview_oid) != 0) {
        /* Best-effort — the message is already sent. Continue. */
        ENIL_LOG("Line.send_image_plain",
                 "preview upload failed (continuing)");
      }
    }
  }

  ok = 1;
  goto done;

upload_failed:
  /* The message was sent but media never made it. Leave the result in place;
   * UI will mark this as a partial success — bubble shows, image missing. */
  ok = 1;

done:
  free(real_id);
  free(preview_oid);
  cJSON_Delete(resp);
  free(body);
  if (local) { talk_message_free(local); free(local); }
  if (have_session) enil_session_free(&session);
  return ok;
}

/* ============================================================================
 * Build base64(JSON.stringify({"keyMaterial":"<b64-km>"})) — the E2EE payload
 * for an image. matrix-line `send_message.go:246-247`.
 * ==========================================================================*/
static char *encode_keymaterial_plaintext(const char *km_b64) {
  cJSON *obj = cJSON_CreateObject();
  char *json_str, *b64;
  if (!obj) return NULL;
  cJSON_AddStringToObject(obj, "keyMaterial", km_b64);
  json_str = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (!json_str) return NULL;
  b64 = enil_b64_encode((const unsigned char *)json_str, strlen(json_str));
  free(json_str);
  return b64;
}

/* ============================================================================
 * Build contentMetadata for an E2EE (flow=2) image message. enc_len is the
 * encrypted-on-the-wire byte count (used for FILE_SIZE per
 * matrix-line:224). Caller owns the returned cJSON.
 * ==========================================================================*/
static cJSON *build_e2ee_image_metadata(const ENILLineSendImageParams *p,
                                        const char *file_name,
                                        const char *oid,
                                        const char *enc_km_b64,
                                        size_t enc_len) {
  cJSON *meta;
  char  size_buf[32];
  char  info_buf[160];

  meta = cJSON_CreateObject();
  if (!meta) return NULL;

  cJSON_AddStringToObject(meta, "OID",         oid);
  cJSON_AddStringToObject(meta, "SID",         "emi");
  cJSON_AddStringToObject(meta, "ENC_KM",      enc_km_b64);
  cJSON_AddStringToObject(meta, "FILE_NAME",   file_name);
  cJSON_AddStringToObject(meta, "contentType", "1");
  cJSON_AddStringToObject(meta, "e2eeVersion", "2");

  snprintf(size_buf, sizeof(size_buf), "%lu", (unsigned long)enc_len);
  cJSON_AddStringToObject(meta, "FILE_SIZE", size_buf);

  snprintf(info_buf, sizeof(info_buf),
           "{\"category\":\"original\",\"fileSize\":%lu,\"extension\":\"jpg\"}",
           (unsigned long)enc_len);
  cJSON_AddStringToObject(meta, "MEDIA_CONTENT_INFO", info_buf);

  if (p->thumb_data && p->thumb_width > 0 && p->thumb_height > 0) {
    snprintf(info_buf, sizeof(info_buf),
             "{\"width\":%d,\"height\":%d}",
             p->thumb_width, p->thumb_height);
    cJSON_AddStringToObject(meta, "MEDIA_THUMB_INFO", info_buf);
  }
  return meta;
}

/* ============================================================================
 * Send an E2EE image (flow=2). Encrypts the JPEG with a fresh keyMaterial,
 * uploads via emi, encrypts the thumbnail with the same keyMaterial, then
 * sends a contentType=1 message with the keyMaterial payload as chunks.
 * Mirror of matrix-line `send_message.go:175-248`.
 * ==========================================================================*/
static int enil_line_send_image_e2ee(const char                    *session_path,
                                      const ENILLineSendImageParams *params,
                                      const char                    *chat_mid,
                                      ENILLineSendResult            *out) {
  session_t session;
  talk_message_t *local = NULL;
  unsigned char km[ENIL_CRYPTO_FILE_KM_LEN];
  unsigned char *enc_main = NULL, *enc_thumb = NULL;
  size_t enc_main_len = 0, enc_thumb_len = 0;
  char *enc_km_b64 = NULL, *plaintext_b64 = NULL;
  char *oid = NULL, *preview_oid = NULL;
  const char *file_name;
  long long req_seq;
  char pfx;
  int ok = 0;
  int have_session = 0;

  pfx = (char)tolower((unsigned char)chat_mid[0]);

  memset(&session, 0, sizeof(session));
  if (!enil_session_load(session_path, &session)) {
    LOG("send_image_e2ee", "cannot read session"); return 0;
  }
  have_session = 1;
  if (!session.accessToken || !session.mid) {
    LOG("send_image_e2ee", "invalid session"); goto done;
  }

  /* 1. Encrypt JPEG, get fresh 32-byte keyMaterial. */
  if (enil_crypto_file_encrypt(params->jpeg_data, params->jpeg_len,
                                km, &enc_main, &enc_main_len) != 0) {
    LOG("send_image_e2ee", "encrypt main failed"); goto done;
  }
  enc_km_b64 = enil_b64_encode(km, ENIL_CRYPTO_FILE_KM_LEN);
  if (!enc_km_b64) goto done;

  /* 2. Upload encrypted bytes to OBS at a fresh reqid. */
  if (enil_obs_upload(session_path, enc_main, enc_main_len, "emi", &oid) != 0) {
    LOG("send_image_e2ee", "OBS main upload failed"); goto done;
  }

  /* 3. Encrypt thumbnail with the same keyMaterial; best-effort upload to
   *    <oid>__ud-preview. */
  if (params->thumb_data && params->thumb_len > 0) {
    size_t n;
    if (enil_crypto_file_encrypt_with_km(params->thumb_data, params->thumb_len,
                                          km, &enc_thumb, &enc_thumb_len) == 0) {
      n = strlen(oid) + strlen("__ud-preview") + 1;
      preview_oid = (char *)malloc(n);
      if (preview_oid) {
        snprintf(preview_oid, n, "%s__ud-preview", oid);
        if (enil_obs_upload_with_oid(session_path, enc_thumb, enc_thumb_len,
                                      "emi", preview_oid) != 0) {
          ENIL_LOG("Line.send_image_e2ee",
                   "preview upload failed (continuing)");
        }
      }
    }
  }

  /* 4. Build the local message + contentMetadata. */
  req_seq = next_req_seq(&session);
  local = build_local_outgoing(session.mid, chat_mid, req_seq);
  if (!local) goto done;
  local->contentType = 1;
  local->hasContent  = 1;

  file_name = (params->file_name && params->file_name[0])
                ? params->file_name : "image.jpg";
  local->contentMetadata = build_e2ee_image_metadata(params, file_name,
                                                     oid, enc_km_b64,
                                                     enc_main_len);
  if (!local->contentMetadata) goto done;

  /* 5. Wrap the keyMaterial in the E2EE plaintext payload. */
  plaintext_b64 = encode_keymaterial_plaintext(enc_km_b64);
  if (!plaintext_b64) goto done;

  /* 6. Worker encrypt + sendMessage with content_type=1. */
  ok = (pfx == 'u')
    ? send_1on1(session_path, &session, session.accessToken, session.mid,
                chat_mid, req_seq, plaintext_b64, 1, local, out)
    : send_group(session_path, &session, session.accessToken, session.mid,
                 chat_mid, req_seq, plaintext_b64, 1, local, out);
  if (ok) local = NULL;  /* ownership moved into out->message */

done:
  free(enc_main);
  free(enc_thumb);
  free(enc_km_b64);
  free(plaintext_b64);
  free(oid);
  free(preview_oid);
  if (local) { talk_message_free(local); free(local); }
  if (have_session) enil_session_free(&session);
  return ok;
}

/* ============================================================================
 * Public image send. Calls TalkService.determineMediaMessageFlow and
 * branches: flowMap["1"] == 2 → E2EE, else → plaintext.
 * ==========================================================================*/
int enil_line_send_image(const char                    *session_path,
                          const char                    *chat_mid,
                          const ENILLineSendImageParams *params,
                          ENILLineSendResult            *out) {
  session_t session;
  talk_media_flow_t flow;
  talk_media_flow_req_t freq;
  cJSON *entry;
  int image_flow = 1;
  int have_session = 0;

  if (!session_path || !chat_mid || !params || !out ||
      !params->jpeg_data || params->jpeg_len == 0) {
    LOG("send_image", "NULL argument"); return 0;
  }

  /* Read access token for the flow probe (cheap; no-op if it fails). */
  memset(&session, 0, sizeof(session));
  if (enil_session_load(session_path, &session)) {
    have_session = 1;
    if (session.accessToken && session.accessToken[0]) {
      memset(&flow, 0, sizeof(flow));
      freq.chatMid = chat_mid;
      if (talk_determine_media_message_flow(session.accessToken,
                                             &freq, &flow) == 0) {
        entry = cJSON_GetObjectItem(flow.flowMap, "1");
        if (cJSON_IsNumber(entry)) image_flow = (int)entry->valuedouble;
        talk_media_flow_free(&flow);
      }
    }
  }
  if (have_session) enil_session_free(&session);

  ENIL_LOG("Line.send_image", "flowMap[1]=%d -> %s",
           image_flow, image_flow == 2 ? "E2EE (emi)" : "plaintext (m)");

  if (image_flow == 2)
    return enil_line_send_image_e2ee(session_path, params, chat_mid, out);
  return enil_line_send_image_plain(session_path, chat_mid, params, out);
}

/* ============================================================================
 * Send a plain text message — runs the worker encrypt → TalkService.sendMessage
 * pipeline for both 1:1 ('u'-mid) and group ('c'-mid) chats.
 * ==========================================================================*/
int enil_line_send_text(const char *session_path, const char *chat_mid,
                         const char *text, ENILLineSendResult *out) {
  session_t session;
  char *plaintext_b64 = NULL;
  talk_message_t *local = NULL;
  long long req_seq;
  char pfx;
  int ok = 0;
  int have_session = 0;

  if (!session_path || !chat_mid || !text || !out) {
    LOG("send_text", "NULL argument");
    return 0;
  }
  pfx = (char)tolower((unsigned char)chat_mid[0]);
  if (pfx != 'u' && pfx != 'c') {
    ENIL_LOG("Line.send_text", "unsupported chat type: '%c' (mid=%s)",
             chat_mid[0], chat_mid);
    return 0;
  }
  memset(&session, 0, sizeof(session));
  if (!enil_session_load(session_path, &session)) {
    LOG("send_text", "invalid session"); return 0;
  }
  have_session = 1;
  if (!session.accessToken || !session.mid) goto done;

  plaintext_b64 = encode_plaintext(text);
  if (!plaintext_b64) goto done;

  req_seq = next_req_seq(&session);

  local = build_local_outgoing(session.mid, chat_mid, req_seq);
  if (!local) goto done;
  local->text            = strdup(text);
  local->contentMetadata = cJSON_CreateObject();
  if (!local->text || !local->contentMetadata) goto done;
  cJSON_AddStringToObject(local->contentMetadata, "e2eeVersion", "2");

  ok = (pfx == 'u')
    ? send_1on1(session_path, &session, session.accessToken, session.mid, chat_mid,
                req_seq, plaintext_b64, 0, local, out)
    : send_group(session_path, &session, session.accessToken, session.mid, chat_mid,
                 req_seq, plaintext_b64, 0, local, out);
  if (ok) local = NULL;   /* ownership moved into out->message */

done:
  if (local) { talk_message_free(local); free(local); }
  free(plaintext_b64);
  if (have_session) enil_session_free(&session);
  return ok;
}

/* Build the REPLACE object: {"sticon":{"resources":[{S, E, productId,
 * sticonId, version, resourceType}, ...]}}. Used both for the E2EE
 * plaintext payload (wrapped in {"text", "REPLACE"}) and as a local-only
 * contentMetadata field so the renderer can identify sticons without
 * re-decrypting chunks. Returns a fresh cJSON object owned by the caller. */
static cJSON *build_replace_sticon_obj(const ENILLineSticonResource *res, int count) {
  cJSON *replace, *sticon_obj, *arr;
  int i;
  replace    = cJSON_CreateObject();
  sticon_obj = cJSON_CreateObject();
  arr        = cJSON_CreateArray();
  if (!replace || !sticon_obj || !arr) {
    cJSON_Delete(replace); cJSON_Delete(sticon_obj); cJSON_Delete(arr);
    return NULL;
  }
  for (i = 0; i < count; i++) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "S",            res[i].S);
    cJSON_AddNumberToObject(r, "E",            res[i].E);
    cJSON_AddStringToObject(r, "productId",    res[i].package_id);
    cJSON_AddStringToObject(r, "sticonId",     res[i].sticon_id);
    /* Real LINE clients send version as a JSON number (e.g. 3) for animated
     * sticons and an empty string for versionless ones. Mirror that: numeric
     * when we have a value, "" otherwise. */
    if (res[i].version && res[i].version[0])
      cJSON_AddNumberToObject(r, "version", (double)strtoll(res[i].version, NULL, 10));
    else
      cJSON_AddStringToObject(r, "version", "");
    cJSON_AddStringToObject(r, "resourceType",
        res[i].resource_type && res[i].resource_type[0] ? res[i].resource_type : "STATIC");
    cJSON_AddItemToArray(arr, r);
  }
  cJSON_AddItemToObject(sticon_obj, "resources", arr);
  cJSON_AddItemToObject(replace,    "sticon",    sticon_obj);
  return replace;
}

static char *encode_sticon_plaintext(const char *text,
                                      const ENILLineSticonResource *res, int count) {
  cJSON *obj, *replace;
  char *json_str, *b64;
  obj = cJSON_CreateObject();
  if (!obj) return NULL;
  cJSON_AddStringToObject(obj, "text", text);
  replace = build_replace_sticon_obj(res, count);
  if (!replace) { cJSON_Delete(obj); return NULL; }
  cJSON_AddItemToObject(obj, "REPLACE", replace);
  json_str = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (!json_str) return NULL;
  b64 = enil_b64_encode((const unsigned char *)json_str, strlen(json_str));
  free(json_str);
  return b64;
}

static char *build_sticon_ownership(const ENILLineSticonResource *res, int count) {
  cJSON *arr = cJSON_CreateArray();
  char *result;
  int i, j;
  if (!arr) return NULL;
  for (i = 0; i < count; i++) {
    int dup = 0;
    for (j = 0; j < i; j++) {
      if (strcmp(res[i].package_id, res[j].package_id) == 0) { dup = 1; break; }
    }
    if (!dup) cJSON_AddItemToArray(arr, cJSON_CreateString(res[i].package_id));
  }
  result = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  return result;
}

/* ============================================================================
 * Send a sticon-bearing message. Encodes the REPLACE block into the E2EE
 * plaintext before encrypting and posting.
 * ==========================================================================*/
int enil_line_send_inline_sticon(const char *session_path, const char *chat_mid,
                                  const char *text,
                                  const ENILLineSticonResource *resources, int resource_count,
                                  ENILLineSendResult *out) {
  session_t session;
  char *plaintext_b64 = NULL, *ownership = NULL, *replace_str = NULL;
  talk_message_t *local = NULL;
  cJSON *replace_obj;
  long long req_seq;
  char pfx;
  int ok = 0;
  int have_session = 0;

  if (!session_path || !chat_mid || !text || !resources || resource_count <= 0 || !out) {
    LOG("send_inline_sticon", "NULL/empty argument");
    return 0;
  }
  pfx = (char)tolower((unsigned char)chat_mid[0]);
  if (pfx != 'u' && pfx != 'c') {
    ENIL_LOG("Line.send_inline_sticon",
             "unsupported chat type: '%c'", chat_mid[0]);
    return 0;
  }
  memset(&session, 0, sizeof(session));
  if (!enil_session_load(session_path, &session)) {
    LOG("send_inline_sticon", "invalid session"); return 0;
  }
  have_session = 1;
  if (!session.accessToken || !session.mid) goto done;

  plaintext_b64 = encode_sticon_plaintext(text, resources, resource_count);
  ownership     = build_sticon_ownership(resources, resource_count);
  if (!plaintext_b64 || !ownership) {
    LOG("send_inline_sticon", "encoding failed");
    goto done;
  }

  /* Serialize REPLACE for local-only contentMetadata. REPLACE is stripped
   * from the wire body by build_wire_body — it travels inside chunks[1]. */
  replace_obj = build_replace_sticon_obj(resources, resource_count);
  if (!replace_obj) goto done;
  replace_str = cJSON_PrintUnformatted(replace_obj);
  cJSON_Delete(replace_obj);
  if (!replace_str) goto done;

  req_seq = next_req_seq(&session);

  local = build_local_outgoing(session.mid, chat_mid, req_seq);
  if (!local) goto done;
  local->text            = strdup(text);
  local->contentMetadata = cJSON_CreateObject();
  if (!local->text || !local->contentMetadata) goto done;
  cJSON_AddStringToObject(local->contentMetadata, "e2eeVersion",      "2");
  cJSON_AddStringToObject(local->contentMetadata, "STICON_OWNERSHIP", ownership);
  cJSON_AddStringToObject(local->contentMetadata, "REPLACE",          replace_str);

  ok = (pfx == 'u')
    ? send_1on1(session_path, &session, session.accessToken, session.mid, chat_mid,
                req_seq, plaintext_b64, 0, local, out)
    : send_group(session_path, &session, session.accessToken, session.mid, chat_mid,
                 req_seq, plaintext_b64, 0, local, out);
  if (ok) local = NULL;

done:
  if (local) { talk_message_free(local); free(local); }
  if (have_session) enil_session_free(&session);
  free(plaintext_b64); free(ownership); free(replace_str);
  return ok;
}

/* ============================================================================
 * Walk a draft text and convert (label) markers into the ordered REPLACE
 * resource array required by the E2EE sticon payload. Returns malloc'd array.
 * ==========================================================================*/
ENILLineSticonResource *enil_line_build_sticon_resources(
    const char *text, int count,
    const char * const *package_ids,
    const char * const *sticon_ids,
    const char * const *alt_texts,
    const char * const *resource_types,
    const char * const *versions) {
  ENILLineSticonResource *res;
  const char *scan, *text_end;
  int i;

  if (!text || count <= 0 || !package_ids || !sticon_ids || !alt_texts) {
    LOG("build_sticon_resources", "NULL argument");
    return NULL;
  }

  res = (ENILLineSticonResource *)calloc((size_t)count, sizeof(ENILLineSticonResource));
  if (!res) return NULL;

  text_end = text + strlen(text);
  scan = text;

  for (i = 0; i < count; i++) {
    const char *alt = alt_texts[i];
    const char *marker;
    size_t mLen;
    char *alt_marker = NULL;

    res[i].package_id    = package_ids[i];
    res[i].sticon_id     = sticon_ids[i];
    res[i].version       = (versions && versions[i]) ? versions[i] : "";
    res[i].resource_type = (resource_types && resource_types[i])
                             ? resource_types[i] : "STATIC";

    if (alt && alt[0]) {
      mLen = strlen(alt) + 2;
      alt_marker = (char *)malloc(mLen + 1);
      if (!alt_marker) { free(res); return NULL; }
      alt_marker[0] = '(';
      memcpy(alt_marker + 1, alt, strlen(alt));
      alt_marker[mLen - 1] = ')';
      alt_marker[mLen] = '\0';
      marker = alt_marker;
    } else {
      marker = "$";
      mLen = 1;
    }

    while (scan < text_end) {
      if ((size_t)(text_end - scan) >= mLen && memcmp(scan, marker, mLen) == 0)
        break;
      scan++;
    }

    res[i].S = (int)(scan - text);
    res[i].E = res[i].S + (int)mLen;

    free(alt_marker);
    if (scan < text_end) scan += mLen;
  }

  return res;
}

/* ============================================================================
 * Wrapper for TalkService.sendChatRemoved. Loads session, bumps reqSeq, calls
 * the RPC, persists the new reqSeq on success. Returns 1 on success.
 * ==========================================================================*/
int enil_line_send_chat_removed(const char *session_path,
                                 const char *chat_mid,
                                 const char *last_read_message_id,
                                 long long   last_read_message_time) {
  session_t session;
  long long req_seq;
  int ok = 0;
  if (!session_path || !chat_mid) {
    LOG("send_chat_removed", "NULL argument");
    return 0;
  }
  memset(&session, 0, sizeof(session));
  if (!enil_session_load(session_path, &session)) {
    LOG("send_chat_removed", "cannot read session");
    return 0;
  }
  if (!session.accessToken) { enil_session_free(&session); return 0; }
  req_seq = next_req_seq(&session);
  if (talk_send_chat_removed(session.accessToken, req_seq, chat_mid,
                             last_read_message_id, last_read_message_time) == 0) {
    update_send_state(session_path, &session, req_seq, 0, 0, NULL);
    ok = 1;
  }
  enil_session_free(&session);
  return ok;
}
