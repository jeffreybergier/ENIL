/* Exercise the UI's shared login entry point with the real Worker client and
 * health gate. Only the configured loopback Worker endpoint is reachable. */
#include "enil_cocoa_progress.h"
#include "enil_health.h"
#include "enil_http.h"
#include "enil_login_store.h"
#include "enil_qrlogin.h"
#include "enil_session.h"
#include "enil_worker.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *origin;
static int requests;

CURLcode __real_curl_easy_perform(CURL *curl);
CURLcode __wrap_curl_easy_perform(CURL *curl) {
  char *url = NULL;
  assert(origin && !strncmp(origin, "http://127.0.0.1:", 17));
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
  assert(url && !strncmp(url, origin, strlen(origin)));
  assert(!strcmp(url + strlen(origin), "/e2ee/unwrap-keychain"));
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  requests++;
  return __real_curl_easy_perform(curl);
}

void enil_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void enil_status_post(const char *key, const char *message, int indeterminate) {
  (void)key; (void)message; (void)indeterminate;
}
void enil_status_post_error(const char *key, const char *message) {
  (void)key; (void)message;
}

static void same_credentials(cJSON *before, cJSON *after) {
  const char *fields[] = {"loginId", "accessToken", "refreshToken", "mid"};
  int i;
  for (i = 0; i < 4; i++)
    assert(cJSON_Compare(cJSON_GetObjectItem(before, fields[i]),
                         cJSON_GetObjectItem(after, fields[i]), 1));
}

int main(int argc, char **argv) {
  char staging[2048], pending_path[4096], staged_path[4096];
  char *pending;
  cJSON *patch, *before, *after;
  enil_health_t *account_health;
  enil_qrlogin_callbacks_t cb;
  volatile int cancel = 0;
  assert(argc == 3);
  origin = argv[2];
  assert(snprintf(staging, sizeof(staging), "%s/.staging-test", argv[1]) <
         (int)sizeof(staging));
  assert(mkdir(staging, 0700) == 0);
  assert(enil_login_store_prepare(staging, "desktopwin", NULL));
  pending = enil_login_store_select(staging);
  assert(pending);
  assert(snprintf(pending_path, sizeof(pending_path), "%s/session.json", pending) <
         (int)sizeof(pending_path));
  assert(snprintf(staged_path, sizeof(staged_path), "%s/session.json", staging) <
         (int)sizeof(staged_path));
  patch = cJSON_Parse(
    "{\"accessToken\":\"synthetic-access\",\"refreshToken\":\"synthetic-refresh\","
    "\"mid\":\"synthetic-mid\",\"nativeTokenRequestStarted\":true,\"e2eeKeyId\":42,"
    "\"workerRestoreState\":{\"synthetic\":true},\"e2eeLoginMetaData\":{"
    "\"keyId\":\"7\",\"publicKey\":\"synthetic-peer\","
    "\"encryptedKeyChain\":\"synthetic-keychain\"}}");
  assert(patch && enil_session_patch(pending_path, patch));
  cJSON_Delete(patch);
  before = enil_session_read(pending_path);
  assert(before);
  enil_worker_set_credentials(origin, "synthetic-secret");
  memset(&cb, 0, sizeof(cb));
  cb.cancel = &cancel;

  /* First attempt fails with HTTP 503 after credentials have been saved. */
  assert(!enil_qrlogin_run_user_attempt(staging, &cb));
  assert(requests == 1 && enil_health_is_failed(ENIL_ERR_WORKER));
  after = enil_session_read(pending_path);
  assert(cJSON_Compare(before, after, 1));
  cJSON_Delete(after);

  /* Automatic recovery and cancelled actions cannot reopen the gate. */
  assert(!enil_qrlogin_recover_e2ee(pending));
  cancel = 1;
  assert(!enil_qrlogin_run_user_attempt(staging, &cb));
  assert(requests == 1 && enil_health_is_failed(ENIL_ERR_WORKER));
  cancel = 0;

  /* An explicit retry makes exactly one new request. If it also fails, the
   * gate closes again rather than silently retrying within the attempt. */
  assert(!enil_qrlogin_run_user_attempt(staging, &cb));
  assert(requests == 2 && enil_health_is_failed(ENIL_ERR_WORKER));
  assert(!enil_qrlogin_recover_e2ee(pending));
  assert(requests == 2);

  /* A successful explicit retry recovers and stages the existing credentials,
   * without resetting another account's LINE failure or issuing a new QR. */
  account_health = enil_health_create("other-account");
  assert(account_health);
  enil_health_bind(account_health);
  enil_health_set_failure(ENIL_ERR_LINE, "synthetic LINE failure");
  assert(enil_qrlogin_run_user_attempt(staging, &cb));
  assert(requests == 3 && !enil_health_is_failed(ENIL_ERR_WORKER));
  assert(enil_health_is_failed(ENIL_ERR_LINE));
  after = enil_session_read(staged_path);
  same_credentials(before, after);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(after, "e2eeKeys")) == 1);
  cJSON_Delete(after);
  after = enil_session_read(pending_path);
  same_credentials(before, after);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(after, "e2eeKeys")) == 1);
  cJSON_Delete(after);
  assert(enil_qrlogin_recover_e2ee(pending));
  assert(requests == 3);
  enil_health_destroy(account_health);
  cJSON_Delete(before);
  free(pending);
  return 0;
}
