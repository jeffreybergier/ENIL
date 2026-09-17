/* Compact Thrift QR protocol, reference: LINEJS ef6c3d9, base/login/mod.ts.
 * No extension code. E2EE operations stay in the existing crypto Worker.
 * Shared native QR flow; account activation is handled by ENILAccount. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "enil_windows_probe.h"
#include "enil_http.h"
#include "enil_identity.h"
#include "enil_session.h"
#include "enil_worker.h"
#include "enil_b64.h"
#include "enil_cocoa_log.h"

#define MAX_WIRE (2U * 1024U * 1024U)
#define LOGIN_URL "https://legy.line-apps.com/acct/lgn/sq/v1"
#define POLL_URL "https://legy.line-apps.com/acct/lp/lgn/sq/v1"

typedef struct {
  const unsigned char *data;
  size_t length, offset;
  int bad;
} Reader;

typedef struct {
  const enil_qrlogin_callbacks_t *cb;
  cJSON *state;
  char path[2048];
  int error_code;
  long http_status;
  CURLcode curl_code;
  int failed;
} Probe;

static const char *str(cJSON *o, const char *key) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}
static void set(cJSON *o, const char *key, cJSON *owned) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, key);
  if (owned) cJSON_AddItemToObject(o, key, owned);
}
static void status(Probe *p, const char *message) {
  ENIL_LOG("WindowsProbe.status", "%s", message);
  if (p->cb && p->cb->on_status) p->cb->on_status(message, p->cb->ctx);
}
static int cancelled(Probe *p) { return p->cb && p->cb->cancel && *p->cb->cancel; }
static int fail(Probe *p, const char *message) {
  p->failed = 1;
  status(p, message);
  return 0;
}

/* Atomic replacement, flushed before the next network step. Mode 0600 even
 * under a permissive app umask; a .tmp recovery copy is never a login trigger. */
static int save(Probe *p) {
  char temporary[2056];
  char *json = cJSON_Print(p->state);
  size_t offset = 0, length;
  int fd, ok = 1;
  if (!json) return fail(p, "Could not save Windows session");
  length = strlen(json);
  snprintf(temporary, sizeof(temporary), "%s.tmp", p->path);
  fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) { free(json); return fail(p, "Could not save Windows session"); }
  if (fchmod(fd, 0600) != 0) ok = 0;
  while (ok && offset < length) {
    ssize_t n = write(fd, json + offset, length - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { ok = 0; break; }
    offset += (size_t)n;
  }
  if (ok && fsync(fd) != 0) ok = 0;
  if (close(fd) != 0) ok = 0;
  if (ok && rename(temporary, p->path) != 0) ok = 0;
  free(json);
  if (!ok) return fail(p, "Could not save Windows session");
  return 1;
}

static int append(ENILBuf *b, const void *data, size_t n) {
  char *grown;
  if (n > MAX_WIRE || b->size > MAX_WIRE - n) return 0;
  grown = (char *)realloc(b->data, b->size + n + 1);
  if (!grown) return 0;
  b->data = grown;
  if (n) memcpy(b->data + b->size, data, n);
  b->size += n;
  b->data[b->size] = 0;
  return 1;
}
static int byte(ENILBuf *b, unsigned char value) { return append(b, &value, 1); }
static int varint(ENILBuf *b, uint64_t value) {
  do {
    unsigned char v = (unsigned char)(value & 127U);
    value >>= 7;
    if (!byte(b, (unsigned char)(v | (value ? 128U : 0U)))) return 0;
  } while (value);
  return 1;
}
static int string_field(ENILBuf *b, const char *value) {
  return value && byte(b, 0x18) && varint(b, strlen(value)) && append(b, value, strlen(value));
}
static int encode(const char *method, cJSON *s, ENILBuf *out) {
  const unsigned char header[] = {0x82, 0x21, 0};
  if (!append(out, header, sizeof(header)) || !varint(out, strlen(method)) ||
      !append(out, method, strlen(method))) return 0;
  if (strcmp(method, "createSession") == 0) return byte(out, 0);
  if (!byte(out, 0x1c) || !string_field(out, str(s, "authSessionId"))) return 0;
  if (strcmp(method, "verifyCertificate") == 0 && !string_field(out, "")) return 0;
  if (strcmp(method, "qrCodeLoginV2ForSecure") == 0) {
    enil_identity_t identity;
    if (!enil_identity_parse(cJSON_GetObjectItemCaseSensitive(s, "clientIdentity"), &identity)) return 0;
    if (!string_field(out, identity.system_name) || !string_field(out, identity.model_name) ||
        !byte(out, 0x12) || !string_field(out, str(s, "nativeQrNonce"))) return 0;
  }
  return byte(out, 0) && byte(out, 0);
}

static unsigned char read_byte(Reader *r) {
  if (r->offset >= r->length) { r->bad = 1; return 0; }
  return r->data[r->offset++];
}
static uint64_t read_varint(Reader *r) {
  uint64_t value = 0;
  unsigned shift;
  for (shift = 0; shift < 70; shift += 7) {
    unsigned char b = read_byte(r);
    if (r->bad || (shift == 63 && (b & 0xfeU))) break;
    value |= (uint64_t)(b & 127U) << shift;
    if (!(b & 128U)) return value;
  }
  r->bad = 1;
  return 0;
}
static int64_t read_integer(Reader *r) {
  uint64_t v = read_varint(r);
  return (int64_t)(v >> 1) ^ -(int64_t)(v & 1U);
}
static char *read_string(Reader *r) {
  uint64_t n = read_varint(r);
  char *s;
  if (r->bad || n > r->length - r->offset || n > MAX_WIRE) { r->bad = 1; return NULL; }
  s = (char *)malloc((size_t)n + 1);
  if (!s) { r->bad = 1; return NULL; }
  memcpy(s, r->data + r->offset, (size_t)n);
  s[n] = 0;
  r->offset += (size_t)n;
  if (memchr(s, 0, (size_t)n)) { free(s); r->bad = 1; return NULL; }
  return s;
}
static cJSON *read_value(Reader *r, unsigned kind, unsigned depth);
static cJSON *read_struct(Reader *r, unsigned depth) {
  cJSON *o = cJSON_CreateObject();
  int64_t previous = 0;
  unsigned count;
  if (!o) { r->bad = 1; return NULL; }
  for (count = 0; count < 10000 && !r->bad; count++) {
    unsigned char field = read_byte(r);
    char key[32];
    cJSON *v;
    int64_t id;
    if (!field) return o;
    id = (field >> 4) ? previous + (field >> 4) : read_integer(r);
    if (id < -32768 || id > 32767) { r->bad = 1; break; }
    previous = id;
    snprintf(key, sizeof(key), "%d", (int)id);
    if (cJSON_GetObjectItemCaseSensitive(o, key)) { r->bad = 1; break; }
    v = read_value(r, field & 15U, depth + 1);
    if (!v || !cJSON_AddItemToObject(o, key, v)) {
      cJSON_Delete(v); r->bad = 1; break;
    }
  }
  r->bad = 1;
  cJSON_Delete(o);
  return NULL;
}
static cJSON *read_value(Reader *r, unsigned kind, unsigned depth) {
  cJSON *v = NULL;
  char number[32];
  if (depth > 32) { r->bad = 1; return NULL; }
  switch (kind) {
    case 1: case 2: v = cJSON_CreateBool(kind == 1); break;
    case 3: v = cJSON_CreateNumber((signed char)read_byte(r)); break;
    case 4: case 5: v = cJSON_CreateNumber((double)read_integer(r)); break;
    case 6:
      snprintf(number, sizeof(number), "%lld", (long long)read_integer(r));
      v = cJSON_CreateString(number); break;
    case 7: {
      uint64_t bits = 0;
      double d;
      unsigned i;
      for (i = 0; i < 8; i++) bits |= (uint64_t)read_byte(r) << (i * 8);
      memcpy(&d, &bits, sizeof(d));
      v = cJSON_CreateNumber(d); break;
    }
    case 8: {
      char *s = read_string(r);
      if (s) { v = cJSON_CreateString(s); free(s); }
      break;
    }
    case 9: case 10: {
      unsigned char header = read_byte(r);
      uint64_t count = header >> 4, i;
      unsigned type = header & 15U;
      if (count == 15) count = read_varint(r);
      if (count > 10000) { r->bad = 1; break; }
      v = cJSON_CreateArray();
      for (i = 0; v && i < count && !r->bad; i++) {
        cJSON *item = read_value(r, type == 1 ? read_byte(r) : type, depth + 1);
        if (!item || !cJSON_AddItemToArray(v, item)) { cJSON_Delete(item); r->bad = 1; }
      }
      break;
    }
    case 11: {
      uint64_t count = read_varint(r), i;
      unsigned types = count ? read_byte(r) : 0;
      if (count > 10000 || (count && (types >> 4) != 8)) { r->bad = 1; break; }
      v = cJSON_CreateObject();
      for (i = 0; v && i < count && !r->bad; i++) {
        char *key = read_string(r);
        unsigned type = types & 15U;
        cJSON *item = read_value(r, type == 1 ? read_byte(r) : type, depth + 1);
        if (!key || !item || !cJSON_AddItemToObject(v, key, item)) {
          cJSON_Delete(item); r->bad = 1;
        }
        free(key);
      }
      break;
    }
    case 12: v = read_struct(r, depth); break;
    default: r->bad = 1; break;
  }
  if (!v || r->bad) { cJSON_Delete(v); r->bad = 1; return NULL; }
  return v;
}
static cJSON *decode(const char *method, const ENILBuf *wire, int *exception) {
  Reader r;
  unsigned char header;
  uint64_t sequence;
  char *name;
  cJSON *o;
  r.data = (const unsigned char *)wire->data; r.length = wire->size; r.offset = 0; r.bad = 0;
  if (read_byte(&r) != 0x82) return NULL;
  header = read_byte(&r);
  if (header != 0x41 && header != 0x61) return NULL;
  *exception = header == 0x61;
  sequence = read_varint(&r);
  name = read_string(&r);
  if (!name || strcmp(name, method) || sequence != 0) { free(name); return NULL; }
  free(name);
  o = read_value(&r, 12, 0);
  if (r.length - r.offset == 1 && r.data[r.offset] == 0) r.offset++;
  if (r.bad || r.offset != r.length) { cJSON_Delete(o); return NULL; }
  return o;
}

static size_t receive(void *data, size_t size, size_t count, void *ctx) {
  if (size && count > MAX_WIRE / size) return 0;
  return append((ENILBuf *)ctx, data, size * count) ? size * count : 0;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  (void)a; (void)b; (void)c; (void)d;
  return cancelled((Probe *)ctx);
}

/* Raw reply is committed BEFORE decoding, including the token-issuing reply.
 * Never log payloads: they can contain tokens or account information. */
static cJSON *call(Probe *p, const char *method, int polling, int interval) {
  ENILBuf request = {NULL, 0}, response = {NULL, 0};
  CURL *curl = NULL;
  struct curl_slist *headers = NULL;
  enil_identity_t id;
  char app[192], access[512], timeout[64], message[128];
  cJSON *root = NULL, *result = NULL, *error, *code;
  char *raw = NULL;
  int exception = 0;
  p->error_code = -1; p->http_status = 0; p->curl_code = CURLE_OK;
  if (cancelled(p)) goto done;
  if (!encode(method, p->state, &request) || !enil_identity_parse(cJSON_GetObjectItemCaseSensitive(p->state, "clientIdentity"), &id)) goto done;
  curl = enil_curl_new_raw();
  if (!curl) goto done;
  headers = curl_slist_append(headers, "Content-Type: application/x-thrift");
  headers = curl_slist_append(headers, "Accept: application/x-thrift");
  headers = curl_slist_append(headers, "X-LPV: 1");
  headers = curl_slist_append(headers, "X-LHM: POST");
  headers = curl_slist_append(headers, "X-LAL: en_US");
  snprintf(app, sizeof(app), "X-Line-Application: %s", id.application);
  headers = curl_slist_append(headers, app);
  if (polling) {
    const char *sid = str(p->state, "authSessionId");
    if (!sid || strlen(sid) > 450 || strpbrk(sid, "\r\n")) goto done;
    snprintf(access, sizeof(access), "X-Line-Access: %s", sid);
    snprintf(timeout, sizeof(timeout), "X-LST: %d", interval * 1000);
    headers = curl_slist_append(headers, access);
    headers = curl_slist_append(headers, timeout);
  }
  curl_easy_setopt(curl, CURLOPT_URL, polling ? POLL_URL : LOGIN_URL);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, id.user_agent);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.data);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request.size);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, polling ? (long)interval + 10 : 30L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, p);
  ENIL_LOG("WindowsProbe.call", "POST %s %s", polling ? POLL_URL : LOGIN_URL, method);
  p->curl_code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &p->http_status);
  ENIL_LOG("WindowsProbe.call", "%s HTTP %ld curl=%d bytes=%lu", method,
           p->http_status, (int)p->curl_code, (unsigned long)response.size);
  raw = enil_b64_encode((const unsigned char *)response.data, response.size);
  if (!raw) goto done;
  set(p->state, "lastNativeMethod", cJSON_CreateString(method));
  set(p->state, "lastNativeHttpStatus", cJSON_CreateNumber((double)p->http_status));
  set(p->state, "lastNativeResponseBase64", cJSON_CreateString(raw));
  if (!save(p)) goto done;
  if (p->curl_code != CURLE_OK || p->http_status != 200) goto done;
  root = decode(method, &response, &exception);
  if (!root) { fail(p, "Invalid Windows login response; reply saved"); goto done; }
  set(p->state, "lastNativeResponse", cJSON_Duplicate(root, 1));
  if (!save(p)) goto done;
  error = exception ? root : cJSON_GetObjectItemCaseSensitive(root, "1");
  if (error) {
    code = cJSON_GetObjectItemCaseSensitive(error, exception ? "2" : "1");
    p->error_code = !exception && cJSON_IsNumber(code) ? code->valueint : -1;
    snprintf(message, sizeof(message), "Windows login: %s error %d", method, p->error_code);
    status(p, message);
    goto done;
  }
  result = cJSON_DetachItemFromObjectCaseSensitive(root, "0");
  if (!result && (polling || strcmp(method, "verifyCertificate") == 0)) result = cJSON_CreateObject();
done:
  free(raw); cJSON_Delete(root);
  curl_slist_free_all(headers);
  if (curl) curl_easy_cleanup(curl);
  enil_buf_free(&request); enil_buf_free(&response);
  if (!result && !p->failed && !cancelled(p) && p->error_code < 0) {
    snprintf(message, sizeof(message), "Windows login: %s HTTP %ld, transport %d",
             method, p->http_status, (int)p->curl_code);
    status(p, message);
  }
  return result;
}

static int poll(Probe *p, const char *method, int count, int interval) {
  int i;
  for (i = 0; i < count && !cancelled(p); i++) {
    cJSON *result;
    time_t started = time(NULL);
    char message[100];
    snprintf(message, sizeof(message), "waiting for scan (%d/%d)", i + 1, count);
    if (strcmp(method, "checkQrCodeVerified") == 0) status(p, message);
    result = call(p, method, 1, interval);
    if (result) { cJSON_Delete(result); return 1; }
    if (p->failed || (p->curl_code != CURLE_OPERATION_TIMEDOUT && p->http_status != 408)) return 0;
    while (!cancelled(p) && time(NULL) - started < interval) sleep(1);
  }
  return 0;
}

static void copy_field(cJSON *dest, const char *name, cJSON *source, const char *key) {
  cJSON *value = cJSON_GetObjectItemCaseSensitive(source, key);
  if (value) set(dest, name, cJSON_Duplicate(value, 1));
}

static int save_login_result(Probe *p, cJSON *result) {
  cJSON *s = p->state;
  cJSON *tokens = cJSON_GetObjectItemCaseSensitive(result, "3");
  cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "10");
  set(s, "nativeLoginResult", cJSON_Duplicate(result, 1));
  copy_field(s, "accessToken", tokens, "1");
  copy_field(s, "refreshToken", tokens, "2");
  copy_field(s, "durationUntilRefreshInSec", tokens, "3");
  copy_field(s, "tokenIssueTimeEpochSec", tokens, "6");
  copy_field(s, "certificate", result, "1");
  copy_field(s, "mid", result, "4");
  copy_field(s, "e2eeLoginMetaData", result, "10");
  copy_field(s, "e2eeLoginPublicKey", meta, "publicKey");
  copy_field(s, "e2eeVersion", meta, "e2eeVersion");
  copy_field(s, "e2eeHashKeyChain", meta, "hashKeyChain");
  return save(p);
}

/* Recover if the app was interrupted after flushing the raw token reply but
 * before mapping it into session fields. This path makes zero network calls. */
static int recover_login_reply(Probe *p) {
  const char *method = str(p->state, "lastNativeMethod");
  const char *base64 = str(p->state, "lastNativeResponseBase64");
  unsigned char *bytes = NULL;
  ENILBuf wire;
  cJSON *root, *result;
  int size, exception = 0, ok = 0;
  if (!method || strcmp(method, "qrCodeLoginV2ForSecure") || !base64) return 0;
  size = enil_b64_decode_alloc(base64, &bytes);
  if (size <= 0) { free(bytes); return 0; }
  wire.data = (char *)bytes; wire.size = (size_t)size;
  root = decode(method, &wire, &exception);
  result = root ? cJSON_GetObjectItemCaseSensitive(root, "0") : NULL;
  if (!exception && str(cJSON_GetObjectItemCaseSensitive(result, "3"), "1"))
    ok = save_login_result(p, result);
  cJSON_Delete(root); free(bytes);
  return ok;
}

int enil_windows_probe_run(const char *directory, const enil_qrlogin_callbacks_t *cb) {
  Probe p;
  char lock_path[2048];
  int lock_fd = -1, ok = 0, count = 12, interval = 30;
  cJSON *s = NULL, *result = NULL, *meta, *value;
  ENILWorkerKeygenResult key;
  CURL *escape = NULL;
  char *encoded = NULL, *qr_url = NULL, *restore = NULL;
  const char *callback, *public_key;
  memset(&p, 0, sizeof(p)); memset(&key, 0, sizeof(key)); p.cb = cb;
  if (!directory || strlen(directory) > 1950) return 0;
  if (mkdir(directory, 0700) != 0 && errno != EEXIST) return fail(&p, "Could not save Windows session");
  if (chmod(directory, 0700) != 0) return fail(&p, "Could not save Windows session");
  snprintf(p.path, sizeof(p.path), "%s/session.json", directory);
  snprintf(lock_path, sizeof(lock_path), "%s/session.lock", directory);
  lock_fd = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
  if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    if (lock_fd >= 0) close(lock_fd);
    return fail(&p, "Windows login test is already running");
  }
  s = enil_session_read(p.path);
  if (!s && access(p.path, F_OK) == 0) { fail(&p, "Saved Windows session cannot be read; no new login attempted"); goto done; }
  if (!s) s = cJSON_CreateObject();
  p.state = s;
  if (!cJSON_IsObject(s)) { fail(&p, "Saved Windows session cannot be read; no new login attempted"); goto done; }
  if (str(s, "accessToken")) {
    status(&p, "Windows session already saved. No new login requested."); ok = 1; goto done;
  }
  if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "nativeTokenRequestStarted"))) {
    if (recover_login_reply(&p)) {
      status(&p, "Windows session already saved. No new login requested."); ok = 1; goto done;
    }
    fail(&p, "Previous login reply saved. No new login attempted."); goto done;
  }
  if (cancelled(&p)) goto done;
  set(s, "nativeTransport", cJSON_CreateString("compact-thrift"));
  if (!cJSON_GetObjectItemCaseSensitive(s, "clientIdentity")) {
    enil_identity_t id;
    cJSON *identity;
    if (!enil_identity_default("desktopwin", &id)) goto done;
    identity = enil_identity_to_json(&id);
    if (!identity) goto done;
    set(identity, "transport", cJSON_CreateString("native-thrift"));
    set(s, "clientIdentity", identity);
  }
  if (!save(&p)) goto done;
  if (!str(s, "e2eePublicKey")) {
    status(&p, "Preparing Windows login key");
    if (!enil_worker_keygen(NULL, &key)) { fail(&p, "Windows login key generation failed"); goto done; }
    set(s, "e2eePublicKey", cJSON_CreateString(key.public_key));
    set(s, "e2eeKeyId", cJSON_CreateNumber(key.key_id));
    set(s, "workerRestoreState", cJSON_Parse(key.worker_restore_state));
    enil_worker_keygen_free(&key);
    if (!save(&p)) goto done;
  }
  if (!str(s, "authSessionId")) {
    status(&p, "Creating native Windows QR session");
    result = call(&p, "createSession", 0, 0);
    if (!result || !str(result, "1")) goto done;
    copy_field(s, "authSessionId", result, "1");
    cJSON_Delete(result); result = NULL;
    if (!save(&p)) goto done;
  }
  if (!str(s, "callbackUrl")) {
    result = call(&p, "createQrCodeForSecure", 0, 0);
    if (!result || !str(result, "1") || !str(result, "4")) goto done;
    copy_field(s, "callbackUrl", result, "1");
    copy_field(s, "nativeQrNonce", result, "4");
    copy_field(s, "longPollingMaxCount", result, "2");
    copy_field(s, "longPollingIntervalSec", result, "3");
    set(s, "createdAt", cJSON_CreateNumber((double)time(NULL)));
    cJSON_Delete(result); result = NULL;
    if (!save(&p)) goto done;
  }
  value = cJSON_GetObjectItemCaseSensitive(s, "longPollingMaxCount");
  if (cJSON_IsNumber(value)) count = value->valueint;
  value = cJSON_GetObjectItemCaseSensitive(s, "longPollingIntervalSec");
  if (cJSON_IsNumber(value)) interval = value->valueint;
  if (count < 1 || count > 120 || interval < 1 || interval > 180 || count * interval > 1800) {
    fail(&p, "Invalid Windows QR polling limits"); goto done;
  }
  callback = str(s, "callbackUrl"); public_key = str(s, "e2eePublicKey");
  if (!callback || !public_key || !str(s, "nativeQrNonce")) goto done;
  escape = enil_curl_new_raw();
  if (!escape) goto done;
  encoded = curl_easy_escape(escape, public_key, 0);
  if (!encoded) goto done;
  qr_url = (char *)malloc(strlen(callback) + strlen(encoded) + 40);
  if (!qr_url) goto done;
  sprintf(qr_url, "%s%csecret=%s&e2eeVersion=1", callback, strchr(callback, '?') ? '&' : '?', encoded);
  if (cb && cb->on_qr_url && !cancelled(&p)) cb->on_qr_url(qr_url, cb->ctx);
  if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "nativeScanVerified"))) {
    if (!poll(&p, "checkQrCodeVerified", count, interval)) goto done;
    set(s, "nativeScanVerified", cJSON_CreateBool(1));
    if (!save(&p)) goto done;
  }
  if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "nativePinVerified"))) {
    result = call(&p, "verifyCertificate", 0, 0);
    if (!result) {
      const char *pin;
      if (p.failed || p.error_code != 2) goto done;
      result = call(&p, "createPinCode", 0, 0);
      pin = str(result, "1");
      if (!pin) goto done;
      if (cb && cb->on_pin) cb->on_pin(pin, cb->ctx);
      status(&p, "Enter the PIN in LINE on your phone");
      cJSON_Delete(result); result = NULL;
      if (!poll(&p, "checkPinCodeVerified", count, interval)) goto done;
    }
    cJSON_Delete(result); result = NULL;
    set(s, "nativePinVerified", cJSON_CreateBool(1));
    if (!save(&p)) goto done;
  }
  if (cancelled(&p)) goto done;
  set(s, "nativeTokenRequestStarted", cJSON_CreateBool(1));
  if (!save(&p)) goto done;
  status(&p, "Completing Windows login");
  result = call(&p, "qrCodeLoginV2ForSecure", 0, 0);
  if (!result) goto done;
  if (!save_login_result(&p, result)) goto done;
  if (!str(s, "accessToken")) { fail(&p, "Windows reply saved; no access token found"); goto done; }
  /* Every credential and the complete reply are durable before optional unwrap. */
  ok = 1;
  status(&p, "Windows login succeeded. Session saved.");
  meta = cJSON_GetObjectItemCaseSensitive(result, "10");
  if (str(meta, "publicKey") && str(meta, "encryptedKeyChain") && !cancelled(&p)) {
    ENILWorkerUnwrapResult unwrapped;
    cJSON *state = cJSON_GetObjectItemCaseSensitive(s, "workerRestoreState");
    const char *error = str(meta, "errorCode");
    if (error && strcmp(error, "0") && strcmp(error, "SUCCESS")) goto done;
    restore = state ? cJSON_PrintUnformatted(state) : NULL;
    value = cJSON_GetObjectItemCaseSensitive(s, "e2eeKeyId");
    if (restore && cJSON_IsNumber(value) && enil_worker_unwrap_keychain(
          restore, value->valueint, str(meta, "publicKey"), str(meta, "encryptedKeyChain"), &unwrapped)) {
      set(s, "e2eeKeys", cJSON_Duplicate(unwrapped.keys, 1));
      set(s, "workerRestoreState", cJSON_Parse(unwrapped.worker_restore_state));
      copy_field(s, "e2eeLatestKeyId", meta, "keyId");
      enil_worker_unwrap_keychain_free(&unwrapped);
      if (!save(&p)) ok = 0;
    }
  }
done:
  if (!ok && !p.failed && !cancelled(&p)) status(&p, "Windows login stopped. Session state saved; see log for details.");
  free(restore); free(qr_url);
  if (encoded) curl_free(encoded);
  if (escape) curl_easy_cleanup(escape);
  cJSON_Delete(result); cJSON_Delete(s);
  if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
  return ok;
}
