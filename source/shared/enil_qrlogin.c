/* ============================================================================
 * QR-code login flow — clean-room port of cloud/client/src/login.js and
 * postlogin.js. Protocol knowledge only (RPC paths, body shapes, ordering);
 * no extension/WASM code. All crypto goes through the worker.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "enil_line.h"
#include "enil_session.h"
#include "enil_worker.h"
#include "enil_qrlogin.h"
#include "enil_windows_probe.h"
#include "enil_cocoa_log.h"

#define SVC    "/api/talk/thrift/LoginQrCode"
#define BASE   SVC "/SecondaryQrCodeLoginService"
#define NOTICE SVC "/SecondaryQrCodeLoginPermitNoticeService"
#define TALK   "/api/talk/thrift/Talk/TalkService"
#define SYNC_INITIALIZATION "2"

#define LOG(fn, msg) enil_log("QrLogin." fn, "%s", msg)

static void emit_status(const enil_qrlogin_callbacks_t *cb, const char *m) {
  if (cb && cb->on_status) cb->on_status(m, cb->ctx);
}

/* Cooperative cancel: 1 once the caller (e.g. closed login window) asks the
 * flow to abandon. Checked at every step/poll boundary. */
static int cancelled(const enil_qrlogin_callbacks_t *cb) {
  return cb && cb->cancel && *cb->cancel;
}

/* TEMPORARY (QR-login bring-up): on a non-200 LINE response, dump the thrift
 * exception body so a rate-limited attempt is diagnostic instead of a coin
 * flip. HTTP 400 alone can't tell "user declined" from "bad request" — the
 * body carries LINE's real {message,code,...}. Remove once QR login is
 * confirmed working end-to-end. */
static void log_resp(const char *fn, const char *call, const ENILLineResponse *r) {
  char tag[64];
  snprintf(tag, sizeof(tag), "QrLogin.%s", fn ? fn : "?");
  ENIL_LOG(tag, "%s HTTP %ld body=%.500s",
           call, r->status, (r->body && r->body[0]) ? r->body : "(empty)");
}

/* ---- small JSON helpers -------------------------------------------------- */

static const char *jstr(cJSON *o, const char *k) {
  cJSON *i = o ? cJSON_GetObjectItem(o, k) : NULL;
  return (i && cJSON_IsString(i) && i->valuestring) ? i->valuestring : NULL;
}

static const char *jstr_def(cJSON *o, const char *k, const char *def) {
  const char *s = jstr(o, k);
  return (s && s[0]) ? s : def;
}

static int jnum(cJSON *o, const char *k, int def) {
  cJSON *i = o ? cJSON_GetObjectItem(o, k) : NULL;
  return cJSON_IsNumber(i) ? (int)i->valuedouble : def;
}

static void set_str(cJSON *root, const char *k, const char *v) {
  cJSON_DeleteItemFromObject(root, k);
  if (v) cJSON_AddStringToObject(root, k, v);
}

static void set_num(cJSON *root, const char *k, double v) {
  cJSON_DeleteItemFromObject(root, k);
  cJSON_AddNumberToObject(root, k, v);
}

static void set_obj(cJSON *root, const char *k, cJSON *owned) {
  cJSON_DeleteItemFromObject(root, k);
  if (owned) cJSON_AddItemToObject(root, k, owned);
}

/* Parse a LINE thrift-JSON envelope: requires {"message":"OK"}, returns a
 * detached copy of `data` (caller cJSON_Delete) or NULL. Mirrors
 * parseThriftJson() in line.js. */
static cJSON *parse_data(const char *body) {
  cJSON *root, *msg, *data, *out;
  if (!body || !body[0]) return NULL;
  root = cJSON_Parse(body);
  if (!root) { LOG("parse_data", "invalid JSON"); return NULL; }
  msg = cJSON_GetObjectItem(root, "message");
  if (!cJSON_IsString(msg) || !msg->valuestring ||
      strcmp(msg->valuestring, "OK") != 0) {
    ENIL_LOG("QrLogin.parse_data", "non-OK response: %.400s", body);
    cJSON_Delete(root);
    return NULL;
  }
  data = cJSON_GetObjectItem(root, "data");
  out  = data ? cJSON_Duplicate(data, 1) : NULL;
  cJSON_Delete(root);
  return out;
}

/* POST `body` to `path` (no token) and return the unwrapped `data` object. */
static cJSON *call_data(const char *path, const char *body) {
  ENILLineResponse r = enil_line_post(path, body, "");
  cJSON *data;
  if (!r.body || r.status != 200) {
    ENIL_LOG("QrLogin.call_data", "%s HTTP %ld", path, r.status);
    enil_line_response_free(&r);
    return NULL;
  }
  data = parse_data(r.body);
  enil_line_response_free(&r);
  return data;
}

/* ---- session.json field access (raw cJSON; keeps unknown fields) --------- */

static int session_path_of(const char *account_dir, char *out, size_t n) {
  int w = snprintf(out, n, "%s/session.json", account_dir);
  return (w > 0 && (size_t)w < n);
}

/* ---- flow steps ---------------------------------------------------------- */

/* createSession + createQrCode → fills authSessionId, callbackUrl,
 * longPollingIntervalSec, longPollingMaxCount, createdAt into `s`. */
static int ensure_qr_session(cJSON *s) {
  cJSON *created, *qr;
  const char *auth_session_id, *callback_url;
  cJSON *interval, *maxcount;

  if (jstr(s, "authSessionId") && jstr(s, "callbackUrl")) {
    LOG("ensure_qr_session", "resuming existing QR session");
    return 1;
  }

  created = call_data(BASE "/createSession", "[{}]");
  if (!created) { LOG("ensure_qr_session", "createSession failed"); return 0; }
  auth_session_id = jstr(created, "authSessionId");
  if (!auth_session_id) {
    LOG("ensure_qr_session", "missing authSessionId");
    cJSON_Delete(created);
    return 0;
  }
  set_str(s, "authSessionId", auth_session_id);
  cJSON_Delete(created);

  {
    cJSON *req = cJSON_CreateArray();
    cJSON *o = cJSON_CreateObject();
    char *body;
    cJSON_AddStringToObject(o, "authSessionId", jstr(s, "authSessionId"));
    cJSON_AddItemToArray(req, o);
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    qr = body ? call_data(BASE "/createQrCode", body) : NULL;
    free(body);
  }
  if (!qr) { LOG("ensure_qr_session", "createQrCode failed"); return 0; }

  callback_url = jstr(qr, "callbackUrl");
  interval     = cJSON_GetObjectItem(qr, "longPollingIntervalSec");
  maxcount     = cJSON_GetObjectItem(qr, "longPollingMaxCount");
  if (!callback_url) {
    LOG("ensure_qr_session", "missing callbackUrl");
    cJSON_Delete(qr);
    return 0;
  }
  set_str(s, "callbackUrl", callback_url);
  set_num(s, "longPollingIntervalSec",
          cJSON_IsNumber(interval) ? interval->valuedouble : 1);
  set_num(s, "longPollingMaxCount",
          cJSON_IsNumber(maxcount) ? maxcount->valuedouble : 30);
  set_num(s, "createdAt", (double)time(NULL));
  cJSON_Delete(qr);
  return 1;
}

/* worker /keygen → e2eeKeyId, e2eePublicKey, workerRestoreState. */
static int ensure_e2ee_key(cJSON *s) {
  ENILWorkerKeygenResult k;
  char *restore_str;
  cJSON *restore;

  /* The QR key is ephemeral and bound to exactly one authSessionId.
     finalize_login -> clear_qr_state only drops it when E2EE capture
     succeeds, so a login that fails *after* qrCodeLoginV2 would otherwise
     leak a stale key into the next attempt — which runs against a brand-new
     authSessionId. Reuse only when the key still belongs to the current
     session; otherwise regenerate. (ensure_qr_session has already populated
     authSessionId by the time this runs.) */
  {
    const char *auth     = jstr(s, "authSessionId");
    const char *key_auth = jstr(s, "e2eeKeyAuthSessionId");
    if (cJSON_GetObjectItem(s, "e2eeKeyId") && jstr(s, "e2eePublicKey") &&
        auth && key_auth && strcmp(auth, key_auth) == 0) {
      LOG("ensure_e2ee_key", "reusing existing QR key");
      return 1;
    }
  }

  restore = cJSON_GetObjectItem(s, "workerRestoreState");
  restore_str = restore ? cJSON_PrintUnformatted(restore) : NULL;
  if (!enil_worker_keygen(restore_str, &k)) {
    free(restore_str);
    LOG("ensure_e2ee_key", "worker keygen failed");
    return 0;
  }
  free(restore_str);

  set_num(s, "e2eeKeyId", (double)k.key_id);
  set_str(s, "e2eePublicKey", k.public_key);
  set_str(s, "e2eeKeyAuthSessionId", jstr(s, "authSessionId"));
  set_obj(s, "workerRestoreState", cJSON_Parse(k.worker_restore_state));
  enil_worker_keygen_free(&k);
  return 1;
}

/* Long-poll checkQrCodeVerified until the phone scans. Returns 1 on scan. */
static int poll_for_scan(cJSON *s, const enil_qrlogin_callbacks_t *cb) {
  const char *auth = jstr(s, "authSessionId");
  int interval_sec = jnum(s, "longPollingIntervalSec", 1);
  int max_count    = jnum(s, "longPollingMaxCount", 30);
  long interval_ms, max_time_ms;
  char sid_hdr[256], lst_hdr[64], body[256];
  const char *xh[2];
  int i;

  if (interval_sec <= 0) interval_sec = 1;
  if (max_count <= 0) max_count = 30;
  interval_ms  = (long)interval_sec * 1000;
  max_time_ms  = (long)(interval_sec + 10) * 1000;
  snprintf(sid_hdr, sizeof(sid_hdr), "X-Line-Session-ID: %s", auth ? auth : "");
  snprintf(lst_hdr, sizeof(lst_hdr), "X-LST: %ld", interval_ms);
  snprintf(body, sizeof(body), "[{\"authSessionId\":\"%s\"}]", auth ? auth : "");
  xh[0] = sid_hdr;
  xh[1] = lst_hdr;

  for (i = 1; i <= max_count; i++) {
    ENILLineResponse r;
    char m[64];
    if (cancelled(cb)) { LOG("poll_for_scan", "cancelled"); return 0; }
    snprintf(m, sizeof(m), "waiting for scan (%d/%d)", i, max_count);
    emit_status(cb, m);
    r = enil_line_post_ex(NOTICE "/checkQrCodeVerified", body, "",
                          xh, 2, max_time_ms, cb ? cb->cancel : NULL);
    if (r.status == 200) { enil_line_response_free(&r); return 1; }
    if (r.status == 410 || r.status == 400) {
      LOG("poll_for_scan", "QR expired - restart login");
      log_resp("poll_for_scan", "checkQrCodeVerified", &r);
      enil_line_response_free(&r);
      return 0;
    }
    enil_line_response_free(&r);
  }
  LOG("poll_for_scan", "timed out waiting for scan");
  return 0;
}

/* verifyCertificate → 1 if the stored certificate is accepted. */
static int verify_certificate(cJSON *s) {
  const char *cert = jstr_def(s, "certificate", "");
  cJSON *req = cJSON_CreateArray();
  cJSON *o = cJSON_CreateObject();
  char *body;
  ENILLineResponse r;
  int ok;

  cJSON_AddStringToObject(o, "authSessionId", jstr_def(s, "authSessionId", ""));
  cJSON_AddStringToObject(o, "certificate", cert);
  cJSON_AddItemToArray(req, o);
  body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!body) return 0;

  r = enil_line_post(BASE "/verifyCertificate", body, "");
  free(body);
  ok = (r.status == 200);
  ENIL_LOG("QrLogin.verify_certificate", "HTTP %ld (cert %s)",
           r.status, cert[0] ? "present" : "absent");
  if (!ok) log_resp("verify_certificate", "verifyCertificate", &r);
  enil_line_response_free(&r);
  return ok;
}

/* createPinCode → surface PIN → checkPinCodeVerified. Returns 1 on confirm. */
static int run_pin_flow(cJSON *s, const enil_qrlogin_callbacks_t *cb) {
  const char *auth = jstr_def(s, "authSessionId", "");
  char body[256], sid_hdr[256];
  const char *xh[2];
  cJSON *create;
  const char *pin;
  ENILLineResponse r;

  snprintf(body, sizeof(body), "[{\"authSessionId\":\"%s\"}]", auth);
  create = call_data(BASE "/createPinCode", body);
  if (!create) { LOG("run_pin_flow", "createPinCode failed"); return 0; }
  pin = jstr(create, "pinCode");
  if (!pin) { LOG("run_pin_flow", "missing pinCode"); cJSON_Delete(create); return 0; }
  if (cb && cb->on_pin) cb->on_pin(pin, cb->ctx);
  emit_status(cb, "confirm the PIN in the LINE app");
  cJSON_Delete(create);

  snprintf(sid_hdr, sizeof(sid_hdr), "X-Line-Session-ID: %s", auth);
  xh[0] = sid_hdr;
  xh[1] = "X-LST: 180000";
  r = enil_line_post_ex(NOTICE "/checkPinCodeVerified", body, "",
                        xh, 2, 190000, cb ? cb->cancel : NULL);
  if (r.status != 200) {
    log_resp("run_pin_flow", "checkPinCodeVerified", &r);
    enil_line_response_free(&r);
    return 0;
  }
  enil_line_response_free(&r);
  LOG("run_pin_flow", "PIN confirmed");
  return 1;
}

/* Reference-grade one-line summary of the qrCodeLoginV2 metaData — mirrors
 * login.js summarizeLoginMetaData(). The lengths (not values) of publicKey /
 * encryptedKeyChain are the diagnostic: a SUCCESS errorCode with a 0-char
 * encryptedKeyChain is a very different bug from errorCode != SUCCESS, and
 * the terse old log conflated both. Secrets are never printed (lengths only). */
static void log_meta(const char *fn, cJSON *meta) {
  char tag[64];
  snprintf(tag, sizeof(tag), "QrLogin.%s", fn ? fn : "?");
  ENIL_LOG(tag,
    "meta: errorCode=%s keyId=%s e2eeVersion=%s "
    "publicKeyChars=%d encryptedKeyChainChars=%d hashKeyChain=%s",
    jstr_def(meta, "errorCode", "none"),
    jstr_def(meta, "keyId", "none"),
    jstr_def(meta, "e2eeVersion", "none"),
    (int)strlen(jstr_def(meta, "publicKey", "")),
    (int)strlen(jstr_def(meta, "encryptedKeyChain", "")),
    (jstr(meta, "hashKeyChain") && jstr(meta, "hashKeyChain")[0])
      ? "present" : "absent");
}

/* Persist the raw keychain material so a failed capture is recoverable
 * offline (no rate-limited re-login). Mirrors login.js finalizeLogin's
 * `if (data?.metaData) patch.e2eeLoginMetaData = data.metaData`. Stored under
 * the same shape buildE2eePatch writes on success, so recover_from_meta()
 * reads it back identically whether capture succeeded or failed. */
static void store_login_meta(cJSON *s, cJSON *meta) {
  cJSON *lm = cJSON_CreateObject();
  cJSON_AddStringToObject(lm, "keyId", jstr_def(meta, "keyId", ""));
  cJSON_AddStringToObject(lm, "publicKey", jstr_def(meta, "publicKey", ""));
  cJSON_AddStringToObject(lm, "encryptedKeyChain",
                          jstr_def(meta, "encryptedKeyChain", ""));
  cJSON_AddStringToObject(lm, "e2eeVersion", jstr_def(meta, "e2eeVersion", "1"));
  cJSON_AddStringToObject(lm, "hashKeyChain", jstr_def(meta, "hashKeyChain", ""));
  set_obj(s, "e2eeLoginMetaData", lm);
}

/* The actual unwrap + store, factored out so both the live login path and the
 * offline recovery path drive the exact same worker call and session writes.
 * `meta` carries keyId/publicKey/encryptedKeyChain/e2eeVersion/hashKeyChain
 * (qrCodeLoginV2 metaData on the live path; the persisted e2eeLoginMetaData on
 * recovery — same field names by construction). Returns 1 on success. */
static int unwrap_and_store(cJSON *s, cJSON *meta, const char *fn) {
  const char *pub = jstr(meta, "publicKey");
  const char *enc = jstr(meta, "encryptedKeyChain");
  const char *kid = jstr(meta, "keyId");
  cJSON *key_item = cJSON_GetObjectItem(s, "e2eeKeyId");
  ENILWorkerUnwrapResult u;
  int qr_key_id;

  char tag[64];
  snprintf(tag, sizeof(tag), "QrLogin.%s", fn ? fn : "?");

  if (!cJSON_IsNumber(key_item)) {
    ENIL_LOG(tag,
      "session e2eeKeyId missing/non-numeric — cannot unwrap "
      "(full re-login required)");
    return 0;
  }
  if (!pub || !pub[0] || !enc || !enc[0] || !kid || !kid[0]) {
    ENIL_LOG(tag,
      "metaData missing publicKey/encryptedKeyChain/keyId — cannot unwrap");
    return 0;
  }
  qr_key_id = key_item->valueint;

  {
    cJSON *restore = cJSON_GetObjectItem(s, "workerRestoreState");
    char *restore_str = restore ? cJSON_PrintUnformatted(restore) : NULL;
    int ok = enil_worker_unwrap_keychain(restore_str, qr_key_id, pub, enc, &u);
    free(restore_str);
    if (!ok) {
      ENIL_LOG(tag,
               "worker unwrap-keychain failed (qrKeyId=%d) — "
               "keychain material kept for offline retry",
               qr_key_id);
      return 0;
    }
  }

  /* e2eeKeys: worker returns exactly [{keyId,exportedKey}] — store as-is. */
  set_obj(s, "e2eeKeys", cJSON_Duplicate(u.keys, 1));
  set_num(s, "e2eeLatestKeyId", (double)atoi(kid));
  set_str(s, "e2eeLoginPublicKey", pub);
  set_str(s, "e2eeVersion", jstr_def(meta, "e2eeVersion", "1"));
  set_str(s, "e2eeHashKeyChain", jstr_def(meta, "hashKeyChain", ""));
  store_login_meta(s, meta);
  set_obj(s, "workerRestoreState", cJSON_Parse(u.worker_restore_state));
  cJSON_DeleteItemFromObject(s, "e2eeKeyCaptureError"); /* recovered/clean */
  enil_worker_unwrap_keychain_free(&u);
  ENIL_LOG(tag, "E2EE keychain captured (%d keys)",
           cJSON_GetArraySize(cJSON_GetObjectItem(s, "e2eeKeys")));
  return 1;
}

/* Port of buildE2eePatch + captureE2eeKeys. Best-effort: failure leaves the
 * account logged in without E2EE keys (decrypt will degrade, not crash) — but
 * now the raw metaData is persisted so enil_qrlogin_recover_e2ee() can retry
 * the unwrap later WITHOUT a rate-limited re-login. */
static void capture_e2ee_keys(cJSON *s, cJSON *data, int *out_captured) {
  cJSON *meta = cJSON_GetObjectItem(data, "metaData");
  const char *err = jstr(meta, "errorCode");

  *out_captured = 0;
  if (!meta) {
    LOG("capture_e2ee_keys", "qrCodeLoginV2 response has no metaData object");
    set_str(s, "e2eeKeyCaptureError", "no metaData in qrCodeLoginV2 response");
    return;
  }
  log_meta("capture_e2ee_keys", meta);

  /* Always persist the material first — even on a non-SUCCESS errorCode the
   * encryptedKeyChain is often present and recoverable once the phone-side
   * registration settles; losing it would force a re-login. */
  store_login_meta(s, meta);

  /* A populated keychain — not an errorCode string — is the success signal.
   * Mirror matrix-line (client.go), which proceeds whenever encryptedKeyChain
   * and publicKey are present. LINE only sets errorCode on FAILURE; on a
   * successful login the field is absent, so the old errorCode=="SUCCESS" gate
   * wrongly skipped the unwrap and left every E2EE message undecryptable. */
  {
    const char *enc = jstr(meta, "encryptedKeyChain");
    const char *pub = jstr(meta, "publicKey");
    if (err && err[0] && strcmp(err, "SUCCESS") != 0) {
      LOG("capture_e2ee_keys",
          "metaData reports errorCode — persisted for offline recovery");
      set_str(s, "e2eeKeyCaptureError", err);
      return;
    }
    if (!enc || !enc[0] || !pub || !pub[0]) {
      LOG("capture_e2ee_keys",
          "no encryptedKeyChain/publicKey in metaData — persisted for retry");
      set_str(s, "e2eeKeyCaptureError", "no keychain material in metaData");
      return;
    }
  }
  if (!unwrap_and_store(s, meta, "capture_e2ee_keys")) {
    set_str(s, "e2eeKeyCaptureError",
            "unwrap failed — metaData persisted for offline recovery");
    return;
  }
  *out_captured = 1;
}

static void clear_qr_state(cJSON *s, int keep_e2ee_runtime) {
  cJSON_DeleteItemFromObject(s, "authSessionId");
  cJSON_DeleteItemFromObject(s, "callbackUrl");
  cJSON_DeleteItemFromObject(s, "longPollingIntervalSec");
  cJSON_DeleteItemFromObject(s, "longPollingMaxCount");
  cJSON_DeleteItemFromObject(s, "createdAt");
  if (!keep_e2ee_runtime) {
    cJSON_DeleteItemFromObject(s, "e2eeKeyId");
    cJSON_DeleteItemFromObject(s, "e2eePublicKey");
    cJSON_DeleteItemFromObject(s, "e2eeKeyAuthSessionId");
  }
}

/* qrCodeLoginV2 → tokens + certificate + E2EE capture. */
static int finalize_login(cJSON *s) {
  cJSON *data, *issue;
  const char *access, *refresh, *cert;
  int captured = 0;
  const enil_identity_t *identity = enil_identity_current();

  if (!identity) return 0;

  {
    char *body;
    cJSON *req = cJSON_CreateArray();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "systemName", identity->system_name);
    cJSON_AddStringToObject(o, "modelName", identity->model_name);
    cJSON_AddBoolToObject(o, "autoLoginIsRequired", 0);
    cJSON_AddStringToObject(o, "authSessionId", jstr_def(s, "authSessionId", ""));
    cJSON_AddItemToArray(req, o);
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    data = body ? call_data(BASE "/qrCodeLoginV2", body) : NULL;
    free(body);
  }
  if (!data) { LOG("finalize_login", "qrCodeLoginV2 failed"); return 0; }

  issue   = cJSON_GetObjectItem(data, "tokenV3IssueResult");
  access  = jstr(issue, "accessToken");
  refresh = jstr_def(issue, "refreshToken", "");
  cert    = jstr_def(data, "certificate", "");
  if (!access) {
    LOG("finalize_login", "missing accessToken");
    cJSON_Delete(data);
    return 0;
  }
  set_str(s, "accessToken", access);
  set_str(s, "refreshToken", refresh);
  set_str(s, "certificate", cert);
  /* Persist the eta fields too — without these, scheduleNextTokenRefresh
   * skips on the first launch after a fresh QR scan and the token would
   * never refresh organically until the next manual tokenRefresh call.
   * Walk cJSON directly: jnum truncates to int but tokenIssueTimeEpochSec
   * is a full epoch second (near INT_MAX today, past it post-2038). */
  {
    cJSON *d = cJSON_GetObjectItem(issue, "durationUntilRefreshInSec");
    cJSON *i = cJSON_GetObjectItem(issue, "tokenIssueTimeEpochSec");
    if (cJSON_IsNumber(d) && d->valuedouble > 0)
      set_num(s, "durationUntilRefreshInSec", d->valuedouble);
    if (cJSON_IsNumber(i) && i->valuedouble > 0)
      set_num(s, "tokenIssueTimeEpochSec",    i->valuedouble);
  }

  capture_e2ee_keys(s, data, &captured);
  clear_qr_state(s, !captured);
  cJSON_Delete(data);
  return 1;
}

/* Port of postlogin.js — primes server state and records profile fields. */
static int post_login_handshake(cJSON *s, const char *token) {
  cJSON *profile;
  const char *mid, *name, *region;
  char body[256];

  profile = NULL;
  {
    ENILLineResponse r = enil_line_post(TALK "/getProfile",
                                        "[" SYNC_INITIALIZATION "]", token);
    if (r.body && r.status == 200) profile = parse_data(r.body);
    enil_line_response_free(&r);
  }
  if (!profile) { LOG("post_login_handshake", "getProfile failed"); return 0; }
  mid    = jstr(profile, "mid");
  name   = jstr(profile, "displayName");
  region = jstr_def(profile, "regionCode", "JP");

  { ENILLineResponse r = enil_line_post(TALK "/getEncryptedIdentityV3", "[]", token);
    enil_line_response_free(&r); }
  { ENILLineResponse r = enil_line_post(TALK "/getServerTime", "[]", token);
    enil_line_response_free(&r); }
  { ENILLineResponse r = enil_line_post(TALK "/getLastOpRevision", "[]", token);
    enil_line_response_free(&r); }
  snprintf(body, sizeof(body), "[\"\",\"\",\"\",\"%s\",\"\"," SYNC_INITIALIZATION "]",
           region);
  { ENILLineResponse r = enil_line_post(TALK "/getConfigurations", body, token);
    enil_line_response_free(&r); }
  { ENILLineResponse r = enil_line_post(TALK "/getSettings",
                                        "[" SYNC_INITIALIZATION "]", token);
    enil_line_response_free(&r); }

  if (mid)    set_str(s, "mid", mid);
  if (name)   set_str(s, "displayName", name);
  if (region) set_str(s, "regionCode", region);
  cJSON_Delete(profile);
  LOG("post_login_handshake", "handshake complete");
  return 1;
}

/* ---- public entry point -------------------------------------------------- */

int enil_qrlogin_run(const char *account_dir,
                     const enil_qrlogin_callbacks_t *cb) {
  char path[1024];
  cJSON *s;
  char *qr_url = NULL;
  const char *cb_url, *pubkey, *token;
  int rc = 0;
  enil_identity_t identity;

  if (!account_dir) { LOG("run", "NULL account_dir"); return 0; }
  if (!session_path_of(account_dir, path, sizeof(path))) {
    LOG("run", "account_dir too long");
    return 0;
  }

  if (!enil_identity_bind(NULL)) return 0;
  s = enil_session_read(path);
  /* Selection must have been persisted by prepare_login. Never start as
   * Chrome because the selected Windows staging file became unreadable. */
  if (!cJSON_IsObject(s) || !cJSON_GetObjectItemCaseSensitive(s, "clientIdentity")) {
    cJSON_Delete(s);
    emit_status(cb, "Invalid client identity");
    return 0;
  }
  if (!enil_identity_parse(cJSON_GetObjectItemCaseSensitive(s, "clientIdentity"),
                           &identity) || !enil_identity_bind(&identity)) {
    emit_status(cb, "Invalid client identity");
    cJSON_Delete(s);
    return 0;
  }
  if (!strcmp(identity.transport, "native-thrift")) {
    cJSON_Delete(s);
    rc = enil_windows_probe_run(account_dir, cb);
    if (rc) rc = enil_qrlogin_recover_e2ee(account_dir);
    enil_identity_bind(NULL);
    return rc;
  }
  /* Persist the exact identity before the first LINE request. */
  if (cancelled(cb) || !enil_session_write(path, s)) {
    cJSON_Delete(s);
    enil_identity_bind(NULL);
    return 0;
  }

  if (jstr(s, "accessToken")) {
    LOG("run", "already logged in");
    enil_identity_bind(NULL);
    cJSON_Delete(s);
    return 1;
  }

  /* This flow runs on its own detached thread that never binds an account
   * health (see enil_health_bind), so every LINE call here is inert with
   * respect to the sticky gate: an expired QR (a handled HTTP 400, not a
   * fault) cannot trip the gate and take down a logged-in account, and a
   * gate a dead account already tripped cannot block this fresh login. */
  emit_status(cb, "creating QR session");
  if (!ensure_qr_session(s)) goto done;
  if (!ensure_e2ee_key(s))   goto done;
  if (!enil_session_write(path, s)) goto done; /* QR + key state before poll */

  cb_url = jstr(s, "callbackUrl");
  pubkey = jstr(s, "e2eePublicKey");
  if (!cb_url || !pubkey) { LOG("run", "missing callbackUrl/publicKey"); goto done; }
  {
    size_t n = strlen(cb_url) + strlen(pubkey) + 32;
    qr_url = (char *)malloc(n);
    if (!qr_url) goto done;
    snprintf(qr_url, n, "%s?secret=%s&e2eeVersion=1", cb_url, pubkey);
  }
  if (cb && cb->on_qr_url) cb->on_qr_url(qr_url, cb->ctx);

  if (!poll_for_scan(s, cb)) goto done;
  if (cancelled(cb)) { LOG("run", "cancelled after scan"); goto done; }
  emit_status(cb, "scan detected — finalizing");

  if (!verify_certificate(s)) {
    LOG("run", "certificate rejected - PIN verification");
    if (!run_pin_flow(s, cb)) goto done;
  }

  if (cancelled(cb)) { LOG("run", "cancelled before finalize"); goto done; }
  if (!finalize_login(s)) goto done;
  token = jstr(s, "accessToken");
  if (!token) goto done;

  emit_status(cb, "running post-login handshake");
  if (!post_login_handshake(s, token)) goto done;

  if (!enil_session_write(path, s)) goto done;
  emit_status(cb, "logged in");
  LOG("run", "logged in successfully");
  rc = 1;

done:
  if (!rc) enil_session_write(path, s); /* keep resumable QR/key state */
  free(qr_url);
  enil_identity_bind(NULL);
  cJSON_Delete(s);
  return rc;
}

/* Offline E2EE keychain recovery — no network to LINE, no re-login. Used when
 * a prior login succeeded (accessToken present) but capture_e2ee_keys failed
 * and persisted the raw metaData. The channel is pure ECDH (static QR private
 * key + static peer public key), so re-running the worker unwrap against the
 * preserved e2eeLoginMetaData + post-keygen workerRestoreState + e2eeKeyId
 * yields the same keychain it would have the first time — see CLAUDE.md
 * "Re-decryption is safe and idempotent".
 *
 * Returns: 1 = keys present (already had them, or recovered now);
 *          0 = unrecoverable here (a full re-login is required).
 * Idempotent and a cheap no-op when e2eeKeys already exist, so it is safe to
 * call unconditionally on every account open. */
int enil_qrlogin_recover_e2ee(const char *account_dir) {
  char path[1024];
  cJSON *s, *keys, *meta;
  int rc = 0;

  if (!account_dir) { LOG("recover_e2ee", "NULL account_dir"); return 0; }
  if (!session_path_of(account_dir, path, sizeof(path))) {
    LOG("recover_e2ee", "account_dir too long");
    return 0;
  }
  s = enil_session_read(path);
  if (!s) return 0; /* no session at all — caller handles via normal validate */

  if (!jstr(s, "accessToken")) { cJSON_Delete(s); return 0; } /* not logged in */

  keys = cJSON_GetObjectItem(s, "e2eeKeys");
  if (cJSON_IsArray(keys) && cJSON_GetArraySize(keys) > 0) {
    cJSON_Delete(s);
    return 1; /* already captured — nothing to do */
  }

  meta = cJSON_GetObjectItem(s, "e2eeLoginMetaData");
  if (!meta) {
    cJSON *native = cJSON_GetObjectItem(s, "nativeLoginResult");
    meta = cJSON_GetObjectItem(native, "10");
    if (meta) { meta = cJSON_Duplicate(meta, 1); cJSON_AddItemToObject(s, "e2eeLoginMetaData", meta); }
  }
  if (!meta) {
    LOG("recover_e2ee",
        "no e2eeKeys and no persisted metaData — full re-login required");
    cJSON_Delete(s);
    return 0;
  }

  LOG("recover_e2ee", "attempting offline keychain recovery from saved metaData");
  log_meta("recover_e2ee", meta);
  if (unwrap_and_store(s, meta, "recover_e2ee")) {
    /* Same teardown the success login path does once E2EE is captured. */
    clear_qr_state(s, 0);
    enil_session_write(path, s);
    LOG("recover_e2ee", "offline recovery succeeded - session.json updated");
    rc = 1;
  } else {
    LOG("recover_e2ee", "offline recovery failed - full re-login required");
  }
  cJSON_Delete(s);
  return rc;
}
