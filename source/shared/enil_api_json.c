/* ============================================================================
 * cJSON helpers — typed field extraction with ownership-safe string dup.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "enil_api_json.h"
#include "enil_api_types.h"
#include "enil_cocoa_log.h"

#define LOG(fn, msg) enil_log("ApiJson." fn, "%s", msg)

/* ============================================================================
 * Release the owned (string / cJSON) fields of a struct described by a
 * enil_field_t table. The same descriptor tables drive ENILStructDict and the
 * typed JSON parse, so a struct's teardown is derived from one source of truth
 * rather than re-listed by hand. The `raw` cJSON is not in the tables; callers
 * free it explicitly.
 * ==========================================================================*/
void enil_field_free_all(void *base, const enil_field_t *fields, size_t count) {
  size_t i;
  if (!base || !fields) return;
  for (i = 0; i < count; i++) {
    void *p = (char *)base + fields[i].offset;
    switch (fields[i].type) {
      case ENIL_F_STR: {
        char **sp = (char **)p;
        free(*sp); *sp = NULL;
        break;
      }
      case ENIL_F_JSON: {
        cJSON **jp = (cJSON **)p;
        if (*jp) { cJSON_Delete(*jp); *jp = NULL; }
        break;
      }
      case ENIL_F_INT:
      case ENIL_F_BOOL:
      case ENIL_F_I64:
        break; /* scalars own nothing */
    }
  }
}

/* ============================================================================
 * Duplicate a required string field. Returns -1 if absent or non-string.
 * ==========================================================================*/
int enil_json_dup_string(cJSON *obj, const char *key, char **out) {
  cJSON *item;
  if (!obj || !key || !out) { LOG("dup_string", "NULL argument"); return -1; }
  item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsString(item) || !item->valuestring) return -1;
  *out = strdup(item->valuestring);
  return *out ? 0 : -1;
}

/* ============================================================================
 * Duplicate an optional string field. Missing/non-string yields *out=NULL, 0.
 * ==========================================================================*/
int enil_json_dup_string_opt(cJSON *obj, const char *key, char **out) {
  cJSON *item;
  if (!obj || !key || !out) { LOG("dup_string_opt", "NULL argument"); return -1; }
  *out = NULL;
  item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsString(item) || !item->valuestring) return 0;
  *out = strdup(item->valuestring);
  return *out ? 0 : -1;
}

/* ============================================================================
 * Extract a required int field. Returns -1 if absent or non-numeric.
 * ==========================================================================*/
int enil_json_get_int(cJSON *obj, const char *key, int *out) {
  cJSON *item;
  if (!obj || !key || !out) { LOG("get_int", "NULL argument"); return -1; }
  item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(item)) return -1;
  *out = (int)item->valuedouble;
  return 0;
}

/* ============================================================================
 * Extract a required boolean field as 0/1. Returns -1 if absent or non-bool.
 * ==========================================================================*/
int enil_json_get_bool(cJSON *obj, const char *key, int *out) {
  cJSON *item;
  if (!obj || !key || !out) { LOG("get_bool", "NULL argument"); return -1; }
  item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsBool(item)) return -1;
  *out = cJSON_IsTrue(item) ? 1 : 0;
  return 0;
}

/* ============================================================================
 * Coerce a cJSON item to int64: accepts JSON number or numeric string, and
 * yields 0 for NULL/absent/any other type. This is the item-taking primitive;
 * LINE's Thrift-JSON routinely encodes large integers as strings, so call
 * sites that read such fields share this one coercion instead of re-rolling it.
 * ==========================================================================*/
long long enil_json_coerce_int64(cJSON *item) {
  if (cJSON_IsNumber(item)) return (long long)item->valuedouble;
  if (cJSON_IsString(item) && item->valuestring)
    return strtoll(item->valuestring, NULL, 10);
  return 0;
}

/* ============================================================================
 * Extract a required int64 field — accepts JSON number or numeric string.
 * Returns -1 (distinct from a coerced 0) when the key is absent/another type.
 * ==========================================================================*/
int enil_json_get_int64(cJSON *obj, const char *key, long long *out) {
  cJSON *item;
  if (!obj || !key || !out) { LOG("get_int64", "NULL argument"); return -1; }
  item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(item) && !(cJSON_IsString(item) && item->valuestring))
    return -1;
  *out = enil_json_coerce_int64(item);
  return 0;
}
