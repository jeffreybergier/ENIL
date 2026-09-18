/* Real Worker HTTP and native polling, forcibly routed to a loopback fixture. */
#include "enil_health.h"
#include "enil_http.h"
#include "enil_identity.h"
#include "enil_line.h"
#include "enil_session.h"
#include "enil_sse.h"
#include "enil_worker.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *origin;
static long long saved_revision = 10;

CURLcode __real_curl_easy_perform(CURL *curl);
CURLcode __wrap_curl_easy_perform(CURL *curl) {
  char *url = NULL, local[1024];
  const char *path;
  assert(origin && !strncmp(origin, "http://127.0.0.1:", 17));
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
  assert(url && !strncmp(url, "https://", 8));
  path = strchr(url + 8, '/');
  assert(path);
  assert(snprintf(local, sizeof(local), "%s%s", origin, path) < (int)sizeof(local));
  curl_easy_setopt(curl, CURLOPT_URL, local);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  return __real_curl_easy_perform(curl);
}

void enil_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void enil_status_post(const char *key, const char *message, int indeterminate) {
  (void)key; (void)message; (void)indeterminate;
}
void enil_status_post_error(const char *key, const char *message) {
  (void)key; (void)message;
}
int enil_db_set_local_rev(sqlite3 *db, long long revision) {
  (void)db;
  saved_revision = revision;
  return SQLITE_OK;
}
static int event(const ENILSSEEvent *ev, void *ctx) {
  (void)ev; (void)ctx;
  assert(0); /* A cancelled response must never deliver or acknowledge events. */
  return 0;
}

int main(int argc, char **argv) {
  enil_identity_t identity;
  enil_health_t *health;
  ENILSSEClient *client;
  cJSON *root;
  int command;
  assert(argc == 3);
  origin = argv[1];
  health = enil_health_create("synthetic");
  assert(health);
  enil_health_bind(health);
  enil_worker_set_credentials("https://worker.invalid", "synthetic-secret");
  assert(enil_identity_default("desktopwin", &identity));
  root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "clientIdentity", enil_identity_to_json(&identity));
  cJSON_AddStringToObject(root, "accessToken", "synthetic-token");
  assert(enil_session_write(argv[2], root));
  cJSON_Delete(root);
  client = enil_sse_create("synthetic-token", 10, argv[2], (sqlite3 *)1, health);
  assert(client && enil_sse_start(client, event, NULL));
  while ((command = getchar()) == 'k') enil_sse_kick(client);
  assert(command == 'q');
  enil_sse_free(client);
  assert(!enil_health_any_failed() && saved_revision == 10);
  root = enil_session_read(argv[2]);
  assert(!cJSON_HasObjectItem(root, "nativeGlobalRevision"));
  assert(!cJSON_HasObjectItem(root, "nativeIndividualRevision"));
  cJSON_Delete(root);
  enil_health_destroy(health);
  return 0;
}
