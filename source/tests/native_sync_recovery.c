/* Exercise the real account, sync, session, and SQLite layers with synthetic
 * RPC replies. Any accidental network request aborts the test. */
#include "enil_account.h"
#include "enil_db.h"
#include "enil_line.h"
#include "enil_session.h"
#include "enil_sse.h"
#include "enil_sync.h"
#include "enil_http.h"
#include "enil_cocoa_image.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sqlite3 *db;
static const char *session_path;
static enil_health_t *health;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t poll_ready = PTHREAD_COND_INITIALIZER;
static int resync_requested, stopped, poll_count, revision_calls;
static int fail_messages, failed, succeeded, fetching;
static long long baseline = 20;
static int event_test, allowed_polls = 1, stop_test;
static long long requested_revision;
static int box_mode, box_calls, history_calls, read_range_calls;
static int poll_failure_mode, fail_session_write, queue_during_sync;
enum {
  CHAT_EMPTY, CHAT_HTTP_ERROR, CHAT_TRANSPORT_ERROR, CHAT_MISSING_ARRAY,
  CHAT_BAD_ENTRY, CHAT_DB_ERROR, CHAT_BAD_ARRAY, CHAT_BAD_BODY
};
static int chat_update_mode, chat_calls;

int __real_fsync(int fd);
int __wrap_fsync(int fd) {
  if (fail_session_write) { errno = EIO; return -1; }
  return __real_fsync(fd);
}
unsigned int __real_sleep(unsigned int seconds);
unsigned int __wrap_sleep(unsigned int seconds) {
  return poll_failure_mode ? 0 : __real_sleep(seconds);
}

void enil_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
void enil_status_post(const char *key, const char *message, int indeterminate) {
  (void)key; (void)message; (void)indeterminate;
}
void enil_status_post_error(const char *key, const char *message) { (void)key; (void)message; }
void enil_progress_post(int step, int total, int n, int count, const char *message,
                        int error, const char *key, int indeterminate) {
  (void)n; (void)count; (void)message; (void)key; (void)indeterminate;
  if (error) failed++;
  else if (step == total) succeeded++;
}
int enil_localized_collate(void *ctx, int an, const void *a, int bn, const void *b) {
  int cmp;
  (void)ctx;
  cmp = memcmp(a, b, an < bn ? (size_t)an : (size_t)bn);
  return cmp ? cmp : an - bn;
}
int enil_format_date_string(long long ms, char *out, size_t size) {
  (void)ms; if (size) out[0] = 0; return 0;
}
int enil_thumb_generate(const char *src, const char *dst, ENILThumbInfo *info) {
  (void)src; (void)dst; (void)info; assert(0); return -1;
}
CURLcode __wrap_curl_easy_perform(CURL *curl) { (void)curl; assert(0); return CURLE_FAILED_INIT; }
char *__wrap_enil_line_acquire_obs_token(const char *path, const char *token) {
  (void)path; (void)token; return strdup("synthetic-obs");
}
static ENILLineResponse response(const char *data) {
  ENILLineResponse r = {200, NULL};
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "message", "OK");
  cJSON_AddItemToObject(root, "data", cJSON_Parse(data));
  r.body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return r;
}
ENILLineResponse __wrap_enil_line_post(const char *path, const char *body, const char *token) {
  const char *method = strrchr(path, '/') + 1;
  (void)body; (void)token;
  if (!strcmp(method, "getChats")) {
    chat_calls++;
    if (chat_update_mode == CHAT_HTTP_ERROR || chat_update_mode == CHAT_TRANSPORT_ERROR) {
      ENILLineResponse r = {chat_update_mode == CHAT_HTTP_ERROR ? 503 : 0, NULL};
      return r;
    }
    if (chat_update_mode == CHAT_MISSING_ARRAY) return response("{}");
    if (chat_update_mode == CHAT_BAD_ARRAY) return response("{\"chats\":{}}");
    if (chat_update_mode == CHAT_BAD_BODY) return response("[]");
    /* A valid entry before a malformed one must not become partial success. */
    if (chat_update_mode == CHAT_BAD_ENTRY)
      return response("{\"chats\":[{\"chatMid\":\"cformer\"},{}]}");
    if (chat_update_mode == CHAT_DB_ERROR)
      return response("{\"chats\":[{\"chatMid\":\"cformer\",\"chatName\":\"Updated\"}]}");
    return response("{\"chats\":[]}");
  }
  if (!strcmp(method, "getLastOpRevision")) {
    char value[40];
    revision_calls++;
    /* A revision queried AFTER the data fetch would skip newer events. */
    snprintf(value, sizeof(value), "\"%lld\"", fetching ? baseline + 10 : baseline);
    return response(value);
  }
  fetching = 1;
  /* The saved cursor must stay unchanged throughout the fetch. */
  assert(enil_db_get_local_rev(db) == 10);
  if (!strcmp(method, "getProfile")) return response("{\"mid\":\"self\",\"displayName\":\"Self\"}");
  if (!strcmp(method, "getAllContactIds")) return response("[]");
  if (!strcmp(method, "getAllChatMids"))
    return response("{\"memberChatMids\":[],\"invitedChatMids\":[]}");
  if (!strcmp(method, "getMessageBoxes")) {
    box_calls++;
    if (box_mode == 1) return response("{\"messageBoxes\":[],\"hasNext\":false}");
    if (box_mode == 2 || box_mode == 3) {
      if (box_calls == 1)
        return response("{\"messageBoxes\":[{\"id\":\"peer\",\"midType\":0}],\"hasNext\":true}");
      if (box_mode == 2) { ENILLineResponse r = {503, NULL}; return r; }
      return response("{\"messageBoxes\":[],\"hasNext\":true}");
    }
    return response("{\"messageBoxes\":[{\"id\":\"peer\",\"midType\":0}],\"hasNext\":false}");
  }
  if (!strcmp(method, "getRecentMessagesV2")) {
    /* This chat is retained locally, but is no longer accessible remotely. */
    assert(!strstr(body, "obsolete-chat"));
    history_calls++;
    if (queue_during_sync) {
      cJSON *request = cJSON_Parse("{\"1\":\"200\",\"2\":\"300\"}");
      assert(enil_session_update_partial_full_syncs(session_path, request) == 1);
      cJSON_Delete(request);
      queue_during_sync = 0;
    }
    if (fail_messages) { ENILLineResponse r = {503, NULL}; return r; }
    return response("[{\"id\":\"saved-message\",\"from\":\"peer\",\"to\":\"self\","
                    "\"toType\":0,\"createdTime\":\"1\",\"contentType\":0,\"text\":\"recovered\"}]");
  }
  if (!strcmp(method, "getMessageReadRange")) {
    assert(!strstr(body, "obsolete-chat"));
    read_range_calls++;
    return response("[]");
  }
  if (!strcmp(method, "getOwnedProductSummaries")) return response("{\"productList\":[]}");
  fprintf(stderr, "Unexpected RPC: %s\n", method);
  assert(0);
  return response("{}");
}
ENILLineResponse __wrap_enil_line_post_ex(const char *path, const char *body, const char *token,
                                         const char *const *headers, int count, long timeout,
                                         const volatile int *cancel) {
  (void)body; (void)token; (void)headers; (void)count; (void)timeout; (void)cancel;
  assert(!strcmp(path, "/native/sync"));
  if (poll_failure_mode) {
    ENILLineResponse r = {503, NULL};
    int n;
    pthread_mutex_lock(&mutex);
    n = ++poll_count;
    pthread_mutex_unlock(&mutex);
    if (poll_failure_mode == 1 && n == 13) {
      r.status = 400;
      r.body = strdup("{\"code\":8}"); /* end the test through the normal callback */
    } else if (poll_failure_mode == 1 && n % 2 == 0) r.status = 204;
    return r;
  }
  if (event_test) {
    ENILLineResponse r;
    cJSON *args = cJSON_Parse(body);
    cJSON *revision = cJSON_GetObjectItem(cJSON_GetArrayItem(args, 0), "lastRevision");
    assert(cJSON_IsString(revision));
    pthread_mutex_lock(&mutex);
    requested_revision = strtoll(revision->valuestring, NULL, 10);
    cJSON_Delete(args);
    poll_count++;
    pthread_cond_broadcast(&poll_ready);
    while (poll_count > allowed_polls && !stop_test)
      pthread_cond_wait(&poll_ready, &mutex);
    pthread_mutex_unlock(&mutex);
    if (stop_test) { ENILLineResponse idle = {204, NULL}; return idle; }
    r = response("{\"operationResponse\":{\"operations\":["
                    "{\"revision\":\"20\",\"type\":26,\"message\":{\"id\":\"first\","
                    "\"from\":\"upeer\",\"to\":\"self\",\"toType\":0,\"createdTime\":\"1\","
                    "\"contentType\":0,\"text\":\"first message\"}},"
                    "{\"revision\":\"21\",\"type\":26,\"message\":{\"id\":\"second\","
                    "\"from\":\"upeer\",\"to\":\"self\",\"toType\":0,\"createdTime\":\"2\","
                    "\"contentType\":0,\"text\":\"second message\"}}],"
                    "\"globalEvents\":{\"lastRevision\":\"99\"},"
                    "\"individualEvents\":{\"lastRevision\":\"100\"}}}");
    if (event_test == 2) {
      cJSON *root = cJSON_Parse(r.body);
      cJSON *operations = cJSON_GetObjectItem(cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "data"), "operationResponse"), "operations");
      /* Neither an added nor a removed reaction to an older, uncached
       * message may block the two new messages later in this response. */
      cJSON_InsertItemInArray(operations, 0, cJSON_Parse(
        "{\"revision\":\"19\",\"type\":139,\"param1\":\"uncached\","
        "\"param2\":\"{\\\"chatMid\\\":\\\"upeer\\\"}\"}"));
      cJSON_InsertItemInArray(operations, 0, cJSON_Parse(
        "{\"revision\":\"18\",\"type\":140,\"param1\":\"uncached\","
        "\"param2\":\"{\\\"chatMid\\\":\\\"upeer\\\","
        "\\\"curr\\\":{\\\"predefinedReactionType\\\":2}}\",\"param3\":\"upeer\"}"));
      free(r.body);
      r.body = cJSON_PrintUnformatted(root);
      cJSON_Delete(root);
    }
    if (event_test == 3) {
      cJSON *root = cJSON_Parse(r.body);
      cJSON *operations = cJSON_GetObjectItem(cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "data"), "operationResponse"), "operations");
      /* Both chat-update op types precede messages from an unrelated peer. */
      cJSON_InsertItemInArray(operations, 0, cJSON_Parse(
        "{\"revision\":\"19\",\"type\":122,\"param1\":\"cformer\"}"));
      cJSON_InsertItemInArray(operations, 0, cJSON_Parse(
        "{\"revision\":\"18\",\"type\":121,\"param1\":\"cformer\"}"));
      free(r.body);
      r.body = cJSON_PrintUnformatted(root);
      cJSON_Delete(root);
    }
    return r;
  }
  poll_count++;
  return response("{\"fullSyncResponse\":{\"reasons\":[1],\"nextRevision\":\"20\"},"
                  "\"operationResponse\":{\"globalEvents\":{\"lastRevision\":\"99\"}}}");
}
static int event(const ENILSSEEvent *ev, void *ctx) {
  enil_account_sse_result_t result;
  int ok;
  (void)ctx;
  ok = enil_account_process_sse_event(health, db, "synthetic", "self", session_path,
                                     ev->type, ev->data, NULL, &result);
  if (!event_test) assert(ok);
  pthread_mutex_lock(&mutex);
  resync_requested = result.should_start_sync;
  stopped = result.should_stop_sse;
  pthread_mutex_unlock(&mutex);
  enil_account_sse_result_free(&result);
  return ok;
}

static void check_messages(int count) {
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM messages_v2", -1, &stmt, NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == count);
  sqlite3_finalize(stmt);
}

static void uncached_reactions(void) {
  ENILSSEClient *client;
  cJSON *saved;
  event_test = 2;
  assert(sqlite3_exec(db, "INSERT INTO contacts_v2(mid,displayName) VALUES('upeer','Peer')",
                      NULL, NULL, NULL) == SQLITE_OK);
  client = enil_sse_create("synthetic", 10, session_path, db, health);
  assert(client && enil_sse_start(client, event, NULL));
  pthread_mutex_lock(&mutex);
  while (poll_count < 2) pthread_cond_wait(&poll_ready, &mutex);
  assert(requested_revision == 21 && enil_db_get_local_rev(db) == 21);
  check_messages(2);
  assert(enil_db_message_box_unread_count(db, "upeer") == 2);
  assert(!enil_health_any_failed());
  saved = enil_session_read(session_path);
  assert(!strcmp(cJSON_GetObjectItem(saved, "nativeGlobalRevision")->valuestring, "99"));
  assert(!strcmp(cJSON_GetObjectItem(saved, "nativeIndividualRevision")->valuestring, "100"));
  cJSON_Delete(saved);
  stop_test = 1;
  pthread_cond_broadcast(&poll_ready);
  pthread_mutex_unlock(&mutex);
  enil_sse_free(client);

  /* Actual database failures must still reach the event retry path. */
  assert(sqlite3_exec(db, "CREATE TRIGGER reject_reaction BEFORE UPDATE ON messages_v2 "
                         "BEGIN SELECT RAISE(FAIL,'reaction write failure'); END",
                      NULL, NULL, NULL) == SQLITE_OK);
  assert(enil_db_message_reactions_update(db, "first", "upeer", 0, 2, NULL, NULL, 1)
         != SQLITE_OK);
  assert(sqlite3_exec(db, "DROP TABLE messages_v2", NULL, NULL, NULL) == SQLITE_OK);
  assert(enil_db_message_reactions_update(db, "uncached", "upeer", 0, 2, NULL, NULL, 1)
         != SQLITE_OK);
}

static void chat_update_recovery(int mode) {
  ENILSSEClient *client;
  cJSON *saved;
  sqlite3_stmt *statement;
  event_test = 3;
  chat_update_mode = mode;
  enil_health_bind(health);
  assert(sqlite3_exec(db,
    "INSERT INTO contacts_v2(mid,displayName) VALUES('upeer','Peer');"
    "INSERT INTO chats_v2(chatMid,chatName) VALUES('cformer','Cached name')",
    NULL, NULL, NULL) == SQLITE_OK);
  if (mode == CHAT_DB_ERROR)
    assert(sqlite3_exec(db,
      "CREATE TRIGGER reject_chat BEFORE INSERT ON chats_v2 "
      "BEGIN SELECT RAISE(FAIL,'chat write failure'); END",
      NULL, NULL, NULL) == SQLITE_OK);
  client = enil_sse_create("synthetic", 10, session_path, db, health);
  assert(client && enil_sse_start(client, event, NULL));
  pthread_mutex_lock(&mutex);
  while (poll_count < 2) pthread_cond_wait(&poll_ready, &mutex);
  if (mode != CHAT_EMPTY) {
    /* A failed lookup/write must retain every cursor and block later events
     * until retry succeeds, rather than treating the chat as inaccessible. */
    assert(chat_calls == 1);
    assert(requested_revision == 10 && enil_db_get_local_rev(db) == 10);
    check_messages(0);
    saved = enil_session_read(session_path);
    assert(!cJSON_HasObjectItem(saved, "nativeGlobalRevision"));
    assert(!cJSON_HasObjectItem(saved, "nativeIndividualRevision"));
    cJSON_Delete(saved);
    if (mode == CHAT_DB_ERROR)
      assert(sqlite3_exec(db, "DROP TRIGGER reject_chat", NULL, NULL, NULL) == SQLITE_OK);
    else
      chat_update_mode = CHAT_EMPTY;
    allowed_polls = 2;
    pthread_cond_broadcast(&poll_ready);
    while (poll_count < 3) pthread_cond_wait(&poll_ready, &mutex);
  }
  assert(chat_calls == (mode == CHAT_EMPTY ? 2 : 3));
  assert(requested_revision == 21 && enil_db_get_local_rev(db) == 21);
  check_messages(2);
  assert(enil_db_message_box_unread_count(db, "upeer") == 2);
  assert(!enil_health_any_failed());
  saved = enil_session_read(session_path);
  assert(!strcmp(cJSON_GetObjectItem(saved, "nativeGlobalRevision")->valuestring, "99"));
  assert(!strcmp(cJSON_GetObjectItem(saved, "nativeIndividualRevision")->valuestring, "100"));
  cJSON_Delete(saved);
  /* Skipping the obsolete update preserves locally cached metadata. */
  assert(sqlite3_prepare_v2(db, "SELECT chatName FROM chats_v2 WHERE chatMid='cformer'",
                            -1, &statement, NULL) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(statement, 0),
                  mode == CHAT_DB_ERROR ? "Updated" : "Cached name"));
  sqlite3_finalize(statement);
  stop_test = 1;
  pthread_cond_broadcast(&poll_ready);
  pthread_mutex_unlock(&mutex);
  enil_sse_free(client);
}

static void legacy_account_start(void) {
  char *directory = strdup(session_path), *id;
  session_t sending, refreshing;
  cJSON *saved, *replacement;
  assert(directory && strrchr(directory, '/'));
  *strrchr(directory, '/') = 0;
  /* Exercise startup of an existing Chrome account without either of the
   * new identity/generation fields, before send and refresh workers start. */
  saved = cJSON_Parse("{\"accessToken\":\"old-access\",\"refreshToken\":\"old-refresh\","
                      "\"mid\":\"self\",\"reqSeq\":1,\"e2eeKeys\":[{\"keyId\":1}]}");
  assert(enil_session_write(session_path, saved));
  cJSON_Delete(saved);
  assert(enil_account_start_core(directory, &db, NULL, NULL, NULL, NULL)
         == ENIL_ACCOUNT_START_OK);
  assert(enil_session_load(session_path, &sending));
  /* Refresh must not migrate the generation underneath an in-flight send. */
  id = enil_session_login_id(session_path);
  assert(id && enil_session_load(session_path, &refreshing));
  free(refreshing.accessToken);
  refreshing.accessToken = strdup("new-access");
  assert(enil_session_save(session_path, &refreshing));
  sending.reqSeq = 2;
  sending.e2eeSequenceNumber = 3;
  sending.workerRestoreState = cJSON_Parse("{\"synthetic\":\"send-state\"}");
  assert(enil_session_save(session_path, &sending));
  saved = enil_session_read(session_path);
  assert(!strcmp(cJSON_GetObjectItem(saved, "loginId")->valuestring, id));
  assert(!strcmp(cJSON_GetObjectItem(saved, "accessToken")->valuestring, "new-access"));
  assert(cJSON_GetObjectItem(saved, "reqSeq")->valueint == 2);
  assert(cJSON_GetObjectItem(saved, "e2eeSequenceNumber")->valueint == 3);
  assert(cJSON_Compare(cJSON_GetObjectItem(saved, "workerRestoreState"), sending.workerRestoreState, 1));
  /* Retain the protection against an actual replacement login. */
  replacement = cJSON_Duplicate(saved, 1);
  cJSON_ReplaceItemInObject(replacement, "loginId", cJSON_CreateString("another-login"));
  assert(enil_session_activate(session_path, replacement));
  sending.reqSeq = 4;
  assert(!enil_session_save(session_path, &sending));
  cJSON_Delete(saved);
  saved = enil_session_read(session_path);
  assert(cJSON_Compare(saved, replacement, 1));
  cJSON_Delete(saved);
  cJSON_Delete(replacement);
  enil_session_free(&sending);
  enil_session_free(&refreshing);
  free(id);
  free(directory);
}

static void event_write_recovery(int cursor_failure) {
  ENILSSEClient *client;
  cJSON *saved;
  event_test = 1;
  assert(sqlite3_exec(db, "INSERT INTO contacts_v2(mid,displayName) VALUES('upeer','Peer')",
                      NULL, NULL, NULL) == SQLITE_OK);
  assert(sqlite3_exec(db, cursor_failure
    ? "CREATE TRIGGER reject_write BEFORE INSERT ON db_meta WHEN NEW.key='localRev' BEGIN SELECT RAISE(FAIL,'cursor write failure'); END"
    : "CREATE TRIGGER reject_write BEFORE INSERT ON messages_v2 BEGIN SELECT RAISE(FAIL,'message write failure'); END",
    NULL, NULL, NULL) == SQLITE_OK);
  client = enil_sse_create("synthetic", 10, session_path, db, health);
  assert(client && enil_sse_start(client, event, NULL));
  pthread_mutex_lock(&mutex);
  while (poll_count < 2) pthread_cond_wait(&poll_ready, &mutex);
  /* The retry must use the old cursor even if only its persistence failed.
   * Neither failure may dispatch the second message or acknowledge other cursors. */
  assert(requested_revision == 10 && enil_db_get_local_rev(db) == 10);
  check_messages(cursor_failure ? 1 : 0);
  assert(enil_db_message_box_unread_count(db, "upeer") == (cursor_failure ? 1 : 0));
  saved = enil_session_read(session_path);
  assert(!cJSON_HasObjectItem(saved, "nativeGlobalRevision"));
  assert(!cJSON_HasObjectItem(saved, "nativeIndividualRevision"));
  cJSON_Delete(saved);
  assert(sqlite3_exec(db, "DROP TRIGGER reject_write", NULL, NULL, NULL) == SQLITE_OK);
  allowed_polls = 2;
  pthread_cond_broadcast(&poll_ready);
  while (poll_count < 3) pthread_cond_wait(&poll_ready, &mutex);
  assert(requested_revision == 21 && enil_db_get_local_rev(db) == 21);
  check_messages(2);
  assert(enil_db_message_box_unread_count(db, "upeer") == 2);
  saved = enil_session_read(session_path);
  assert(!strcmp(cJSON_GetObjectItem(saved, "nativeGlobalRevision")->valuestring, "99"));
  assert(!strcmp(cJSON_GetObjectItem(saved, "nativeIndividualRevision")->valuestring, "100"));
  cJSON_Delete(saved);
  stop_test = 1;
  pthread_cond_broadcast(&poll_ready);
  pthread_mutex_unlock(&mutex);
  enil_sse_free(client);
}
static void current_chats(int mode) {
  sqlite3_stmt *statement;
  int ok;
  box_mode = mode;
  assert(sqlite3_exec(db, "INSERT INTO message_boxes_v2(id) VALUES('obsolete-chat');"
                         "INSERT INTO messages_v2(id,chat_id,text) VALUES('old','obsolete-chat','cached history')",
                      NULL, NULL, NULL) == SQLITE_OK);
  ok = enil_account_sync_all(health, db, "synthetic", "self", session_path);
  assert(ok == (mode < 2));
  assert(enil_db_get_local_rev(db) == (mode < 2 ? baseline : 10));
  assert(history_calls == (mode == 0 ? 1 : 0));
  assert(read_range_calls == (mode == 0 ? 1 : 0));
  assert(box_calls == (mode >= 2 ? 2 : 1));
  /* Keeping local history must not require successfully fetching it again. */
  assert(sqlite3_prepare_v2(db, "SELECT text FROM messages_v2 WHERE id='old'", -1,
                            &statement, NULL) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(statement, 0), "cached history"));
  sqlite3_finalize(statement);
}

static void polling_failure_streak(int consecutive) {
  ENILSSEClient *client;
  int i, count = 0;
  poll_failure_mode = consecutive ? 2 : 1;
  enil_health_bind(health);
  client = enil_sse_create("synthetic", 10, session_path, db, health);
  assert(client && enil_sse_start(client, event, NULL));
  for (i = 0; i < 200; i++) {
    pthread_mutex_lock(&mutex);
    count = poll_count;
    pthread_mutex_unlock(&mutex);
    if (count >= 13 || enil_health_any_failed()) break;
    usleep(10000);
  }
  enil_sse_free(client);
  assert(count == (consecutive ? 5 : 13));
  assert(enil_health_any_failed() == consecutive);
  assert(enil_db_get_local_rev(db) == 10);
}

static void check_partial_state(const char *completed, const char *pending) {
  cJSON *root = enil_session_read(session_path);
  cJSON *expected = cJSON_Parse(completed);
  cJSON *queued = cJSON_GetObjectItem(root, "pendingPartialFullSyncs");
  char *wire = enil_session_get_partial_full_syncs_json(session_path);
  cJSON *advertised = cJSON_Parse(wire);
  assert(cJSON_Compare(cJSON_GetObjectItem(root, "lastPartialFullSyncs"), expected, 1));
  assert(cJSON_Compare(advertised, expected, 1)); /* pending work never goes to LINE */
  cJSON_Delete(expected);
  expected = pending ? cJSON_Parse(pending) : NULL;
  assert(pending ? cJSON_Compare(queued, expected, 1) : !queued);
  cJSON_Delete(expected);
  cJSON_Delete(advertised);
  free(wire);
  cJSON_Delete(root);
}

static int partial_event(void) {
  enil_account_sse_result_t result;
  int scheduled;
  assert(enil_account_process_sse_event(health, db, "synthetic", "self", session_path,
    "partialFullSync", "{\"targetCategories\":{\"1\":\"100\"}}", NULL, &result));
  scheduled = result.should_start_sync;
  enil_account_sse_result_free(&result);
  return scheduled;
}

static void partial_sync_recovery(int newer) {
  enil_account_sse_result_t result;
  cJSON *snapshot, *replacement;
  char *login_id = enil_session_login_id(session_path);
  assert(login_id);
  free(login_id);
  /* A failed queue write must fail delivery, rather than silently losing work. */
  fail_session_write = 1;
  assert(!enil_account_process_sse_event(health, db, "synthetic", "self", session_path,
    "partialFullSync", "{\"targetCategories\":{\"1\":\"100\"}}", NULL, &result));
  enil_account_sse_result_free(&result);
  fail_session_write = 0;
  check_partial_state("{\"1\":\"9\"}", NULL);
  assert(partial_event());
  check_partial_state("{\"1\":\"9\"}", "{\"1\":\"100\"}");
  fail_messages = 1;
  assert(!enil_account_sync_all(health, db, "synthetic", "self", session_path));
  check_partial_state("{\"1\":\"9\"}", "{\"1\":\"100\"}");
  /* Reopening the saved session/redelivering the event still schedules work. */
  assert(enil_session_bind_identity(session_path));
  assert(partial_event());
  fail_messages = 0; fetching = 0;
  assert(sqlite3_exec(db, "CREATE TRIGGER reject_cursor BEFORE INSERT ON db_meta "
    "WHEN NEW.key='localRev' BEGIN SELECT RAISE(FAIL,'cursor write failure'); END",
    NULL, NULL, NULL) == SQLITE_OK);
  assert(!enil_account_sync_all(health, db, "synthetic", "self", session_path));
  check_partial_state("{\"1\":\"9\"}", "{\"1\":\"100\"}");
  assert(sqlite3_exec(db, "DROP TRIGGER reject_cursor", NULL, NULL, NULL) == SQLITE_OK);
  /* Completion is also atomic: a failed session write leaves work pending. */
  snapshot = enil_session_read(session_path);
  fail_session_write = 1;
  assert(!enil_session_complete_partial_full_syncs(session_path, snapshot));
  fail_session_write = 0;
  cJSON_Delete(snapshot);
  check_partial_state("{\"1\":\"9\"}", "{\"1\":\"100\"}");
  fetching = 0;
  queue_during_sync = newer;
  snapshot = enil_session_read(session_path);
  assert(enil_account_sync_all(health, db, "synthetic", "self", session_path));
  check_partial_state("{\"1\":\"100\"}", newer ? "{\"1\":\"200\",\"2\":\"300\"}" : NULL);
  assert(!partial_event()); /* the completed request is now a no-op */
  if (newer) {
    cJSON *request = cJSON_Parse("{\"1\":\"200\",\"2\":\"300\"}");
    assert(enil_session_update_partial_full_syncs(session_path, request) == 1);
    cJSON_Delete(request);
  }
  /* A sync finishing after reauthentication cannot acknowledge the new login. */
  replacement = enil_session_read(session_path);
  cJSON_ReplaceItemInObject(replacement, "loginId", cJSON_CreateString("replacement"));
  assert(enil_session_activate(session_path, replacement));
  assert(!enil_session_complete_partial_full_syncs(session_path, snapshot));
  cJSON_Delete(snapshot);
  snapshot = enil_session_read(session_path);
  assert(cJSON_Compare(snapshot, replacement, 1));
  cJSON_Delete(snapshot);
  cJSON_Delete(replacement);
}

static void replay_synced_messages(void) {
  const char *ids[] = {"sent", "received", "new-reply", "new-send"};
  const int outgoing[] = {1, 0, 0, 1};
  const int unread[] = {1, 1, 2, 0};
  int i, handled;
  char event_data[512];
  /* Full sync already fetched our send and the unread reply. */
  assert(sqlite3_exec(db,
    "INSERT INTO contacts_v2(mid,displayName) VALUES('upeer','Peer');"
    "INSERT INTO message_boxes_v2(id,unreadCount) VALUES('upeer',1);"
    "INSERT INTO messages_v2(id,chat_id,text,contentType) VALUES"
    "('sent','upeer','hello',0),('received','upeer','reply',0)",
    NULL, NULL, NULL) == SQLITE_OK);
  assert(enil_session_bind_identity(session_path));
  for (i = 0; i < 4; i++) {
    snprintf(event_data, sizeof(event_data),
      "{\"revision\":\"%d\",\"type\":%d,\"message\":{\"id\":\"%s\","
      "\"from\":\"%s\",\"to\":\"%s\",\"toType\":0,\"createdTime\":\"%d\","
      "\"contentType\":0,\"text\":\"hello\"}}",
      21 + i, outgoing[i] ? 25 : 26, ids[i],
      outgoing[i] ? "self" : "upeer", outgoing[i] ? "upeer" : "self", 100 + i);
    assert(enil_sync_process_sse_event(db, "synthetic", "self", session_path,
      "message", event_data, &handled) == SQLITE_OK && handled);
    /* Replayed messages preserve sync's count; new arrivals/sends still change it. */
    assert(enil_db_message_box_unread_count(db, "upeer") == unread[i]);
  }
}

static void chat_page_and_status_queries(void) {
  message_row_t *rows = NULL;
  sqlite3_stmt *stmt = NULL;
  int count = 0, more = 0, i;
  const char *sql =
    "INSERT INTO messages_v2 (id, chat_id, createdTime, text) VALUES"
    " ('a10','chat-a',10,'a10'),('a20','chat-a',20,'a20'),"
    " ('a30','chat-a',30,'a30'),('a40','chat-a',40,'a40'),"
    " ('b50','chat-b',50,'b50')";

  /* A populated existing database gets the new index on startup. */
  assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
  assert(sqlite3_exec(db, "DROP INDEX idx_messages_chat_time", NULL, NULL, NULL) == SQLITE_OK);
  assert(enil_db_create_tables(db) == SQLITE_OK);
  assert(sqlite3_prepare_v2(db,
    "SELECT count(*) FROM sqlite_master WHERE type='index'"
    " AND name='idx_messages_chat_time'", -1, &stmt, NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
  sqlite3_finalize(stmt);

  assert(enil_db_message_rows_get_page(db, "chat-a", "self", 0, 2,
                                       &rows, &count, &more) == SQLITE_OK);
  assert(count == 2 && more == 1);
  assert(!strcmp(rows[0].message_id, "a30") && !strcmp(rows[1].message_id, "a40"));
  for (i = 0; i < count; i++) message_row_free(&rows[i]);
  free(rows);

  rows = NULL;
  assert(enil_db_message_rows_get_page(db, "chat-a", "self", 30, 2,
                                       &rows, &count, &more) == SQLITE_OK);
  assert(count == 2 && more == 0);
  assert(!strcmp(rows[0].message_id, "a10") && !strcmp(rows[1].message_id, "a20"));
  for (i = 0; i < count; i++) message_row_free(&rows[i]);
  free(rows);

  assert(sqlite3_exec(db,
    "INSERT INTO message_boxes_v2 (id, unreadCount, lastDeliveredTime)"
    " VALUES ('chat-a',3,40),('chat-b',NULL,NULL)",
    NULL, NULL, NULL) == SQLITE_OK);
  assert(enil_db_message_box_unread_count(db, "chat-a") == 3);
  assert(enil_db_message_box_last_delivered_time(db, "chat-a") == 40);
  assert(enil_db_message_box_unread_count(db, "chat-b") == 0);
  assert(enil_db_message_box_last_delivered_time(db, "chat-b") == 0);
  assert(enil_db_message_box_last_delivered_time(db, "missing") == 0);
}

int main(int argc, char **argv) {
  enil_identity_t identity;
  cJSON *root;
  ENILSSEClient *client;
  sqlite3_stmt *statement;
  int i, seen = 0;
  assert(argc == 3 || argc == 4);
  session_path = argv[1];
  db = enil_db_open(argv[2]);
  assert(db && enil_db_create_tables(db) == SQLITE_OK);
  assert(enil_db_set_local_rev(db, 10) == SQLITE_OK);
  health = enil_health_create("synthetic");
  assert(enil_identity_default(argc == 4 && !strcmp(argv[3], "partial-chrome")
    ? "chrome" : "desktopwin", &identity));
  root = cJSON_Parse("{\"accessToken\":\"synthetic\",\"mid\":\"self\",\"lastPartialFullSyncs\":{\"1\":\"9\"}}");
  cJSON_AddItemToObject(root, "clientIdentity", enil_identity_to_json(&identity));
  assert(enil_session_write(session_path, root));
  cJSON_Delete(root);
  if (argc == 4) {
    if (!strcmp(argv[3], "chat-page")) chat_page_and_status_queries();
    else if (!strcmp(argv[3], "replay-synced")) replay_synced_messages();
    else if (!strcmp(argv[3], "idle-reset")) polling_failure_streak(0);
    else if (!strncmp(argv[3], "chat-update-", 12)) chat_update_recovery(atoi(argv[3] + 12));
    else if (!strcmp(argv[3], "consecutive-failures")) polling_failure_streak(1);
    else if (!strncmp(argv[3], "partial-", 8) && strcmp(argv[3], "partial-boxes"))
      partial_sync_recovery(!strcmp(argv[3], "partial-newer"));
    else if (!strcmp(argv[3], "reactions")) uncached_reactions();
    else if (!strcmp(argv[3], "legacy")) legacy_account_start();
    else if (!strcmp(argv[3], "obsolete")) current_chats(0);
    else if (!strcmp(argv[3], "empty")) current_chats(1);
    else if (!strcmp(argv[3], "partial-boxes")) current_chats(2);
    else if (!strcmp(argv[3], "invalid-boxes")) current_chats(3);
    else event_write_recovery(!strcmp(argv[3], "cursor"));
    enil_health_destroy(health);
    enil_db_close(db);
    return 0;
  }
  client = enil_sse_create("synthetic", 10, session_path, db, health);
  assert(client && enil_sse_start(client, event, NULL));
  for (i = 0; i < 100; i++) {
    pthread_mutex_lock(&mutex);
    seen = resync_requested && stopped;
    pthread_mutex_unlock(&mutex);
    if (seen) break;
    usleep(10000);
  }
  assert(seen);
  usleep(200000); /* Let dispatch finish before testing its cursor writes. */
  assert(enil_db_get_local_rev(db) == 10);
  enil_sse_free(client);
  assert(poll_count == 1);
  root = enil_session_read(session_path);
  assert(!cJSON_HasObjectItem(root, "nativeGlobalRevision"));
  cJSON_Delete(root);

  fail_messages = 1;
  assert(!enil_account_sync_all(health, db, "synthetic", "self", session_path));
  assert(failed == 1 && succeeded == 0 && revision_calls == 1);
  assert(enil_db_get_local_rev(db) == 10);
  /* A failed SQLite write must also keep the old revision and roll back. */
  fail_messages = 0; fetching = 0;
  assert(sqlite3_exec(db, "CREATE TRIGGER reject_message BEFORE INSERT ON messages_v2 "
                         "BEGIN SELECT RAISE(FAIL, 'synthetic write failure'); END", NULL, NULL, NULL) == SQLITE_OK);
  assert(!enil_account_sync_all(health, db, "synthetic", "self", session_path));
  assert(failed == 2 && succeeded == 0 && revision_calls == 2);
  assert(enil_db_get_local_rev(db) == 10);
  assert(sqlite3_exec(db, "DROP TRIGGER reject_message", NULL, NULL, NULL) == SQLITE_OK);
  fetching = 0;
  assert(enil_account_sync_all(health, db, "synthetic", "self", session_path));
  assert(failed == 2 && succeeded == 1 && revision_calls == 3);
  assert(enil_db_get_local_rev(db) == baseline);
  assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM messages_v2 WHERE id='saved-message'", -1,
                            &statement, NULL) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1);
  sqlite3_finalize(statement);
  enil_health_destroy(health);
  enil_db_close(db);
  return 0;
}
