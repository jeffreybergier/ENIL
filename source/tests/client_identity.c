/* Host-only integration driver. Real native request/session code; synthetic
 * crypto and a mandatory loopback redirect. Never contacts LINE or the Worker. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "enil_identity.h"
#include "enil_http.h"
#include "enil_session.h"
#include "enil_line.h"
#include "enil_talkserv.h"
#include "enil_qrlogin.h"
#include "enil_obs.h"
#include "enil_sse.h"
#include "enil_worker.h"
#include "enil_cocoa_progress.h"

static const char *fixture_origin;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_ready = PTHREAD_COND_INITIALIZER;
static int received;

CURLcode __real_curl_easy_perform(CURL *curl);
CURLcode __wrap_curl_easy_perform(CURL *curl) {
  char *url = NULL, redirected[2048];
  const char *path;
  assert(fixture_origin && strncmp(fixture_origin, "http://127.0.0.1:", 17) == 0);
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
  assert(url && strncmp(url, "https://", 8) == 0);
  path = strchr(url + 8, '/');
  assert(path);
  snprintf(redirected, sizeof(redirected), "%s%s", fixture_origin, path);
  curl_easy_setopt(curl, CURLOPT_URL, redirected);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  return __real_curl_easy_perform(curl);
}

void enil_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int enil_health_is_failed(enil_err_source_t source) { (void)source; return 0; }
int enil_health_any_failed(void) { return 0; }
void enil_health_bind(enil_health_t *h) { (void)h; }
void enil_health_set_failure(enil_err_source_t s, const char *m) { (void)s; (void)m; }
int __wrap_enil_db_set_local_rev(sqlite3 *db, long long rev) { (void)db; (void)rev; return 0; }
char *enil_worker_sign(const char *p, const char *b, const char *t) {
  (void)p; (void)b; (void)t;
  assert(enil_identity_current());
  return strdup("synthetic-signature");
}
int enil_worker_keygen(const char *state, ENILWorkerKeygenResult *out) {
  (void)state;
  out->key_id = 1;
  out->public_key = strdup("synthetic-public-key");
  out->worker_restore_state = strdup("{}");
  return 1;
}
void enil_worker_keygen_free(ENILWorkerKeygenResult *out) {
  free(out->public_key); free(out->worker_restore_state);
}
int enil_worker_unwrap_keychain(const char *s, int id, const char *p,
                                const char *k, ENILWorkerUnwrapResult *out) {
  (void)s; (void)id; (void)p; (void)k;
  out->keys = cJSON_Parse("[{\"keyId\":1,\"exportedKey\":\"synthetic\"}]");
  out->worker_restore_state = strdup("{}");
  return 1;
}
void enil_worker_unwrap_keychain_free(ENILWorkerUnwrapResult *out) {
  cJSON_Delete(out->keys); free(out->worker_restore_state);
}

static int on_event(const ENILSSEEvent *event, void *ctx) {
  (void)event; (void)ctx;
  pthread_mutex_lock(&event_lock);
  received = 1;
  pthread_cond_signal(&event_ready);
  pthread_mutex_unlock(&event_lock);
  return 1;
}

static void *parallel_request(void *arg) {
  enil_identity_t id;
  ENILLineResponse response;
  int i;
  assert(enil_identity_default((const char *)arg, &id));
  strcpy(id.transport, "chrome-gateway"); /* legacy experiment snapshot */
  assert(enil_identity_bind(&id));
  for (i = 0; i < 5; ++i) {
    response = enil_line_post("/parallel", "[]", id.profile_id);
    assert(response.status == 200);
    enil_line_response_free(&response);
  }
  return NULL;
}

static void check_profile(const char *root, const char *profile) {
  char dir[1024], path[1200], staged[1200], media[1200];
  session_t session;
  enil_identity_t identity;
  cJSON *json, *snapshot;
  char *token, *oid = NULL;
  ENILSSEClient *sse;
  ENILLineResponse response;
  volatile int cancel = 1;
  enil_qrlogin_callbacks_t cb;
  int code;
  sticker_package_t *stickers = NULL;
  sticon_package_t *sticons = NULL;
  int count;

  snprintf(dir, sizeof(dir), "%s/%s", root, profile);
  assert(mkdir(dir, 0700) == 0);
  snprintf(path, sizeof(path), "%s/session.json", dir);
  assert(!enil_qrlogin_run(dir, NULL)); /* missing staging never defaults to Chrome */
  assert(enil_session_prepare_login(path, profile, NULL));
  json = enil_session_read(path);
  snapshot = cJSON_GetObjectItem(json, "clientIdentity");
  cJSON_ReplaceItemInObjectCaseSensitive(snapshot, "transport", cJSON_CreateString("chrome-gateway"));
  assert(enil_session_write(path, json)); cJSON_Delete(json);
  assert(!enil_session_prepare_login(path, "chrome", NULL));
  assert(enil_session_bind_identity(path));
  identity = *enil_identity_current();
  memset(&cb, 0, sizeof(cb));
  cb.cancel = &cancel;
  assert(!enil_qrlogin_run(dir, &cb)); /* no HTTP requests on cancellation */
  assert(!enil_identity_current());
  assert(enil_qrlogin_run(dir, NULL));
  assert(enil_session_load(path, &session));
  assert(memcmp(&identity, &session.clientIdentity, sizeof(identity)) == 0);
  enil_session_free(&session);

  assert(enil_session_bind_identity(path));
  assert(sticker_package_fetch_all(profile, &stickers, &count) == 0 && count == 0);
  assert(sticon_package_fetch_all(profile, &sticons, &count) == 0 && count == 0);
  free(stickers);
  free(sticons);
  enil_identity_bind(NULL);

  /* Prove exact persisted values survive refresh, not merely default lookup. */
  json = enil_session_read(path);
  snapshot = cJSON_GetObjectItemCaseSensitive(json, "clientIdentity");
  cJSON_ReplaceItemInObjectCaseSensitive(snapshot, "userAgent", cJSON_CreateString("saved-session-agent"));
  cJSON_AddItemToObject(json, "encryptedAccessTokens", cJSON_Parse("{\"2\":\"synthetic-obs-token\"}"));
  assert(enil_session_write(path, json));
  cJSON_Delete(json);
  token = enil_line_token_refresh(path, &code);
  assert(token && code == 0);
  free(token);
  assert(enil_line_send_chat_removed(path, "synthetic-chat", "9007199254740993",
                                      1800000000000LL));
  assert(enil_session_load(path, &session));
  assert(strcmp(session.clientIdentity.user_agent, "saved-session-agent") == 0);
  assert(strcmp(session.clientIdentity.profile_id, profile) == 0);
  enil_session_free(&session);

  snprintf(staged, sizeof(staged), "%s/reauth.json", dir);
  assert(enil_session_prepare_login(staged, NULL, path));
  json = enil_session_read(staged);
  assert(!cJSON_HasObjectItem(json, "accessToken"));
  assert(!cJSON_HasObjectItem(json, "certificate"));
  assert(enil_identity_parse(cJSON_GetObjectItemCaseSensitive(json, "clientIdentity"), &identity));
  assert(strcmp(identity.user_agent, "saved-session-agent") == 0);
  assert(strcmp(identity.profile_id, profile) == 0);
  cJSON_Delete(json);

  assert(enil_obs_upload(path, (const unsigned char *)"image", 5, "m", &oid) == 0);
  assert(oid); free(oid);
  assert(enil_obs_upload_with_oid(path, (const unsigned char *)"thumb", 5, "m", "test-preview") == 0);
  snprintf(media, sizeof(media), "%s/media.bin", dir);
  assert(enil_obs_download_message(path, "test-message", "m", "test-message",
                                   NULL, NULL, NULL, 0, media) == 0);
  token = enil_curl_get("https://profile.line-scdn.net/test-avatar");
  assert(token); free(token);

  received = 0;
  sse = enil_sse_create(profile, 1, path, NULL, NULL);
  assert(sse && enil_sse_start(sse, on_event, NULL));
  pthread_mutex_lock(&event_lock);
  while (!received) pthread_cond_wait(&event_ready, &event_lock);
  pthread_mutex_unlock(&event_lock);
  enil_sse_free(sse);

  /* A bad saved identity must clear an old thread binding and send nothing. */
  json = enil_session_read(path);
  cJSON_ReplaceItemInObjectCaseSensitive(json, "clientIdentity", cJSON_CreateNull());
  assert(enil_session_write(path, json)); cJSON_Delete(json);
  assert(!enil_session_bind_identity(path));
  assert(!enil_identity_current());
  response = enil_line_post("/must-not-send", "[]", "synthetic");
  assert(response.status == 0 && !response.body);
}

int main(int argc, char **argv) {
  enil_identity_t id;
  cJSON *json;
  session_t session;
  pthread_t chrome, windows;
  assert(argc == 4);
  fixture_origin = argv[2];
  if (strcmp(argv[3], "default")) enil_line_set_language(argv[3]);
  assert(enil_identity_parse(NULL, &id));
  assert(strcmp(id.profile_id, "chrome") == 0);
  assert(!enil_identity_default("unknown", &id));
  json = cJSON_Parse("{\"accessToken\":\"synthetic-legacy\"}");
  assert(enil_session_parse(json, &session));
  cJSON_Delete(json);
  json = enil_session_to_json(&session);
  assert(cJSON_HasObjectItem(json, "clientIdentity"));
  enil_session_free(&session); cJSON_Delete(json);
  assert(enil_identity_default("desktopwin", &id));
  assert(strcmp(id.transport, "native-thrift") == 0);
  json = enil_identity_to_json(&id);
  cJSON_ReplaceItemInObjectCaseSensitive(json, "userAgent", cJSON_CreateString("bad\r\nInjected: header"));
  assert(!enil_identity_parse(json, &id)); cJSON_Delete(json);
  assert(enil_identity_default("android", &id));
  json = enil_identity_to_json(&id);
  assert(enil_identity_parse(json, &id));
  /* Android secondary can neither become a primary phone nor use the gateway. */
  cJSON_ReplaceItemInObjectCaseSensitive(json, "application",
    cJSON_CreateString("ANDROID\t26.6.2\tAndroid OS\t16"));
  assert(!enil_identity_parse(json, &id)); cJSON_Delete(json);
  assert(enil_identity_default("android", &id));
  json = enil_identity_to_json(&id);
  cJSON_ReplaceItemInObjectCaseSensitive(json, "transport", cJSON_CreateString("chrome-gateway"));
  assert(!enil_identity_parse(json, &id)); cJSON_Delete(json);
  check_profile(argv[1], "chrome");
  check_profile(argv[1], "desktopwin");
  assert(pthread_create(&chrome, NULL, parallel_request, "chrome") == 0);
  assert(pthread_create(&windows, NULL, parallel_request, "desktopwin") == 0);
  pthread_join(chrome, NULL); pthread_join(windows, NULL);
  puts("Client identity integration checks passed");
  return 0;
}

cJSON *enil_worker_decrypt(const char *p, cJSON *v) { (void)p; (void)v; assert(0); return NULL; }
cJSON *enil_worker_decrypt_ex(const char *p, cJSON *v, const volatile int *cancel) {
  (void)cancel; return enil_worker_decrypt(p, v);
}
