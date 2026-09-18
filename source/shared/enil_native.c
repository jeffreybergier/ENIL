/* Windows native transport. The Worker performs crypto; only ENIL calls LINE. */
#include "enil_native.h"
#include "enil_b64.h"
#include "enil_cocoa_log.h"
#include "enil_health.h"
#include "enil_identity.h"
#include "enil_session.h"
#include "enil_thrift.h"
#include "enil_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *json_string(cJSON *o, const char *key) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}
static int cancel_request(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return p && *(const volatile int *)p;
}
static void append_header(struct curl_slist **hs, const char *name, const char *value) {
  size_t n;
  char *h;
  if (!value)
    return;
  n = strlen(name) + strlen(value) + 3;
  h = malloc(n);
  if (!h)
    return;
  snprintf(h, n, "%s: %s", name, value);
  *hs = curl_slist_append(*hs, h);
  free(h);
}

/* A normal long poll may expire while waiting for its first response byte.
 * DNS/TCP/TLS timeouts, incomplete uploads, and truncated responses are real
 * failures and must reach the polling loop's backoff/error handling. */
static int idle_poll_timeout(CURL *curl, size_t request_size, size_t response_size,
                             long status) {
  double ready_at = 0;
  curl_off_t uploaded = 0;
  return response_size == 0 && (status == 0 || status == 200) &&
         curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME, &ready_at) == CURLE_OK &&
         ready_at > 0 &&
         curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded) == CURLE_OK &&
         uploaded == (curl_off_t)request_size;
}

static int current_login(void) {
  char *token = NULL;
  int result = enil_session_bound_access_token(&token);
  free(token);
  return result >= 0;
}

ENILLineResponse enil_native_post(const char *path, const char *body, const char *token,
                                  long timeout_ms, const volatile int *cancel) {
  ENILLineResponse out = {0, NULL};
  ENILBuf request = {NULL, 0}, response = {NULL, 0};
  const enil_identity_t *identity = enil_identity_current();
  const char *method, *endpoint, *url;
  cJSON *args = NULL, *payload = NULL, *encoded = NULL, *decoded = NULL, *reply = NULL,
        *data = NULL, *envelope = NULL;
  CURL *curl = NULL;
  struct curl_slist *hs = NULL;
  CURLcode rc;
  char *base64 = NULL, *bound_token = NULL;
  unsigned char *wire = NULL;
  int len, encrypted, contacts = 0;
  if (!identity || strcmp(identity->transport, "native-thrift") || !path || !body || !token)
    return out;
  if (cancel && *cancel)
    return out;
  if (enil_session_bound_access_token(&bound_token) < 0) {
    ENIL_LOG("Native.post", "bound login is no longer available");
    goto done;
  }
  if (bound_token)
    token = bound_token;
  method = strrchr(path, '/');
  if (!method)
    goto done;
  method++;
  args = cJSON_Parse(body);
  if (!args)
    goto done;
  endpoint = "/S4";
  encrypted = 1;
  if (!strcmp(path, "/api/auth/tokenRefresh")) {
    cJSON *wrapper = cJSON_CreateArray();
    cJSON_AddItemToArray(wrapper, args);
    args = wrapper;
    method = "refresh";
    endpoint = "/EXT/auth/tokenrefresh/v1";
    encrypted = 0;
  } else if (!strcmp(path, "/native/sync"))
    endpoint = "/SYNC4";
  else if (!strncmp(path, "/api/shop/thrift/ShopService/ShopService/",
                    strlen("/api/shop/thrift/ShopService/ShopService/"))) {
    endpoint = "/TSHOP4";
    encrypted = 0;
  } else if (strncmp(path, "/api/talk/thrift/Talk/TalkService/",
                     strlen("/api/talk/thrift/Talk/TalkService/")))
    goto done;
  if (!strcmp(method, "getContactsV2")) {
    cJSON *mids = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(args, 0), "targetUserMids");
    cJSON *wrapper = cJSON_CreateArray();
    if (!cJSON_IsArray(mids)) {
      cJSON_Delete(wrapper);
      goto done;
    }
    cJSON_AddItemToArray(wrapper, cJSON_Duplicate(mids, 1));
    cJSON_Delete(args);
    args = wrapper;
    method = "getContacts";
    contacts = 1;
  }
  if (!strcmp(method, "sendChatRemoved")) {
    /* The gateway's fourth argument is lastReadMessageTime. Native Talk's
     * fourth field is an optional byte sessionId, not a timestamp. Omit it,
     * as for sendChatChecked; preserve seq, chatMid, and lastMessageId. */
    if (!cJSON_IsArray(args) || cJSON_GetArraySize(args) != 4)
      goto done;
    cJSON_DeleteItemFromArray(args, 3);
  }
  if (!enil_thrift_encode(method, args, &request)) {
    ENIL_LOG("Native.post", "unsupported or invalid request: %s", method);
    goto done;
  }
  if (encrypted) {
    base64 = enil_b64_encode((unsigned char *)request.data, request.size);
    payload = cJSON_CreateObject();
    if (!base64 || !payload)
      goto done;
    cJSON_AddStringToObject(payload, "path", endpoint);
    cJSON_AddStringToObject(payload, "body", base64);
    cJSON_AddStringToObject(payload, "accessToken", token);
    encoded = enil_worker_decrypt_ex("/transport/legy/encode", payload, cancel);
    cJSON_Delete(payload);
    payload = NULL;
    free(base64);
    base64 = NULL;
    if ((cancel && *cancel) || !json_string(encoded, "body") || !json_string(encoded, "key") || !json_string(encoded, "xLcs"))
      goto done;
    len = enil_b64_decode_alloc(json_string(encoded, "body"), &wire);
    if (len < 0)
      goto done;
    enil_buf_free(&request);
    request.data = (char *)wire;
    request.size = (size_t)len;
    wire = NULL;
    url = "https://gf.line.naver.jp/enc";
    append_header(&hs, "X-LE", "7");
    append_header(&hs, "X-LAP", "5");
    append_header(&hs, "X-LPV", "1");
    append_header(&hs, "X-LCS", json_string(encoded, "xLcs"));
    append_header(&hs, "X-LHM", "POST");
  } else {
    url = !strcmp(method, "refresh") ? "https://legy.line-apps.com/EXT/auth/tokenrefresh/v1"
                                     : "https://legy.line-apps.com/TSHOP4";
    if (*token)
      append_header(&hs, "X-Line-Access", token);
  }
  append_header(&hs, "Content-Type", "application/x-thrift");
  append_header(&hs, "Accept", "application/x-thrift");
  append_header(&hs, "X-Line-Application", identity->application);
  append_header(&hs, "X-LAL", "en_US");
  curl = enil_curl_new(&response);
  if (!curl)
    goto done;
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, identity->user_agent);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.data);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request.size);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 60000L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (cancel) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_request);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
  }
  /* LEGY encoding may have overlapped reauthentication. Do not send the
   * prepared request if its login was replaced while the Worker ran. */
  if (!current_login()) goto done;
  ENIL_LOG("Native.post", "POST %s %s (%lu bytes)", url, method, (unsigned long)request.size);
  rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
  ENIL_LOG("Native.post", "%s %s HTTP %ld curl=%d", url, method, out.status, (int)rc);
  if (rc != CURLE_OK) {
    if (rc == CURLE_OPERATION_TIMEDOUT && !strcmp(method, "sync") &&
        idle_poll_timeout(curl, request.size, response.size, out.status))
      out.status = 204;
    if (rc != CURLE_ABORTED_BY_CALLBACK)
      ENIL_LOG("Native.post", "%s: transport %d", method, (int)rc);
    goto done;
  }
  if (out.status != 200)
    goto done;
  if (encrypted) {
    payload = cJSON_CreateObject();
    base64 = enil_b64_encode((unsigned char *)response.data, response.size);
    if (!base64 || !payload)
      goto done;
    cJSON_AddStringToObject(payload, "key", json_string(encoded, "key"));
    cJSON_AddStringToObject(payload, "body", base64);
    decoded = enil_worker_decrypt_ex("/transport/legy/decode", payload, cancel);
    if (!json_string(decoded, "body"))
      goto done;
    data = cJSON_GetObjectItemCaseSensitive(decoded, "status");
    ENIL_LOG("Native.post", "%s %s LEGY status=%d", endpoint, method,
             cJSON_IsNumber(data) ? data->valueint : 200);
    if (cJSON_IsNumber(data) && data->valueint != 200) {
      out.status = data->valueint;
      goto done;
    }
    if (!current_login()) goto done;
    {
      const char *next =
          json_string(cJSON_GetObjectItemCaseSensitive(decoded, "headers"), "x-line-next-access");
      if (next && !enil_session_accept_next_access(token, next)) {
        enil_health_set_failure(ENIL_ERR_LINE, "Could not save updated native access token");
        goto done;
      }
    }
    len = enil_b64_decode_alloc(json_string(decoded, "body"), &wire);
    if (len < 0)
      goto done;
    enil_buf_free(&response);
    response.data = (char *)wire;
    response.size = (size_t)len;
    wire = NULL;
  }
  /* Refresh retains its response for the generation-checked recovery journal.
   * Other replies from an obsolete login must not reach account consumers. */
  if (strcmp(method, "refresh") && !current_login()) goto done;
  reply = enil_thrift_decode(method, response.data, response.size);
  if (!reply) {
    ENIL_LOG("Native.post", "invalid Thrift reply: %s", method);
    goto done;
  }
  data = cJSON_GetObjectItemCaseSensitive(reply, "e");
  if (data) {
    cJSON *code = cJSON_GetObjectItemCaseSensitive(data, "code");
    out.status = 400;
    out.body = cJSON_PrintUnformatted(data);
    if (cJSON_IsNumber(code) &&
        (code->valueint == 1 || code->valueint == 14 || code->valueint == 117))
      enil_health_set_failure(ENIL_ERR_LINE,
                              "LINE rejected this session. Reauthentication is required.");
    ENIL_LOG("Native.post", "%s: LINE exception %d", method, code ? code->valueint : 0);
    goto done;
  }
  data = cJSON_DetachItemFromObjectCaseSensitive(reply, "success");
  if (!data) {
    if (strcmp(method, "sendChatChecked") && strcmp(method, "sendChatRemoved"))
      goto done;
    data = cJSON_CreateObject();
  }
  if (contacts) {
    cJSON *item, *wrapped = cJSON_CreateObject(), *map = cJSON_CreateObject();
    for (item = data->child; item; item = item->next) {
      const char *mid = json_string(item, "mid");
      cJSON *entry;
      if (!mid)
        continue;
      entry = cJSON_CreateObject();
      cJSON_AddItemToObject(entry, "contact", cJSON_Duplicate(item, 1));
      cJSON_AddItemToObject(map, mid, entry);
    }
    cJSON_AddItemToObject(wrapped, "contacts", map);
    cJSON_Delete(data);
    data = wrapped;
  }
  envelope = cJSON_CreateObject();
  cJSON_AddStringToObject(envelope, "message", "OK");
  cJSON_AddItemToObject(envelope, "data", data);
  out.body = cJSON_PrintUnformatted(envelope);
done:
  if (out.status == 401 || out.status == 403)
    enil_health_set_failure(ENIL_ERR_LINE,
                            "LINE rejected this session. Reauthentication is required.");
  cJSON_Delete(args);
  cJSON_Delete(payload);
  cJSON_Delete(encoded);
  cJSON_Delete(decoded);
  cJSON_Delete(reply);
  cJSON_Delete(envelope);
  free(bound_token);
  free(base64);
  free(wire);
  enil_buf_free(&request);
  enil_buf_free(&response);
  curl_slist_free_all(hs);
  if (curl)
    curl_easy_cleanup(curl);
  return out;
}
