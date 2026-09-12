#ifndef ENIL_API_JSON_H
#define ENIL_API_JSON_H

#include "cJSON.h"

/* All return 0 on success, -1 on failure. */

int enil_json_dup_string(cJSON *obj, const char *key, char **out);
int enil_json_dup_string_opt(cJSON *obj, const char *key, char **out);
int enil_json_get_int(cJSON *obj, const char *key, int *out);
int enil_json_get_bool(cJSON *obj, const char *key, int *out);
int enil_json_get_int64(cJSON *obj, const char *key, long long *out);

/* Item-taking int64 coercion (number or numeric string -> value, else 0).
 * Unlike the key-taking accessor above this never signals "absent"; callers
 * that only want a best-effort value use this. */
long long enil_json_coerce_int64(cJSON *item);

#endif
