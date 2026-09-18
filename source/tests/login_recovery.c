/* Local filesystem recovery, using the production lifecycle and synthetic data. */
#include "enil_login_store.h"
#include "enil_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
int main(int argc, char **argv) {
  assert(argc == 3);
  if (!strcmp(argv[2], "cycles"))
    activation_cycles(argv[1]);
  else if (!strcmp(argv[2], "restart"))
    restart_uncertain(argv[1]);
  else if (!strcmp(argv[2], "import"))
    import_saved_and_retire(argv[1]);
  else
    assert(0);
  return 0;
}
