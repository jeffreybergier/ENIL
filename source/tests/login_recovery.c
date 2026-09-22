/* Local filesystem recovery, using the production lifecycle and synthetic data. */
#include "enil_login_store.h"
#include "enil_session.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_syncs, directory_syncs, fail_directory_sync;
int __real_fsync(int fd);
int __wrap_fsync(int fd) {
  struct stat info;
  assert(fstat(fd, &info) == 0);
  if (S_ISDIR(info.st_mode)) {
    directory_syncs++;
    if (fail_directory_sync) {
      errno = EIO;
      return -1;
    }
  } else {
    file_syncs++;
  }
  return __real_fsync(fd);
}

void enil_log(const char *tag, const char *format, ...) {
  (void)tag;
  (void)format;
}

static void path(char *out, size_t size, const char *dir, const char *name) {
  assert(snprintf(out, size, "%s/%s", dir, name) < (int)size);
}
static cJSON *read_saved(const char *dir) {
  char file[4096];
  path(file, sizeof(file), dir, "session.json");
  return enil_session_read(file);
}
static void patch(const char *dir, const char *json) {
  char file[4096];
  cJSON *value = cJSON_Parse(json);
  path(file, sizeof(file), dir, "session.json");
  assert(value && enil_session_patch(file, value));
  cJSON_Delete(value);
}
static void staging(char *dir, size_t size, const char *root, int n, const char *mid) {
  char name[64];
  snprintf(name, sizeof(name), ".staging-%d", n);
  path(dir, size, root, name);
  assert(mkdir(dir, 0700) == 0);
  assert(enil_login_store_prepare(dir, "desktopwin", mid));
}
static void complete(const char *pending) {
  patch(pending, "{\"accessToken\":\"synthetic-access\",\"refreshToken\":\"synthetic-refresh\","
                 "\"mid\":\"synthetic-mid\",\"nativeTokenRequestStarted\":true,"
                 "\"nativeLoginResult\":{\"10\":{\"encryptedKeyChain\":\"retained\"}}}");
}
static void activation_cycles(const char *root) {
  char stage[4096], account[4096], marker[4096];
  char *attempts[4];
  int i, j;
  path(account, sizeof(account), root, "synthetic-mid");
  for (i = 0; i < 4; i++) {
    cJSON *saved;
    staging(stage, sizeof(stage), root, i, i ? "synthetic-mid" : NULL);
    attempts[i] = enil_login_store_select(stage);
    assert(attempts[i]);
    for (j = 0; j < i; j++)
      assert(strcmp(attempts[j], attempts[i]));
    complete(attempts[i]);
    assert(enil_login_store_stage(stage, attempts[i]));
    assert(enil_login_store_activate(stage, account));
    path(marker, sizeof(marker), attempts[i], "retired.json");
    assert(access(marker, F_OK) == 0);
    patch(account, "{\"accessToken\":\"rotated-live-token\"}");
    saved = read_saved(account);
    assert(!strcmp(cJSON_GetObjectItem(saved, "nativeLoginSource")->valuestring,
                   strrchr(attempts[i], '/') + 1));
    cJSON_Delete(saved);
    /* Simulate interruption before marking consumed: login generations alone
     * still prevent A being selected after B replaced it. */
    assert(unlink(marker) == 0);
  }
  for (i = 0; i < 4; i++)
    free(attempts[i]);
}
static void restart_uncertain(const char *root) {
  char first[4096], second[4096], third[4096], marker[4096];
  char *pending, *resumed, *fresh, *other;
  cJSON *saved;
  staging(first, sizeof(first), root, 10, NULL);
  pending = enil_login_store_select(first);
  assert(pending);
  patch(pending,
        "{\"nativeTokenRequestStarted\":true,\"lastNativeResponseBase64\":\"saved-reply\"}");
  staging(second, sizeof(second), root, 11, NULL);
  resumed = enil_login_store_select(second);
  assert(resumed && !strcmp(resumed, pending));
  assert(enil_login_store_can_restart(second));
  assert(enil_login_store_restart(second));
  saved = read_saved(pending);
  assert(
      !strcmp(cJSON_GetObjectItem(saved, "lastNativeResponseBase64")->valuestring, "saved-reply"));
  cJSON_Delete(saved);
  path(marker, sizeof(marker), pending, "retired.json");
  assert(access(marker, F_OK) == 0);
  /* A second abandoned attempt must not hijack an explicit start-new action. */
  staging(third, sizeof(third), root, 12, NULL);
  other = enil_login_store_select(third);
  assert(other);
  patch(other, "{\"nativeTokenRequestStarted\":true}");
  fresh = enil_login_store_select(second);
  assert(fresh && strcmp(fresh, pending) && strcmp(fresh, other));
  saved = read_saved(fresh);
  assert(!cJSON_HasObjectItem(saved, "nativeTokenRequestStarted"));
  assert(!cJSON_HasObjectItem(saved, "accessToken"));
  assert(cJSON_HasObjectItem(saved, "loginId"));
  cJSON_Delete(saved);
  free(pending);
  free(resumed);
  free(fresh);
  free(other);
}
static void import_saved_and_retire(const char *root) {
  char legacy[4096], file[4096], stage[4096], account[4096], marker[4096];
  char *selected;
  cJSON *saved, *journal = cJSON_CreateObject();
  path(legacy, sizeof(legacy), root, ".windows-login-probe");
  assert(mkdir(legacy, 0700) == 0);
  path(file, sizeof(file), legacy, "session.json");
  assert(enil_session_prepare_login(file, "desktopwin", NULL));
  complete(legacy);
  staging(stage, sizeof(stage), root, 20, NULL);
  selected = enil_login_store_select(stage);
  assert(selected && !strcmp(selected, legacy));
  assert(enil_login_store_stage(stage, selected));
  path(account, sizeof(account), root, "synthetic-mid");
  assert(mkdir(account, 0700) == 0);
  path(file, sizeof(file), account, "session.json.refresh-pending");
  cJSON_AddStringToObject(journal, "responseBody", "old-refresh-reply");
  assert(enil_session_write(file, journal));
  cJSON_Delete(journal);
  assert(enil_login_store_activate(stage, account));
  assert(access(file, F_OK) != 0);
  path(marker, sizeof(marker), legacy, "retired.json");
  assert(access(marker, F_OK) == 0);
  saved = read_saved(legacy);
  assert(cJSON_HasObjectItem(saved, "nativeLoginResult"));
  cJSON_Delete(saved);
  free(selected);
  /* A removed account must not be silently resurrected from consumed tokens. */
  path(file, sizeof(file), account, "session.json");
  assert(unlink(file) == 0);
  staging(stage, sizeof(stage), root, 21, NULL);
  selected = enil_login_store_select(stage);
  assert(selected && strcmp(selected, legacy));
  free(selected);
}

static void reject_superseded_save(const char *root, int legacy) {
  char stage[4096], account[4096], file[4096];
  char *pending;
  session_t stale;
  cJSON *expected, *saved;
  path(account, sizeof(account), root, "synthetic-mid");
  path(file, sizeof(file), account, "session.json");
  staging(stage, sizeof(stage), root, 30, NULL);
  pending = enil_login_store_select(stage);
  assert(pending);
  complete(pending);
  assert(enil_login_store_stage(stage, pending));
  assert(enil_login_store_activate(stage, account));
  free(pending);
  if (legacy) {
    saved = read_saved(account);
    cJSON_DeleteItemFromObject(saved, "loginId");
    assert(enil_session_write(file, saved));
    cJSON_Delete(saved);
  }
  assert(enil_session_load(file, &stale));
  free(stale.accessToken);
  stale.accessToken = strdup("refreshed-A");
  free(stale.refreshToken);
  stale.refreshToken = strdup("refreshed-A-refresh");
  /* An ordinary refresh within the same login (including legacy) works. */
  assert(enil_session_save(file, &stale));
  enil_session_free(&stale);
  assert(enil_session_load(file, &stale));

  staging(stage, sizeof(stage), root, 31, "synthetic-mid");
  pending = enil_login_store_select(stage);
  assert(pending);
  complete(pending);
  patch(pending, "{\"accessToken\":\"access-B\",\"refreshToken\":\"refresh-B\","
                 "\"workerRestoreState\":{\"key\":\"B\"}}");
  assert(enil_login_store_stage(stage, pending));
  assert(enil_login_store_activate(stage, account));
  free(pending);
  expected = read_saved(account);

  /* Both credential and unrelated sync/key changes from A are rejected. */
  free(stale.accessToken);
  stale.accessToken = strdup("late-refreshed-A");
  stale.reqSeq = 123;
  assert(!enil_session_save(file, &stale));
  saved = read_saved(account);
  assert(cJSON_Compare(expected, saved, 1));
  cJSON_Delete(saved);
  cJSON_Delete(expected);
  enil_session_free(&stale);

  /* In-flight work must not recreate an account after removal either. */
  assert(enil_session_load(file, &stale));
  assert(unlink(file) == 0);
  stale.reqSeq++;
  assert(!enil_session_save(file, &stale));
  assert(access(file, F_OK) != 0);
  enil_session_free(&stale);
}

static void directory_durability(const char *root) {
  char file[4096];
  cJSON *value = cJSON_Parse("{\"accessToken\":\"synthetic\"}"), *saved;
  path(file, sizeof(file), root, "session.json");
  assert(value && enil_session_write(file, value));
  assert(file_syncs == 1 && directory_syncs == 1);

  /* A directory flush failure must not be reported as a durable commit.
   * The renamed credentials remain available for recovery, not rolled back. */
  fail_directory_sync = 1;
  cJSON_ReplaceItemInObject(value, "accessToken", cJSON_CreateString("rotated"));
  assert(!enil_session_write(file, value));
  assert(file_syncs == 2 && directory_syncs == 2);
  saved = enil_session_read(file);
  assert(cJSON_Compare(saved, value, 1));
  cJSON_Delete(saved);
  fail_directory_sync = 0;
  assert(enil_session_write(file, value));
  assert(enil_session_retire_file(file));
  assert(access(file, F_OK) != 0 && directory_syncs == 4);

  /* Relative paths must flush '.', including recovery journal retirement. */
  assert(chdir(root) == 0);
  assert(enil_session_write("session.json", value));
  fail_directory_sync = 1;
  assert(!enil_session_retire_file("session.json"));
  assert(access("session.json", F_OK) != 0 && directory_syncs == 6);
  cJSON_Delete(value);
}

static void android_pending_isolation(const char *root) {
  char windows[4096], android[4096], retry[4096], account[4096], reauth[4096];
  char *win_pending, *android_pending, *resumed;
  cJSON *saved;
  enil_identity_t identity;
  staging(windows, sizeof(windows), root, 50, NULL);
  win_pending = enil_login_store_select(windows);
  assert(win_pending);
  patch(win_pending, "{\"nativeTokenRequestStarted\":true}");
  path(android, sizeof(android), root, ".staging-android");
  assert(mkdir(android, 0700) == 0);
  assert(enil_login_store_prepare(android, "android", NULL));
  android_pending = enil_login_store_select(android);
  assert(android_pending && strcmp(android_pending, win_pending));
  complete(android_pending);
  path(retry, sizeof(retry), root, ".staging-android-retry");
  assert(mkdir(retry, 0700) == 0);
  assert(enil_login_store_prepare(retry, "android", NULL));
  resumed = enil_login_store_select(retry);
  assert(resumed && !strcmp(resumed, android_pending));
  assert(enil_login_store_stage(retry, resumed));
  path(account, sizeof(account), root, "synthetic-mid");
  assert(enil_login_store_activate(retry, account));
  path(reauth, sizeof(reauth), root, ".staging-android-reauth");
  assert(mkdir(reauth, 0700) == 0);
  assert(enil_login_store_prepare(reauth, NULL, "synthetic-mid"));
  saved = read_saved(reauth);
  assert(enil_identity_parse(cJSON_GetObjectItem(saved, "clientIdentity"), &identity));
  assert(!strcmp(identity.profile_id, "android"));
  cJSON_Delete(saved);
  free(win_pending); free(android_pending); free(resumed);
}

int main(int argc, char **argv) {
  assert(argc == 3);
  if (!strcmp(argv[2], "durability"))
    directory_durability(argv[1]);
  else if (!strcmp(argv[2], "cycles"))
    activation_cycles(argv[1]);
  else if (!strcmp(argv[2], "restart"))
    restart_uncertain(argv[1]);
  else if (!strcmp(argv[2], "import"))
    import_saved_and_retire(argv[1]);
  else if (!strcmp(argv[2], "android"))
    android_pending_isolation(argv[1]);
  else if (!strcmp(argv[2], "stale") || !strcmp(argv[2], "stale-legacy"))
    reject_superseded_save(argv[1], !strcmp(argv[2], "stale-legacy"));
  else
    assert(0);
  return 0;
}
