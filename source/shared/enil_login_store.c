#include "enil_login_store.h"
#include "enil_session.h"
#include "enil_cocoa_log.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *string(cJSON *root, const char *key) {
  cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
  return cJSON_IsString(value) ? value->valuestring : NULL;
}

static int safe_name(const char *name) {
  return name && *name && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

static int pending_name(const char *name) {
  return safe_name(name) &&
         (!strcmp(name, ".windows-login-probe") || !strncmp(name, ".native-login-", 14));
}

static char *join(const char *dir, const char *name) {
  char *path;
  if (!dir || !name)
    return NULL;
  path = malloc(strlen(dir) + strlen(name) + 2);
  if (path)
    sprintf(path, "%s/%s", dir, name);
  return path;
}

static char *parent(const char *path) {
  char *copy = path ? strdup(path) : NULL;
  char *slash = copy ? strrchr(copy, '/') : NULL;
  if (!slash || slash == copy) {
    free(copy);
    return NULL;
  }
  *slash = 0;
  return copy;
}

static cJSON *read_session(const char *directory) {
  char *path = join(directory, "session.json");
  cJSON *root = path ? enil_session_read(path) : NULL;
  free(path);
  return root;
}

static int write_session(const char *directory, cJSON *root) {
  char *path = join(directory, "session.json");
  int ok = path && enil_session_write(path, root);
  free(path);
  return ok;
}

static int retired(const char *directory) {
  char *path = join(directory, "retired.json");
  int result = !path || access(path, F_OK) == 0;
  free(path);
  return result;
}

/* Use the same lock as the QR handshake so an explicit restart cannot retire
 * an attempt while it is still issuing credentials in another window. */
static int retire(const char *directory) {
  char *lock = join(directory, "session.lock");
  char *path = join(directory, "retired.json");
  int fd = -1, ok = 0;
  cJSON *record = cJSON_CreateObject();
  if (!lock || !path || !record)
    goto done;
  fd = open(lock, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
  if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0)
    goto done;
  cJSON_AddBoolToObject(record, "retired", 1);
  ok = enil_session_write(path, record);
done:
  if (fd >= 0)
    close(fd);
  free(lock);
  free(path);
  cJSON_Delete(record);
  return ok;
}

int enil_login_store_prepare(const char *staging_dir, const char *profile,
                             const char *expected_mid) {
  char *root = parent(staging_dir), *path = join(staging_dir, "session.json");
  char *account = NULL, *source = NULL, *login_id = NULL;
  cJSON *patch = NULL;
  int ok = 0;
  if (!root || !path)
    goto done;
  if (expected_mid) {
    if (!safe_name(expected_mid))
      goto done;
    account = join(root, expected_mid);
    source = join(account, "session.json");
    login_id = enil_session_login_id(source);
    if (!login_id)
      goto done;
  }
  if (!enil_session_prepare_login(path, profile, source))
    goto done;
  ok = 1;
  if (expected_mid) {
    patch = cJSON_CreateObject();
    ok = patch && cJSON_AddStringToObject(patch, "reauthMid", expected_mid) &&
         cJSON_AddStringToObject(patch, "reauthLoginId", login_id) &&
         enil_session_patch(path, patch);
  }
done:
  free(root);
  free(path);
  free(account);
  free(source);
  free(login_id);
  cJSON_Delete(patch);
  return ok;
}

static int matches(cJSON *staged, cJSON *prior, const char *root, const char *name) {
  const char *expected = string(staged, "reauthMid");
  const char *prior_expected = string(prior, "reauthMid");
  const char *mid = string(prior, "mid");
  cJSON *active = NULL;
  char *account = NULL;
  int ok = 0;
  enil_identity_t wanted, saved;
  if (!cJSON_IsObject(prior) ||
      !enil_identity_parse(cJSON_GetObjectItem(staged, "clientIdentity"), &wanted) ||
      !enil_identity_parse(cJSON_GetObjectItem(prior, "clientIdentity"), &saved) ||
      memcmp(&wanted, &saved, sizeof(saved)))
    return 0;
  if ((expected || prior_expected) &&
      (!expected || !prior_expected || strcmp(expected, prior_expected)))
    return 0;
  if (expected) {
    const char *generation = string(staged, "reauthLoginId");
    const char *prior_generation = string(prior, "reauthLoginId");
    /* Legacy reauth attempts lack a baseline and cannot safely replace an
     * active account. New attempts match one exact login, not rotating tokens. */
    if (!generation || !prior_generation || strcmp(generation, prior_generation))
      return 0;
  }
  if (mid) {
    const char *source, *generation, *prior_id;
    if (!safe_name(mid))
      return 0;
    account = join(root, mid);
    active = read_session(account);
    source = string(active, "nativeLoginSource");
    generation = string(active, "loginId");
    prior_id = string(prior, "loginId");
    if ((!expected && active) || (source && !strcmp(source, name)) ||
        (generation && prior_id && !strcmp(generation, prior_id)))
      goto done;
    if (expected && !strcmp(expected, mid) && active) {
      const char *baseline = string(prior, "reauthLoginId");
      if (!generation || !baseline || strcmp(generation, baseline))
        goto done;
    }
  }
  ok = string(prior, "accessToken") != NULL ||
       cJSON_IsTrue(cJSON_GetObjectItem(prior, "nativeTokenRequestStarted"));
done:
  free(account);
  cJSON_Delete(active);
  return ok;
}

char *enil_login_store_select(const char *staging_dir) {
  char *root = parent(staging_dir), *selected = NULL, *path = NULL;
  cJSON *staged = read_session(staging_dir), *patch = NULL;
  DIR *directory = NULL;
  struct dirent *entry;
  const char *source;
  if (!root || !cJSON_IsObject(staged))
    goto done;
  source = string(staged, "nativeLoginSource");
  if (pending_name(source)) {
    selected = join(root, source);
    if (selected && retired(selected)) {
      free(selected);
      selected = NULL;
    }
  }
  directory = opendir(root);
  if (!directory)
    goto done;
  while (!selected && !cJSON_IsTrue(cJSON_GetObjectItem(staged, "nativeLoginFresh")) &&
         (entry = readdir(directory))) {
    cJSON *prior;
    char *candidate;
    if (!pending_name(entry->d_name))
      continue;
    candidate = join(root, entry->d_name);
    prior = read_session(candidate);
    if (candidate && !retired(candidate) && matches(staged, prior, root, entry->d_name))
      selected = candidate;
    else
      free(candidate);
    cJSON_Delete(prior);
  }
  if (!selected) {
    selected = join(root, ".native-login-XXXXXX");
    if (!selected || !mkdtemp(selected) || !write_session(selected, staged)) {
      free(selected);
      selected = NULL;
      goto done;
    }
  }
  patch = cJSON_CreateObject();
  path = join(staging_dir, "session.json");
  if (!patch || !path ||
      !cJSON_AddStringToObject(patch, "nativeLoginSource", strrchr(selected, '/') + 1) ||
      !enil_session_patch(path, patch)) {
    free(selected);
    selected = NULL;
  }
done:
  if (directory)
    closedir(directory);
  free(root);
  free(path);
  cJSON_Delete(staged);
  cJSON_Delete(patch);
  return selected;
}

int enil_login_store_stage(const char *staging_dir, const char *pending_dir) {
  cJSON *ready;
  char *path = join(pending_dir, "session.json"), *id;
  int ok = 0;
  id = enil_session_login_id(path);
  ready = read_session(pending_dir);
  if (id && !retired(pending_dir) && string(ready, "accessToken") && string(ready, "mid")) {
    cJSON_DeleteItemFromObjectCaseSensitive(ready, "nativeLoginSource");
    if (cJSON_AddStringToObject(ready, "nativeLoginSource", strrchr(pending_dir, '/') + 1))
      ok = write_session(staging_dir, ready);
  }
  free(path);
  free(id);
  cJSON_Delete(ready);
  return ok;
}

int enil_login_store_can_restart(const char *staging_dir) {
  cJSON *staged = read_session(staging_dir);
  int ok = pending_name(string(staged, "nativeLoginSource"));
  cJSON_Delete(staged);
  return ok;
}

int enil_login_store_restart(const char *staging_dir) {
  cJSON *staged = read_session(staging_dir), *fresh = cJSON_CreateObject();
  const char *source = string(staged, "nativeLoginSource");
  const char *fields[] = {"clientIdentity", "reauthMid", "reauthLoginId"};
  char *root = parent(staging_dir), *pending = NULL, *path = NULL, *id = NULL;
  int i, ok = 0;
  if (!root || !fresh || !pending_name(source))
    goto done;
  pending = join(root, source);
  if (!pending || !retire(pending))
    goto done;
  for (i = 0; i < 3; i++) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(staged, fields[i]);
    if (value && !cJSON_AddItemToObject(fresh, fields[i], cJSON_Duplicate(value, 1)))
      goto done;
  }
  if (!cJSON_AddBoolToObject(fresh, "nativeLoginFresh", 1))
    goto done;
  if (!write_session(staging_dir, fresh))
    goto done;
  path = join(staging_dir, "session.json");
  id = enil_session_login_id(path);
  ok = id != NULL;
done:
  free(root);
  free(pending);
  free(path);
  free(id);
  cJSON_Delete(staged);
  cJSON_Delete(fresh);
  return ok;
}

int enil_login_store_activate(const char *staging_dir, const char *account_dir) {
  cJSON *staged = read_session(staging_dir);
  const char *mid = string(staged, "mid"), *source = string(staged, "nativeLoginSource");
  const char *name = account_dir ? strrchr(account_dir, '/') : NULL;
  char *root = parent(account_dir), *pending = NULL, *path = NULL;
  int ok = 0;
  if (!root || !name || !safe_name(mid) || strcmp(name + 1, mid) || !string(staged, "accessToken"))
    goto done;
  if (mkdir(account_dir, 0700) != 0 && errno != EEXIST)
    goto done;
  path = join(account_dir, "session.json");
  if (!path || !enil_session_activate(path, staged))
    goto done;
  ok = 1;
  /* The active loginId/source already prevent replay if retirement fails or
   * the process stops here. Retirement also survives later account removal. */
  if (pending_name(source)) {
    pending = join(root, source);
    if (!pending || !retire(pending))
      ENIL_LOG("LoginStore.activate", "Could not retire consumed login directory");
  }
done:
  free(root);
  free(pending);
  free(path);
  cJSON_Delete(staged);
  return ok;
}
