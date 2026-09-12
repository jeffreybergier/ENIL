/* ============================================================================
 * Generic LINE TalkService request dispatcher — builds, signs, sends, and
 * unwraps the standard {"message":"OK","data":...} envelope.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "enil_line.h"
#include "enil_api_call.h"
#include "enil_cocoa_log.h"

/* ============================================================================
 * Validates HTTP 200 + {"message":"OK"} and detaches the inner "data" object.
 * Returned cJSON is owned by the caller. `tag` is the full log prefix.
 * Exported so the hand-rolled TalkService wrappers share this one copy of the
 * envelope check rather than re-implementing it.
 * ==========================================================================*/
cJSON *enil_api_unwrap_ok(const char *tag, ENILLineResponse *r) {
  cJSON *root, *message, *data;
  if (r->status != 200) {
    ENIL_LOG(tag, "HTTP %ld", r->status);
    return NULL;
  }
  root = cJSON_Parse(r->body);
  if (!root) { ENIL_LOG(tag, "invalid JSON"); return NULL; }
  message = cJSON_GetObjectItem(root, "message");
  if (!cJSON_IsString(message) || strcmp(message->valuestring, "OK") != 0) {
    ENIL_LOG(tag, "non-OK: %s", r->body ? r->body : "");
    cJSON_Delete(root);
    return NULL;
  }
  data = cJSON_DetachItemFromObject(root, "data");
  cJSON_Delete(root);
  return data;
}

/* ============================================================================
 * Endpoint dispatcher — runs build_fn → POST → unwrap → parse_fn and writes
 * the parsed struct into *out. Returns 0 on success, -1 on any failure.
 * ==========================================================================*/
int enil_api_call(
  const enil_endpoint_t *endpoint,
  const char            *access_token,
  const void            *req,
  enil_build_fn          build_fn,
  void                  *out,
  enil_parse_fn          parse_fn)
{
  cJSON           *body_json = NULL;
  char            *body_str  = NULL;
  ENILLineResponse resp = { 0, NULL };
  cJSON           *data = NULL;
  int              rc;
  char             tag[128];

  if (!endpoint || !parse_fn || !out) {
    ENIL_LOG("ApiCall.call", "NULL argument");
    return -1;
  }
  snprintf(tag, sizeof(tag), "ApiCall.%s", endpoint->name);

  if (endpoint->needs_auth && !access_token) {
    ENIL_LOG(tag, "missing access_token");
    return -1;
  }

  if (build_fn) {
    body_json = build_fn(req);
    if (!body_json) { ENIL_LOG(tag, "builder returned NULL"); return -1; }
    body_str = cJSON_PrintUnformatted(body_json);
    cJSON_Delete(body_json);
    if (!body_str) { ENIL_LOG(tag, "serialize failed"); return -1; }
  }

  if (endpoint->method != ENIL_HTTP_POST) {
    ENIL_LOG(tag, "only POST is implemented");
    free(body_str);
    return -1;
  }

  resp = enil_line_post(endpoint->path, body_str ? body_str : "[]", access_token);
  free(body_str);

  data = enil_api_unwrap_ok(tag, &resp);
  enil_line_response_free(&resp);
  if (!data) return -1;

  rc = parse_fn(data, out);
  cJSON_Delete(data);
  if (rc != 0) {
    ENIL_LOG(tag, "parser returned %d", rc);
    return -1;
  }
  return 0;
}
