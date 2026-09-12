/* ============================================================================
 * Cloudflare worker client — /sign for HMAC, /e2ee/encrypt-user-v2,
 * /e2ee/encrypt-group-v2, and /e2ee/decrypt with workerRestoreState plumbing.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "enil_http.h"
#include "enil_worker.h"
#include "enil_api_json.h"
#include "enil_cocoa_log.h"
#include "enil_health.h"

/* Thin wrapper: routes fixed-string call sites through NSLog with a
 * "Worker.<fn>" tag. Formatted sites should call ENIL_LOG directly. */
#define LOG(fn, msg) enil_log("Worker." fn, "%s", msg)

/* Runtime-configured worker endpoint + shared secret. Both are NULL until
 * enil_worker_set_credentials() is called from ENILAccount / AppDelegate. */
static char *g_worker_url    = NULL;
static char *g_worker_secret = NULL;

static char *dup_or_null(const char *s) {
  size_t n;
  char *p;
  if (!s || !*s) return NULL;
  n = strlen(s);
  p = (char *)malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

void enil_worker_set_credentials(const char *url, const char *secret) {
  free(g_worker_url);
  free(g_worker_secret);
  g_worker_url    = dup_or_null(url);
  g_worker_secret = dup_or_null(secret);
  ENIL_LOG("Worker.set_credentials",
           "url=%s secret=%s",
           g_worker_url    ? g_worker_url : "(unset)",
           g_worker_secret ? "(set)"      : "(unset)");
  /* Fresh credentials give us a reason to try again — clear any sticky
   * worker failure so the next call hits the network. enil_health
   * suppresses the UI notification if the flag wasn't actually set, so
   * this is a no-op churn-free call when nothing was wrong. */
  enil_health_clear_failure(ENIL_ERR_WORKER);
}

static char *worker_post(const char *path, const char *json_body) {
  char url[512];
  char secret_hdr[256];
  ENILBuf buf = {NULL, 0};
  CURL *curl;
  struct curl_slist *hdrs = NULL;
  CURLcode rc;
  long status = 0;

  if (!g_worker_url || !g_worker_secret) {
    LOG("worker_post", "credentials not configured");
    return NULL;
  }

  /* Sticky-failure gate. Once any worker call has failed (auth, transport,
   * or HTTP 5xx), every subsequent worker_post() returns NULL without
   * hitting the wire. The flag clears only when the user re-saves credentials
   * in Preferences (enil_worker_set_credentials calls enil_health_clear_failure).
   * This keeps us from hammering a broken worker — and, more importantly,
   * keeps us from issuing LINE API calls that will fail at the /sign step
   * with the gateway's auth-rejection counter ticking up toward a lockout. */
  if (enil_health_is_failed(ENIL_ERR_WORKER)) {
    ENIL_LOG("Worker.post",
             "skipping %s — worker is in failed state", path);
    return NULL;
  }

  snprintf(url, sizeof(url), "%s%s", g_worker_url, path);
  snprintf(secret_hdr, sizeof(secret_hdr),
           "X-Worker-Secret: %s", g_worker_secret);
  curl = enil_curl_new(&buf);
  if (!curl) { LOG("worker_post", "enil_curl_new failed"); return NULL; }

  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, secret_hdr);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

  /* Bound every worker call so a wedged socket cannot hang a sync or
   * SSE-recovery thread for minutes. iOS invalidates open sockets across an
   * app background/resume, but libcurl keeps waiting on the dead connection
   * unless given an explicit deadline (the LINE client sets CURLOPT_TIMEOUT_MS
   * for the same reason). NOSIGNAL is required because worker_post runs on
   * detached pthreads and libcurl's default SIGALRM-based DNS timeout is unsafe
   * off the main thread. The worker normally answers in well under a second;
   * 30s total / 15s connect is generous headroom that still fails fast. */
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);

  ENIL_LOG("Worker.post", "POST %s (%lu bytes)",
           path, json_body ? (unsigned long)strlen(json_body) : 0UL);

  rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    char msg[160];
    ENIL_LOG("Worker.post", "transport error %s: %s",
             curl_easy_strerror(rc), path);
    snprintf(msg, sizeof(msg),
             "cannot reach worker (%s). Re-save credentials in Preferences to retry.",
             curl_easy_strerror(rc));
    enil_health_set_failure(ENIL_ERR_WORKER, msg);
    enil_buf_free(&buf);
    return NULL;
  }
  ENIL_LOG("Worker.post", "-> HTTP %ld: %s", status, path);

  /* Categorise the HTTP outcome and set the sticky gate for anything that
   * isn't a clean 2xx. The user picked "all errors sticky" as the policy
   * (see CLAUDE.md / earlier discussion): the safety win against LINE
   * account lockout outweighs the UX cost of a wifi blip requiring a
   * manual reconnect. */
  if (status == 401 || status == 403) {
    ENIL_LOG("Worker.post",
             "auth failed (HTTP %ld) — sticky", status);
    enil_health_set_failure(ENIL_ERR_WORKER,
      "rejected credentials — check Shared Secret in Preferences");
    enil_buf_free(&buf);
    return NULL;
  }
  if (status >= 500 && status < 600) {
    char msg[96];
    snprintf(msg, sizeof(msg),
             "server error (HTTP %ld). Re-save credentials in Preferences to retry.",
             status);
    enil_health_set_failure(ENIL_ERR_WORKER, msg);
    enil_buf_free(&buf);
    return NULL;
  }
  if (status < 200 || status >= 300) {
    char msg[96];
    snprintf(msg, sizeof(msg),
             "unexpected HTTP %ld. Re-save credentials in Preferences to retry.",
             status);
    enil_health_set_failure(ENIL_ERR_WORKER, msg);
    enil_buf_free(&buf);
    return NULL;
  }
  return buf.data;
}

static cJSON *parse_restore_state(const char *s) {
  if (s && *s) return cJSON_Parse(s);
  return cJSON_CreateObject();
}

/* ============================================================================
 * Serialize `req` (whose ownership this takes; it is deleted here), POST it
 * to the worker `path`, and return the parsed JSON response (caller deletes),
 * or NULL on any serialize / transport / parse failure. `fn` tags log lines.
 * Every worker operation shares this envelope so the req/resp string lifetime
 * bookkeeping lives in exactly one place.
 * ==========================================================================*/
static cJSON *worker_call(const char *path, cJSON *req, const char *fn) {
  char *req_str, *resp_str;
  cJSON *resp;

  req_str = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!req_str) { ENIL_LOG("Worker.call", "%s: serialize failed", fn); return NULL; }

  resp_str = worker_post(path, req_str);
  free(req_str);
  if (!resp_str) { ENIL_LOG("Worker.call", "%s: no response", fn); return NULL; }

  resp = cJSON_Parse(resp_str);
  free(resp_str);
  if (!resp) { ENIL_LOG("Worker.call", "%s: invalid JSON response", fn); return NULL; }
  return resp;
}

/* Print the response's workerRestoreState object to a malloc'd JSON string,
 * or NULL if absent. Shared by every op that threads restore state back out. */
static char *worker_take_restore_state(cJSON *resp) {
  cJSON *item = cJSON_GetObjectItem(resp, "workerRestoreState");
  return item ? cJSON_PrintUnformatted(item) : NULL;
}

static int extract_encrypt_result(cJSON *resp, ENILWorkerEncryptResult *out,
                                   const char *fn) {
  cJSON *cipher_item = cJSON_GetObjectItem(resp, "ciphertext");
  cJSON *restore_item = cJSON_GetObjectItem(resp, "workerRestoreState");

  if (!cJSON_IsString(cipher_item) || !cipher_item->valuestring) {
    ENIL_LOG("Worker.extract_encrypt_result",
             "%s: missing ciphertext", fn);
    return 0;
  }
  if (!restore_item) {
    ENIL_LOG("Worker.extract_encrypt_result",
             "%s: missing workerRestoreState", fn);
    return 0;
  }

  out->ciphertext = strdup(cipher_item->valuestring);
  out->worker_restore_state = cJSON_PrintUnformatted(restore_item);
  if (!out->ciphertext || !out->worker_restore_state) {
    free(out->ciphertext);
    free(out->worker_restore_state);
    out->ciphertext = NULL;
    out->worker_restore_state = NULL;
    return 0;
  }
  return 1;
}

/* ============================================================================
 * Release malloc'd fields inside an ENILWorkerEncryptResult.
 * ==========================================================================*/
void enil_worker_encrypt_free(ENILWorkerEncryptResult *r) {
  if (!r) return;
  free(r->ciphertext);
  free(r->worker_restore_state);
  r->ciphertext = NULL;
  r->worker_restore_state = NULL;
}

/* ============================================================================
 * POST /e2ee/encrypt-user-v2 — encrypts a 1:1 message and returns ciphertext
 * plus the updated workerRestoreState. Returns 1 on success, 0 on failure.
 * ==========================================================================*/
int enil_worker_encrypt_user(const ENILWorkerEncryptUserParams *p,
                              ENILWorkerEncryptResult *out) {
  cJSON *req, *restore, *resp;
  int ok = 0;

  if (!p || !out) { LOG("encrypt_user", "NULL argument"); return 0; }

  req = cJSON_CreateObject();
  if (!req) return 0;

  restore = parse_restore_state(p->worker_restore_state);
  if (!restore) { cJSON_Delete(req); return 0; }
  cJSON_AddItemToObject(req, "workerRestoreState", restore);

  cJSON_AddStringToObject(req, "privateKey", p->private_key);
  cJSON_AddNumberToObject(req, "privateKeyId", (double)p->private_key_id);
  cJSON_AddStringToObject(req, "peerMid", p->peer_mid ? p->peer_mid : "");
  cJSON_AddStringToObject(req, "peerPublicKey", p->peer_public_key);
  cJSON_AddNumberToObject(req, "peerKeyId", (double)p->peer_key_id);
  cJSON_AddStringToObject(req, "to", p->to_mid);
  cJSON_AddStringToObject(req, "from", p->from_mid);
  cJSON_AddNumberToObject(req, "senderKeyId", (double)p->sender_key_id);
  cJSON_AddNumberToObject(req, "receiverKeyId", (double)p->peer_key_id);
  cJSON_AddNumberToObject(req, "contentType", (double)p->content_type);
  cJSON_AddNumberToObject(req, "sequenceNumber", (double)p->sequence_number);
  cJSON_AddStringToObject(req, "plaintext", p->plaintext);

  resp = worker_call("/e2ee/encrypt-user-v2", req, "encrypt_user");
  if (!resp) return 0;

  ok = extract_encrypt_result(resp, out, "encrypt_user");
  cJSON_Delete(resp);
  return ok;
}

/* ============================================================================
 * POST /e2ee/encrypt-group-v2 — encrypts a group message using the group
 * shared key bundle and returns ciphertext + updated workerRestoreState.
 * ==========================================================================*/
int enil_worker_encrypt_group(const ENILWorkerEncryptGroupParams *p,
                               ENILWorkerEncryptResult *out) {
  cJSON *req, *restore, *group_key_copy, *resp;
  int ok = 0;

  if (!p || !out || !p->group_key) { LOG("encrypt_group", "NULL argument"); return 0; }

  req = cJSON_CreateObject();
  if (!req) return 0;

  restore = parse_restore_state(p->worker_restore_state);
  if (!restore) { cJSON_Delete(req); return 0; }
  cJSON_AddItemToObject(req, "workerRestoreState", restore);

  cJSON_AddStringToObject(req, "groupMid", p->group_mid);

  group_key_copy = cJSON_Duplicate(p->group_key, 1);
  if (!group_key_copy) { cJSON_Delete(req); return 0; }
  cJSON_AddItemToObject(req, "groupKey", group_key_copy);

  cJSON_AddStringToObject(req, "myPrivateKey", p->my_private_key);
  cJSON_AddNumberToObject(req, "myPrivateKeyId", (double)p->my_private_key_id);
  cJSON_AddStringToObject(req, "senderMid", p->sender_mid);
  cJSON_AddNumberToObject(req, "senderKeyId", (double)p->sender_key_id);
  cJSON_AddNumberToObject(req, "groupKeyId", (double)p->group_key_id);
  cJSON_AddStringToObject(req, "creatorPublicKey", p->creator_public_key);
  cJSON_AddStringToObject(req, "senderPublicKey", p->sender_public_key);
  cJSON_AddStringToObject(req, "to", p->to_mid);
  cJSON_AddStringToObject(req, "from", p->from_mid);
  cJSON_AddNumberToObject(req, "contentType", (double)p->content_type);
  cJSON_AddNumberToObject(req, "sequenceNumber", (double)p->sequence_number);
  cJSON_AddStringToObject(req, "plaintext", p->plaintext);

  resp = worker_call("/e2ee/encrypt-group-v2", req, "encrypt_group");
  if (!resp) return 0;

  ok = extract_encrypt_result(resp, out, "encrypt_group");
  cJSON_Delete(resp);
  return ok;
}

/* ============================================================================
 * Release the encrypted-keys array and workerRestoreState inside the result.
 * ==========================================================================*/
void enil_worker_create_group_key_free(ENILWorkerCreateGroupKeyResult *r) {
  if (!r) return;
  free(r->worker_restore_state);
  cJSON_Delete(r->enc_keys);
  r->worker_restore_state = NULL;
  r->enc_keys = NULL;
}

/* ============================================================================
 * POST /e2ee/create-group-key — generates per-member encrypted key shares for
 * a new group. Returns the encryptedKeys array and updated restore state.
 * ==========================================================================*/
int enil_worker_create_group_key(const char *worker_restore_state,
                                  const char *my_private_key,
                                  int         my_private_key_id,
                                  cJSON      *members,
                                  ENILWorkerCreateGroupKeyResult *out) {
  cJSON *req, *restore, *members_copy, *resp;
  cJSON *enc_keys_item;
  int ok = 0;

  if (!my_private_key || !members || !out) {
    LOG("create_group_key", "NULL argument");
    return 0;
  }

  req = cJSON_CreateObject();
  if (!req) return 0;

  restore = parse_restore_state(worker_restore_state);
  if (!restore) { cJSON_Delete(req); return 0; }
  cJSON_AddItemToObject(req, "workerRestoreState", restore);
  cJSON_AddStringToObject(req, "myPrivateKey", my_private_key);
  cJSON_AddNumberToObject(req, "myPrivateKeyId", (double)my_private_key_id);

  members_copy = cJSON_Duplicate(members, 1);
  if (!members_copy) { cJSON_Delete(req); return 0; }
  cJSON_AddItemToObject(req, "members", members_copy);

  resp = worker_call("/e2ee/create-group-key", req, "create_group_key");
  if (!resp) return 0;

  enc_keys_item = cJSON_GetObjectItem(resp, "encryptedKeys");
  if (!cJSON_IsArray(enc_keys_item)) {
    LOG("create_group_key", "missing encryptedKeys");
    cJSON_Delete(resp);
    return 0;
  }

  out->worker_restore_state = worker_take_restore_state(resp);
  out->enc_keys = cJSON_DetachItemFromObject(resp, "encryptedKeys");
  cJSON_Delete(resp);
  if (!out->worker_restore_state || !out->enc_keys) {
    enil_worker_create_group_key_free(out);
    return 0;
  }
  ok = 1;
  return ok;
}

/* ============================================================================
 * POST /keygen — portable Curve25519 keygen for QR login.
 * Request:  { workerRestoreState }
 * Response: { keyId:int, publicKey:base64, workerRestoreState:object }
 * ==========================================================================*/
void enil_worker_keygen_free(ENILWorkerKeygenResult *r) {
  if (!r) return;
  free(r->public_key);
  free(r->worker_restore_state);
  r->public_key = NULL;
  r->worker_restore_state = NULL;
}

int enil_worker_keygen(const char *worker_restore_state,
                        ENILWorkerKeygenResult *out) {
  cJSON *req, *restore, *resp;

  if (!out) { LOG("keygen", "NULL out"); return 0; }
  memset(out, 0, sizeof(*out));

  req = cJSON_CreateObject();
  if (!req) return 0;
  restore = parse_restore_state(worker_restore_state);
  if (!restore) { cJSON_Delete(req); return 0; }
  cJSON_AddItemToObject(req, "workerRestoreState", restore);

  resp = worker_call("/keygen", req, "keygen");
  if (!resp) return 0;

  if (enil_json_get_int(resp, "keyId", &out->key_id) != 0 ||
      enil_json_dup_string(resp, "publicKey", &out->public_key) != 0) {
    LOG("keygen", "malformed response");
    cJSON_Delete(resp);
    return 0;
  }
  out->worker_restore_state = worker_take_restore_state(resp);
  cJSON_Delete(resp);

  if (!out->worker_restore_state) {
    enil_worker_keygen_free(out);
    return 0;
  }
  return 1;
}

/* ============================================================================
 * POST /e2ee/unwrap-keychain — unwrap LINE's post-scan E2EE key bundle.
 * Request:  { workerRestoreState, qrKeyId, peerPublicKey, encryptedKeyChain }
 * Response: { keys:[{keyId,exportedKey}], workerRestoreState:object }
 * ==========================================================================*/
void enil_worker_unwrap_keychain_free(ENILWorkerUnwrapResult *r) {
  if (!r) return;
  if (r->keys) cJSON_Delete(r->keys);
  free(r->worker_restore_state);
  r->keys = NULL;
  r->worker_restore_state = NULL;
}

int enil_worker_unwrap_keychain(const char *worker_restore_state,
                                 int         qr_key_id,
                                 const char *peer_public_key,
                                 const char *encrypted_keychain,
                                 ENILWorkerUnwrapResult *out) {
  cJSON *req, *restore, *resp, *keys_item;

  if (!out || !peer_public_key || !encrypted_keychain) {
    LOG("unwrap_keychain", "NULL argument");
    return 0;
  }
  memset(out, 0, sizeof(*out));

  req = cJSON_CreateObject();
  if (!req) return 0;
  restore = parse_restore_state(worker_restore_state);
  if (!restore) { cJSON_Delete(req); return 0; }
  cJSON_AddItemToObject(req, "workerRestoreState", restore);
  cJSON_AddNumberToObject(req, "qrKeyId", qr_key_id);
  cJSON_AddStringToObject(req, "peerPublicKey", peer_public_key);
  cJSON_AddStringToObject(req, "encryptedKeyChain", encrypted_keychain);

  resp = worker_call("/e2ee/unwrap-keychain", req, "unwrap_keychain");
  if (!resp) return 0;

  keys_item = cJSON_GetObjectItem(resp, "keys");
  if (!cJSON_IsArray(keys_item)) {
    LOG("unwrap_keychain", "malformed response");
    cJSON_Delete(resp);
    return 0;
  }

  out->keys                 = cJSON_DetachItemFromObject(resp, "keys");
  out->worker_restore_state = worker_take_restore_state(resp);
  cJSON_Delete(resp);

  if (!out->keys || !out->worker_restore_state) {
    enil_worker_unwrap_keychain_free(out);
    return 0;
  }
  return 1;
}

// TODO: add enil_worker_decrypt_batch(const char *path, cJSON *messages_array)
//       that POSTs all messages for a chat in one request to a /e2ee/decrypt-batch
//       worker endpoint, eliminating the per-message round-trip in sync step 6.
/* ============================================================================
 * Generic decrypt POST — forwards payload to a worker /e2ee path and
 * returns the parsed JSON response. Caller must cJSON_Delete the result.
 * ==========================================================================*/
cJSON *enil_worker_decrypt(const char *path, cJSON *payload) {
  char *body, *resp_str;
  cJSON *resp;

  if (!path || !payload) { LOG("decrypt", "NULL argument"); return NULL; }

  body = cJSON_PrintUnformatted(payload);
  if (!body) return NULL;

  resp_str = worker_post(path, body);
  free(body);
  if (!resp_str) { LOG("decrypt", "no response"); return NULL; }

  resp = cJSON_Parse(resp_str);
  free(resp_str);
  if (!resp) { LOG("decrypt", "invalid JSON response"); return NULL; }
  return resp;
}

/* ============================================================================
 * POST /sign — returns the malloc'd X-Hmac header value for a LINE API call.
 * Caller must free.
 * ==========================================================================*/
char *enil_worker_sign(const char *path, const char *body, const char *access_token) {
  cJSON *req, *resp;
  char *hmac = NULL;

  if (!path || !body || !access_token) { LOG("sign", "NULL argument"); return NULL; }

  req = cJSON_CreateObject();
  if (!req) return NULL;
  cJSON_AddStringToObject(req, "path", path);
  cJSON_AddStringToObject(req, "body", body);
  cJSON_AddStringToObject(req, "accessToken", access_token);

  resp = worker_call("/sign", req, "sign");
  if (!resp) return NULL;

  if (enil_json_dup_string(resp, "hmac", &hmac) != 0)
    LOG("sign", "missing hmac in response");

  cJSON_Delete(resp);
  return hmac;
}
