/* Synthetic requests only. All LINE traffic is forcibly redirected to loopback. */
#include "enil_health.h"
#include "enil_http.h"
#include "enil_identity.h"
#include "enil_line.h"
#include "enil_login_store.h"
#include "enil_session.h"
#include "enil_talkserv.h"
#include "enil_sse.h"
#include "enil_thrift.h"
#include "enil_worker.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *origin;
static int health_failed, fail_event, event_count, requests;
static long long last_revision;
static const char *activate_staging, *activate_account;
CURLcode __real_curl_easy_perform(CURL *);
CURLcode __wrap_curl_easy_perform(CURL *c) {
  char *url = NULL, buf[1024];
  const char *path;
  assert(origin && !strncmp(origin, "http://127.0.0.1:", 17));
  curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &url);
  assert(url && !strncmp(url, "https://", 8));
  path = strchr(url + 8, '/');
  assert(path);
  snprintf(buf, sizeof(buf), "%s%s", origin, path);
  curl_easy_setopt(c, CURLOPT_URL, buf);
  curl_easy_setopt(c, CURLOPT_PROXY, "");
  requests++;
  {
    CURLcode rc = __real_curl_easy_perform(c);
    /* Deterministically activate B while A's refresh is still in flight,
     * before its reply reaches the refresh journal/session commit. */
    if (activate_staging) {
      assert(enil_login_store_activate(activate_staging, activate_account));
      activate_staging = NULL;
    }
    return rc;
  }
}
void enil_log(const char *t, const char *f, ...) {
  (void)t;
  (void)f;
}
int enil_health_is_failed(enil_err_source_t s) {
  (void)s;
  return health_failed;
}
int enil_health_any_failed(void) { return health_failed; }
void enil_health_bind(enil_health_t *h) { (void)h; }
int enil_db_set_local_rev(sqlite3 *db, long long rev) {
  (void)db;
  last_revision = rev;
  return 0;
}
void enil_health_set_failure(enil_err_source_t s, const char *m) {
  (void)s;
  (void)m;
  health_failed = 1;
}
char *enil_worker_sign(const char *p, const char *b, const char *t) {
  (void)p;
  (void)b;
  (void)t;
  assert(0);
  return NULL;
}
cJSON *enil_worker_decrypt_ex(const char *p, cJSON *v, const volatile int *cancel) {
  cJSON *r = cJSON_CreateObject();
  (void)cancel;
  cJSON_AddItemToObject(r, "body", cJSON_Duplicate(cJSON_GetObjectItem(v, "body"), 1));
  if (strstr(p, "encode")) {
    cJSON_AddStringToObject(r, "key", "synthetic");
    cJSON_AddStringToObject(r, "xLcs", "0008synthetic");
  } else
    cJSON_AddNumberToObject(r, "status", 200);
  return r;
}
static void persistence(const char *path) {
  session_t stale, fresh;
  enil_identity_t id;
  cJSON *root = cJSON_CreateObject(), *check;
  assert(enil_identity_default("desktopwin", &id));
  cJSON_AddItemToObject(root, "clientIdentity", enil_identity_to_json(&id));
  cJSON_AddStringToObject(root, "accessToken", "original");
  cJSON_AddStringToObject(root, "refreshToken", "original-refresh");
  cJSON_AddItemToObject(root, "nativeLoginResult",
                        cJSON_Parse("{\"10\":{\"encryptedKeyChain\":\"retained\"}}"));
  assert(enil_session_write(path, root));
  cJSON_Delete(root);
  assert(enil_session_load(path, &stale));
  assert(enil_session_load(path, &fresh));
  free(fresh.accessToken);
  fresh.accessToken = strdup("rotated");
  free(fresh.refreshToken);
  fresh.refreshToken = strdup("rotated-refresh");
  fresh.durationUntilRefreshInSec = 3600;
  fresh.tokenIssueTimeEpochSec = 1800000000;
  assert(enil_session_save(path, &fresh));
  stale.reqSeq = 9;
  assert(enil_session_save(path, &stale));
  check = enil_session_read(path);
  assert(!strcmp(cJSON_GetObjectItem(check, "accessToken")->valuestring, "rotated"));
  assert(!strcmp(cJSON_GetObjectItem(check, "refreshToken")->valuestring, "rotated-refresh"));
  assert(!strcmp(cJSON_GetObjectItem(check, "durationUntilRefreshInSec")->valuestring, "3600"));
  assert(cJSON_HasObjectItem(check, "nativeLoginResult"));
  assert(cJSON_GetObjectItem(check, "reqSeq")->valueint == 9);
  cJSON_Delete(check);
  enil_session_free(&stale);
  enil_session_free(&fresh);
}
static int event_cb(const ENILSSEEvent *ev, void *ctx) {
  (void)ctx;
  if (!strcmp(ev->type, "message")) {
    event_count++;
    if (fail_event)
      health_failed = 1;
  }
  return 1;
}
static void polling(const char *path, int fail) {
  enil_identity_t id;
  cJSON *root = cJSON_CreateObject(), *saved;
  ENILSSEClient *client;
  int i;
  assert(enil_identity_default("desktopwin", &id));
  cJSON_AddItemToObject(root, "clientIdentity", enil_identity_to_json(&id));
  cJSON_AddStringToObject(root, "accessToken", "synthetic-token");
  assert(enil_session_write(path, root));
  cJSON_Delete(root);
  fail_event = fail;
  client = enil_sse_create("synthetic-token", 0, path, (sqlite3 *)1, NULL);
  assert(client);
  assert(enil_sse_start(client, event_cb, NULL));
  for (i = 0; i < 100; i++) {
    usleep(10000);
    saved = enil_session_read(path);
    if (cJSON_HasObjectItem(saved, "nativeGlobalRevision") || health_failed) {
      cJSON_Delete(saved);
      break;
    }
    cJSON_Delete(saved);
  }
  enil_sse_free(client);
  assert(event_count == 1);
  saved = enil_session_read(path);
  if (fail) {
    assert(last_revision == 0);
    assert(!cJSON_HasObjectItem(saved, "nativeGlobalRevision"));
  } else {
    assert(last_revision == 9007199254740993LL);
    assert(!strcmp(cJSON_GetObjectItem(saved, "nativeGlobalRevision")->valuestring,
                   "9007199254740994"));
    assert(!strcmp(cJSON_GetObjectItem(saved, "nativeIndividualRevision")->valuestring,
                   "9007199254740995"));
  }
  cJSON_Delete(saved);
}
int main(int argc, char **argv) {
  enil_identity_t id;
  ENILLineResponse r;
  assert(argc >= 4);
  origin = argv[1];
  assert(enil_identity_default("desktopwin", &id));
  assert(enil_identity_bind(&id));
  if (!strcmp(argv[2], "poll-kick")) {
    ENILSSEClient *client;
    cJSON *root = cJSON_CreateObject();
    int i;
    cJSON_AddItemToObject(root, "clientIdentity", enil_identity_to_json(&id));
    cJSON_AddStringToObject(root, "accessToken", "synthetic-token");
    assert(enil_session_write(argv[3], root));
    cJSON_Delete(root);
    client = enil_sse_create("synthetic-token", 10, argv[3], (sqlite3 *)1, NULL);
    assert(client && enil_sse_start(client, event_cb, NULL));
    /* The Python server signals only after each request is blocked waiting
     * for a reply. More kicks than the retry budget must remain healthy. */
    for (i = 0; i < 6; i++) {
      assert(getchar() == 'k');
      enil_sse_kick(client);
    }
    assert(getchar() == 'q');
    enil_sse_free(client);
    assert(!health_failed && !event_count && !last_revision);
    assert(requests == 7);
    return 0;
  }
  if (!strcmp(argv[2], "remove-chat")) {
    assert(talk_send_chat_removed("synthetic-token", 37,
                                  "c00000000000000000000000000000000",
                                  "9007199254740993", 1800000000000LL) == 0);
    assert(requests == 1);
    return 0;
  }
  if (!strcmp(argv[2], "refresh-reauth")) {
    char account[1024], path[1100], stage[1024], journal[1150];
    cJSON *root, *saved;
    char *token;
    int code;
    assert(snprintf(account, sizeof(account), "%s/synthetic-mid", argv[3]) < (int)sizeof(account));
    assert(snprintf(stage, sizeof(stage), "%s/staged", argv[3]) < (int)sizeof(stage));
    assert(snprintf(path, sizeof(path), "%s/session.json", account) < (int)sizeof(path));
    assert(snprintf(journal, sizeof(journal), "%s.refresh-pending", path) < (int)sizeof(journal));
    /* The Python fixture creates these directories. */
    root = cJSON_Parse("{\"loginId\":\"login-A\",\"accessToken\":\"access-A\","
                       "\"refreshToken\":\"refresh-A\",\"mid\":\"synthetic-mid\"}");
    cJSON_AddItemToObject(root, "clientIdentity", enil_identity_to_json(&id));
    assert(enil_session_write(path, root));
    cJSON_ReplaceItemInObject(root, "loginId", cJSON_CreateString("login-B"));
    cJSON_ReplaceItemInObject(root, "accessToken", cJSON_CreateString("access-B"));
    cJSON_ReplaceItemInObject(root, "refreshToken", cJSON_CreateString("refresh-B"));
    {
      char staged_path[1100];
      assert(snprintf(staged_path, sizeof(staged_path), "%s/session.json", stage) <
             (int)sizeof(staged_path));
      assert(enil_session_write(staged_path, root));
    }
    activate_staging = stage;
    activate_account = account;
    token = enil_line_token_refresh(path, &code);
    assert(!token && code == 0 && requests == 1);
    saved = enil_session_read(path);
    assert(cJSON_Compare(root, saved, 1));
    cJSON_Delete(saved);
    saved = enil_session_read(journal);
    assert(!strcmp(cJSON_GetObjectItem(saved, "loginId")->valuestring, "login-A"));
    cJSON_Delete(saved);
    /* B's engine has independent health. Its refresh must retire A's late
     * journal, then refresh B rather than replaying A's credentials. */
    health_failed = 0;
    token = enil_line_token_refresh(path, &code);
    assert(token && !strcmp(token, "refreshed-B") && requests == 2);
    free(token);
    saved = enil_session_read(path);
    assert(!strcmp(cJSON_GetObjectItem(saved, "loginId")->valuestring, "login-B"));
    assert(!strcmp(cJSON_GetObjectItem(saved, "refreshToken")->valuestring, "refresh-B-next"));
    assert(access(journal, F_OK) != 0);
    cJSON_Delete(saved);
    cJSON_Delete(root);
    return 0;
  }
  if (!strcmp(argv[2], "poll") || !strcmp(argv[2], "poll-fail")) {
    polling(argv[3], !strcmp(argv[2], "poll-fail"));
    return 0;
  }
  if (!strncmp(argv[2], "journal-", 8)) {
    char pending[1024];
    cJSON *record, *patch;
    char *token, *id;
    int code, already_committed = strstr(argv[2], "committed") != NULL;
    persistence(argv[3]);
    id = enil_session_login_id(argv[3]);
    assert(id);
    snprintf(pending, sizeof(pending), "%s.refresh-pending", argv[3]);
    record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "previousRefreshToken", "earlier-refresh");
    cJSON_AddStringToObject(record, "responseBody",
                            "{\"message\":\"OK\",\"data\":{\"accessToken\":\"older-access\","
                            "\"refreshToken\":\"rotated-refresh\"}}");
    if (!strcmp(argv[2], "journal-stale")) {
      cJSON_ReplaceItemInObject(record, "responseBody",
                                cJSON_CreateString("{\"message\":\"OK\",\"data\":{\"accessToken\":"
                                                   "\"obsolete\",\"refreshToken\":\"obsolete\"}}"));
    } else if (!strcmp(argv[2], "journal-superseded")) {
      cJSON_AddStringToObject(record, "loginId", "previous-login");
      cJSON_ReplaceItemInObject(record, "previousRefreshToken",
                                cJSON_CreateString("rotated-refresh"));
    } else if (!strcmp(argv[2], "journal-committed")) {
      cJSON_AddStringToObject(record, "loginId", id);
      cJSON_AddStringToObject(record, "refreshId", "committed-refresh");
      /* Even an unchanged refresh token must not replay a committed reply. */
      cJSON_ReplaceItemInObject(record, "previousRefreshToken",
                                cJSON_CreateString("rotated-refresh"));
      patch = cJSON_Parse(
          "{\"refreshJournalId\":\"committed-refresh\",\"accessToken\":\"inline-next-access\"}");
      assert(enil_session_patch(argv[3], patch));
      cJSON_Delete(patch);
    }
    assert(enil_session_write(pending, record));
    cJSON_Delete(record);
    token = enil_line_token_refresh(argv[3], &code);
    assert(token && code == 0 && access(pending, F_OK) != 0);
    if (already_committed) {
      assert(requests == 0);
      assert(
          !strcmp(token, !strcmp(argv[2], "journal-committed") ? "inline-next-access" : "rotated"));
    } else {
      assert(requests == 1 && !strcmp(token, "fresh-from-fixture"));
    }
    free(token);
    free(id);
    return 0;
  }
  if (!strcmp(argv[2], "refresh-recovery")) {
    char pending[1024];
    cJSON *root, *record;
    char *token;
    int code;
    persistence(argv[3]);
    snprintf(pending, sizeof(pending), "%s.refresh-pending", argv[3]);
    record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "previousRefreshToken", "rotated-refresh");
    cJSON_AddStringToObject(record, "responseBody",
                            "{\"message\":\"OK\",\"data\":{\"accessToken\":\"recovered\","
                            "\"refreshToken\":\"recovered-refresh\",\"durationUntilRefreshInSec\":"
                            "\"3600\",\"tokenIssueTimeEpochSec\":\"1800000000\"}}");
    assert(enil_session_write(pending, record));
    cJSON_Delete(record);
    token = enil_line_token_refresh(argv[3], &code);
    assert(token && !strcmp(token, "recovered"));
    free(token);
    assert(access(pending, F_OK) != 0);
    root = enil_session_read(argv[3]);
    assert(!strcmp(cJSON_GetObjectItem(root, "refreshToken")->valuestring, "recovered-refresh"));
    assert(cJSON_HasObjectItem(root, "nativeLoginResult"));
    cJSON_Delete(root);
    return 0;
  }
  if (!strcmp(argv[2], "save")) {
    persistence(argv[3]);
    return 0;
  }
  if (!strcmp(argv[2], "encode")) {
    ENILBuf b = {NULL, 0};
    cJSON *v = cJSON_Parse(argv[4]);
    assert(enil_thrift_encode(argv[3], v, &b));
    fwrite(b.data, 1, b.size, stdout);
    enil_buf_free(&b);
    cJSON_Delete(v);
    return 0;
  }
  if (!strcmp(argv[2], "decode")) {
    unsigned char buf[32768];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    cJSON *v = enil_thrift_decode(argv[3], buf, n);
    char *s;
    if (!v)
      return 2;
    s = cJSON_PrintUnformatted(v);
    puts(s);
    free(s);
    cJSON_Delete(v);
    return 0;
  }
  r = enil_line_post(argv[2], argv[3], "synthetic-token");
  printf("%ld\n%s\n", r.status, r.body ? r.body : "");
  enil_line_response_free(&r);
  return 0;
}
