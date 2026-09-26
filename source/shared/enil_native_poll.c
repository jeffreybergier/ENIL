/* Native Thrift operation polling; shares only the event callback and cursor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "enil_sse_internal.h"
#include "enil_line.h"
#include "enil_session.h"
#include "enil_health.h"
#include "cJSON.h"

/* Native sync uses the same event callback as Chrome SSE. A failed event
 * retains its cursor for redelivery; later operations are not dispatched. */
static int native_dispatch(ENILSSEClient *c, const char *type, cJSON *data) {
  ENILSSEEvent ev;
  char *json = cJSON_PrintUnformatted(data);
  long long rev;
  if (!json)
    return 0;
  ev.type = type;
  ev.data = json;
  if ((c->fn && !c->fn(&ev, c->ctx)) || enil_health_any_failed() || c->stop) {
    free(json);
    return 0;
  }
  if (strcmp(type, "fullSync")) {
    rev = enil_sse_event_revision(type, json);
    if (!enil_sse_save_local_rev(c, rev)) {
      free(json);
      return 0;
    }
  }
  free(json);
  return 1;
}
void enil_native_poll(ENILSSEClient *c) {
  int failures = 0;
  while (!c->stop && !enil_health_any_failed()) {
    cJSON *saved, *args, *req;
    cJSON *v, *root = NULL, *data, *ops, *op, *patch = NULL;
    char revision[32], *body = NULL;
    const char *token;
    ENILLineResponse response = {0, NULL};
    int ok = 1, delay = 1, i, resync = 0;
    /* Clear a completed kick before the next request. A concurrent stop is
     * sticky and must be checked after clearing the request's cancel flag. */
    c->reconnect = 0;
    c->interrupt = 0;
    if (c->stop) break;
    if (!enil_session_continue_identity(c->session_path)) break;
    saved = enil_session_read(c->session_path);
    args = cJSON_CreateArray();
    req = cJSON_CreateObject();
    v = cJSON_GetObjectItemCaseSensitive(saved, "accessToken");
    token = cJSON_IsString(v) ? v->valuestring : NULL;
    if (!token) {
      cJSON_Delete(saved);
      cJSON_Delete(args);
      cJSON_Delete(req);
      break;
    }
    snprintf(revision, sizeof(revision), "%lld", c->local_rev);
    cJSON_AddStringToObject(req, "lastRevision", revision);
    cJSON_AddNumberToObject(req, "count", 100);
    v = cJSON_GetObjectItemCaseSensitive(saved, "nativeGlobalRevision");
    cJSON_AddItemToObject(req, "lastGlobalRevision",
                          v ? cJSON_Duplicate(v, 1) : cJSON_CreateString("0"));
    v = cJSON_GetObjectItemCaseSensitive(saved, "nativeIndividualRevision");
    cJSON_AddItemToObject(req, "lastIndividualRevision",
                          v ? cJSON_Duplicate(v, 1) : cJSON_CreateString("0"));
    v = cJSON_GetObjectItemCaseSensitive(saved, "lastPartialFullSyncs");
    if (v)
      cJSON_AddItemToObject(req, "lastPartialFullSyncs", cJSON_Duplicate(v, 1));
    cJSON_AddItemToArray(args, req);
    body = cJSON_PrintUnformatted(args);
    if (body)
      response = enil_line_post_ex("/native/sync", body, token, NULL, 0, 35000, &c->interrupt);
    if (response.status == 204 || c->reconnect || c->stop) {
      /* A completed idle poll breaks the failure streak just like a reply.
       * A user cancellation alone is not evidence of a healthy connection. */
      if (response.status == 204 && !c->reconnect && !c->stop)
        failures = 0;
      cJSON_Delete(saved);
      cJSON_Delete(args);
      free(body);
      enil_line_response_free(&response);
      continue; /* idle timeouts and explicit cancellation are not failures */
    }
    if (response.status == 400 && response.body) {
      root = cJSON_Parse(response.body);
      if (root)
        native_dispatch(c, "talkException", root);
      cJSON_Delete(saved);
      cJSON_Delete(args);
      cJSON_Delete(root);
      free(body);
      enil_line_response_free(&response);
      break;
    }
    if (response.status == 200 && response.body)
      root = cJSON_Parse(response.body);
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!data)
      ok = 0;
    v = cJSON_GetObjectItemCaseSensitive(data, "fullSyncResponse");
    if (v && ok) {
      native_dispatch(c, "fullSync", v);
      resync = 1;
      goto release_response;
    }
    ops = cJSON_GetObjectItemCaseSensitive(data, "operationResponse");
    v = cJSON_GetObjectItemCaseSensitive(ops, "operations");
    for (op = v ? v->child : NULL; op && ok; op = op->next)
      ok = native_dispatch(c, "message", op);
    v = cJSON_GetObjectItemCaseSensitive(data, "partialFullSyncResponse");
    if (v && ok)
      ok = native_dispatch(c, "partialFullSync", v);
    if (ok) {
      patch = cJSON_CreateObject();
      v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ops, "globalEvents"),
                                           "lastRevision");
      if (v)
        cJSON_AddItemToObject(patch, "nativeGlobalRevision", cJSON_Duplicate(v, 1));
      v = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(ops, "individualEvents"), "lastRevision");
      if (v)
        cJSON_AddItemToObject(patch, "nativeIndividualRevision", cJSON_Duplicate(v, 1));
      if (patch->child && !enil_session_patch(c->session_path, patch))
        ok = 0;
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ops, "hasMoreOps")))
        delay = 0;
    }
    if (ok)
      failures = 0;
    else {
      failures++;
      delay = failures < 5 ? (1 << failures) : 30;
    }
release_response:
    cJSON_Delete(saved);
    cJSON_Delete(args);
    cJSON_Delete(root);
    cJSON_Delete(patch);
    free(body);
    enil_line_response_free(&response);
    /* The main-thread callback starts a full sync asynchronously. Do not
     * poll again or acknowledge any cursors from this response meanwhile. */
    if (resync) break;
    if (failures >= 5) {
      enil_health_set_failure(ENIL_ERR_LINE,
                              "Native event polling failed repeatedly; restart to retry.");
      break;
    }
    for (i = 0; i < delay && !c->stop && !c->reconnect; i++)
      sleep(1);
  }
}

