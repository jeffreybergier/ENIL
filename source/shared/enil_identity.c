#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "enil_identity.h"

/* Frozen migration value. Introduce a separate default if new Chrome logins
 * ever need newer values; never change this fallback for old sessions. */
static const enil_identity_t legacy_chrome_identity = {
  "chrome", "chrome-gateway", "CHROMEOS\t3.7.2\tChrome_OS",
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
  "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36",
  "CHROMEOS", "CHROME", "3.7.2"
};
/* Reference profile: evex-dev/linejs ef6c3d9, devices.ts / request/mod.ts.
 * Persist the resolved values; future default updates must not mutate sessions. */
static const enil_identity_t windows_identity = {
  "desktopwin", "native-thrift", "DESKTOPWIN\t9.7.0.3556\tWINDOWS\t10.0.0-NT-x64",
  "Line/9.7.0.3556", "WINDOWS", "DESKTOPWIN", "3.7.2"
};

int enil_identity_default(const char *profile_id, enil_identity_t *out) {
  if (!profile_id || !out) return 0;
  if (strcmp(profile_id, "chrome") == 0) *out = legacy_chrome_identity;
  else if (strcmp(profile_id, "desktopwin") == 0) *out = windows_identity;
  else return 0;
  return 1;
}

static int read_string(const cJSON *json, const char *key, char *out,
                       size_t size, int allow_tabs) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
  const unsigned char *p;
  size_t n;
  if (!cJSON_IsString(item) || !item->valuestring) return 0;
  n = strlen(item->valuestring);
  if (!n || n >= size) return 0;
  for (p = (const unsigned char *)item->valuestring; *p; ++p) {
    if ((*p < 32 && !(allow_tabs && *p == '\t')) || *p == 127) return 0;
  }
  memcpy(out, item->valuestring, n + 1);
  return 1;
}

int enil_identity_parse(const cJSON *json, enil_identity_t *out) {
  enil_identity_t value, known;
  const cJSON *version, *transport;
  const char *prefix;
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  if (!json) { *out = legacy_chrome_identity; return 1; }
  if (!cJSON_IsObject(json)) return 0;
  version = cJSON_GetObjectItemCaseSensitive(json, "schemaVersion");
  transport = cJSON_GetObjectItemCaseSensitive(json, "transport");
  if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
      !cJSON_IsString(transport) ||
      (strcmp(transport->valuestring, "chrome-gateway") != 0 &&
       strcmp(transport->valuestring, "native-thrift") != 0)) return 0;
  memset(&value, 0, sizeof(value));
  if (!read_string(json, "profileId", value.profile_id, sizeof(value.profile_id), 0) ||
      !enil_identity_default(value.profile_id, &known) ||
      !read_string(json, "application", value.application, sizeof(value.application), 1) ||
      !read_string(json, "userAgent", value.user_agent, sizeof(value.user_agent), 0) ||
      !read_string(json, "systemName", value.system_name, sizeof(value.system_name), 0) ||
      !read_string(json, "modelName", value.model_name, sizeof(value.model_name), 0) ||
      !read_string(json, "gatewayVersion", value.gateway_version, sizeof(value.gateway_version), 0))
    return 0;
  if (!strcmp(transport->valuestring, "native-thrift") && strcmp(value.profile_id, "desktopwin")) return 0;
  strcpy(value.transport, transport->valuestring);
  prefix = strcmp(value.profile_id, "chrome") == 0 ? "CHROMEOS\t" : "DESKTOPWIN\t";
  if (strncmp(value.application, prefix, strlen(prefix)) != 0) return 0;
  *out = value;
  return 1;
}

cJSON *enil_identity_to_json(const enil_identity_t *identity) {
  cJSON *json;
  if (!identity) return NULL;
  json = cJSON_CreateObject();
  if (!json) return NULL;
  if (!cJSON_AddNumberToObject(json, "schemaVersion", 1) ||
      !cJSON_AddStringToObject(json, "transport", identity->transport) ||
      !cJSON_AddStringToObject(json, "profileId", identity->profile_id) ||
      !cJSON_AddStringToObject(json, "application", identity->application) ||
      !cJSON_AddStringToObject(json, "userAgent", identity->user_agent) ||
      !cJSON_AddStringToObject(json, "systemName", identity->system_name) ||
      !cJSON_AddStringToObject(json, "modelName", identity->model_name) ||
      !cJSON_AddStringToObject(json, "gatewayVersion", identity->gateway_version)) {
    cJSON_Delete(json);
    return NULL;
  }
  return json;
}

static pthread_key_t current_key;
static pthread_once_t current_once = PTHREAD_ONCE_INIT;
static int key_ready;
static void make_key(void) { key_ready = pthread_key_create(&current_key, free) == 0; }

const enil_identity_t *enil_identity_current(void) {
  pthread_once(&current_once, make_key);
  return key_ready ? (const enil_identity_t *)pthread_getspecific(current_key) : NULL;
}

int enil_identity_bind(const enil_identity_t *identity) {
  enil_identity_t *copy = NULL;
  void *old;
  pthread_once(&current_once, make_key);
  if (!key_ready) return 0;
  old = pthread_getspecific(current_key);
  if (pthread_setspecific(current_key, NULL) != 0) return 0;
  /* Copy before releasing old: callers may bind the current snapshot. */
  if (identity) {
    copy = (enil_identity_t *)malloc(sizeof(*copy));
    if (copy) *copy = *identity;
  }
  free(old);
  if (identity && !copy) return 0;
  if (pthread_setspecific(current_key, copy) != 0) { free(copy); return 0; }
  return 1;
}
