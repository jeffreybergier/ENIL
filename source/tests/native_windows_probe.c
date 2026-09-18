/* Real native probe against a mandatory loopback Thrift fixture. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "enil_native_login.h"
#include "enil_session.h"
#include "enil_http.h"
#include "enil_worker.h"

static const char *origin;
static const char *directory;
static int calls, keys, unwraps, qr_count, pins;
static int fail_unwrap;

void enil_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
CURLcode __real_curl_easy_perform(CURL *curl);
CURLcode __wrap_curl_easy_perform(CURL *curl) {
  char *url = NULL, target[2048];
  const char *path;
  assert(origin && strncmp(origin, "http://127.0.0.1:", 17) == 0);
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
  assert(url && strncmp(url, "https://legy.line-apps.com/", 26) == 0);
  path = strchr(url + 8, '/');
  assert(path);
  snprintf(target, sizeof(target), "%s%s", origin, path);
  curl_easy_setopt(curl, CURLOPT_URL, target);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  calls++;
  return __real_curl_easy_perform(curl);
}

int enil_worker_keygen(const char *state, ENILWorkerKeygenResult *out) {
  (void)state;
  keys++;
  out->key_id = 7;
  out->public_key = strdup("A+B/C==");
  out->worker_restore_state = strdup("{\"syntheticPrivate\":\"private\"}");
  return 1;
}
void enil_worker_keygen_free(ENILWorkerKeygenResult *out) {
  free(out->public_key); free(out->worker_restore_state);
  memset(out, 0, sizeof(*out));
}
static cJSON *read_saved(void) {
  char path[2048];
  struct stat st;
  cJSON *s;
  snprintf(path, sizeof(path), "%s/session.json", directory);
  assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
  s = enil_session_read(path);
  assert(s);
  return s;
}
int enil_worker_unwrap_keychain(const char *state, int id, const char *public_key,
                                const char *encrypted, ENILWorkerUnwrapResult *out) {
  cJSON *s = read_saved();
  (void)state;
  assert(id == 7 && strcmp(public_key, "peer") == 0 && strcmp(encrypted, "encrypted") == 0);
  /* Credentials are already durable when the optional Worker step begins. */
  assert(strcmp(cJSON_GetObjectItem(s, "accessToken")->valuestring, "private-access-token") == 0);
  assert(cJSON_GetObjectItem(s, "nativeLoginResult"));
  assert(cJSON_GetObjectItem(s, "workerRestoreState"));
  cJSON_Delete(s);
  unwraps++;
  if (fail_unwrap) return 0;
  out->keys = cJSON_Parse("[{\"keyId\":9,\"exportedKey\":\"synthetic\"}]");
  out->worker_restore_state = strdup("{\"unwrapped\":true}");
  return 1;
}
void enil_worker_unwrap_keychain_free(ENILWorkerUnwrapResult *out) {
  cJSON_Delete(out->keys); free(out->worker_restore_state);
}
static void qr(const char *url, void *ctx) {
  (void)ctx;
  qr_count++;
  assert(strstr(url, "secret=A%2BB%2FC%3D%3D"));
  assert(strstr(url, "existing=1&"));
}
static void pin(const char *value, void *ctx) {
  (void)ctx;
  pins++;
  assert(strcmp(value, "123456") == 0);
}
static void status(const char *message, void *ctx) {
  (void)ctx;
  assert(!strstr(message, "private-access-token"));
  assert(!strstr(message, "private-refresh-token"));
}
int main(int argc, char **argv) {
  enil_qrlogin_callbacks_t cb;
  volatile int cancel = 1;
  cJSON *s;
  int before;
  assert(argc == 4);
  directory = argv[1]; origin = argv[2]; fail_unwrap = strcmp(argv[3], "unwrap-fails") == 0;
  memset(&cb, 0, sizeof(cb));
  cb.on_qr_url = qr; cb.on_pin = pin; cb.on_status = status; cb.cancel = &cancel;
  assert(!enil_native_login_run(directory, &cb));
  assert(calls == 0 && keys == 0);
  cancel = 0;
  if (strcmp(argv[3], "token-error") == 0) {
    assert(!enil_native_login_run(directory, &cb));
    s = read_saved();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(s, "nativeTokenRequestStarted")));
    assert(cJSON_GetObjectItem(s, "lastNativeResponseBase64"));
    cJSON_Delete(s);
    before = calls;
    assert(!enil_native_login_run(directory, &cb));
    assert(calls == before && keys == 1);
    return 0;
  }
  assert(enil_native_login_run(directory, &cb));
  assert(qr_count == 1 && pins == 1 && keys == 1 && unwraps == 1);
  s = read_saved();
  assert(strcmp(cJSON_GetObjectItem(s, "refreshToken")->valuestring, "private-refresh-token") == 0);
  assert(strcmp(cJSON_GetObjectItem(s, "certificate")->valuestring, "certificate") == 0);
  assert(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(s, "clientIdentity"), "transport")->valuestring,
                "native-thrift") == 0);
  assert(cJSON_GetObjectItem(s, "nativeLoginResult"));
  assert(cJSON_GetObjectItem(s, "e2eeLoginMetaData"));
  if (!fail_unwrap) assert(cJSON_GetObjectItem(s, "e2eeKeys"));
  cJSON_Delete(s);
  before = calls;
  assert(enil_native_login_run(directory, &cb));
  assert(calls == before && keys == 1 && unwraps == 1);
  /* Simulate interruption between saving raw reply and mapping tokens. */
  s = read_saved();
  cJSON_DeleteItemFromObject(s, "accessToken");
  cJSON_DeleteItemFromObject(s, "refreshToken");
  cJSON_DeleteItemFromObject(s, "nativeLoginResult");
  {
    char path[2048];
    snprintf(path, sizeof(path), "%s/session.json", directory);
    assert(enil_session_write(path, s));
  }
  cJSON_Delete(s);
  assert(enil_native_login_run(directory, &cb));
  assert(calls == before && keys == 1 && unwraps == 1);
  s = read_saved();
  assert(strcmp(cJSON_GetObjectItem(s, "accessToken")->valuestring, "private-access-token") == 0);
  cJSON_Delete(s);
  return 0;
}
