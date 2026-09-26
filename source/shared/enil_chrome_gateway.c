/* Chrome gateway signed JSON request and its transport-specific health rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "enil_chrome_gateway.h"
#include "enil_http.h"
#include "enil_worker.h"
#include "enil_health.h"
#include "enil_cocoa_log.h"

#define LOG(fn, msg) enil_log("Line." fn, "%s", msg)

static char *make_header(const char *name, const char *value) {
  size_t len = strlen(name) + 2 + strlen(value) + 1;
  char *h = malloc(len);
  if (h) snprintf(h, len, "%s: %s", name, value);
  return h;
}

/* libcurl transfer-info callback: returning non-zero aborts the transfer with
 * CURLE_ABORTED_BY_CALLBACK. libcurl invokes this roughly once per second even
 * while a long-poll sits idle waiting on the server, so a cancel flipped by the
 * UI (closed QR window) tears the connection down within ~1s. clientp is the
 * caller's cancel flag. */
static int cancel_xferinfo(void *clientp,
                           curl_off_t dltotal, curl_off_t dlnow,
                           curl_off_t ultotal, curl_off_t ulnow)
{
  const volatile int *cancel = (const volatile int *)clientp;
  (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
  return (cancel && *cancel) ? 1 : 0;
}

ENILLineResponse enil_chrome_gateway_post(
  const enil_identity_t *identity, const char *path, const char *body,
  const char *access_token, const char *const *extra_headers,
  int extra_header_count, long timeout_ms, const volatile int *cancel)
{
  ENILLineResponse result = {0, NULL};
  char url[512];
  char *hmac, *hmac_hdr, *access_hdr, *cookie_hdr = NULL;
  ENILBuf buf = {NULL, 0};
  CURL *curl;
  struct curl_slist *hdrs = NULL;
  CURLcode rc;
  int i;
  char application_header[192], version_header[64];

  hmac = enil_worker_sign(path, body, access_token);
  if (!hmac) { LOG("post", "sign failed"); return result; }

  snprintf(url, sizeof(url), "%s%s", ENIL_LINE_GATEWAY, path);
  curl = enil_curl_new(&buf);
  if (!curl) { free(hmac); LOG("post", "enil_curl_new failed"); return result; }

  hdrs = curl_slist_append(hdrs, "Accept: application/json, text/plain, */*");
  hdrs = enil_line_language_headers(hdrs);
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  snprintf(version_header, sizeof(version_header), "X-Line-Chrome-Version: %s",
           identity->gateway_version);
  snprintf(application_header, sizeof(application_header), "X-Line-Application: %s",
           identity->application);
  hdrs = curl_slist_append(hdrs, version_header);
  hdrs = curl_slist_append(hdrs, application_header);
  hdrs = curl_slist_append(hdrs, "Origin: " ENIL_LINE_ORIGIN);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, identity->user_agent);

  hmac_hdr   = make_header("X-Hmac", hmac);
  access_hdr = access_token[0] ? make_header("X-Line-Access", access_token) : NULL;
  free(hmac);

  if (access_token[0]) {
    size_t n = strlen("Cookie: lct=") + strlen(access_token) + 1;
    cookie_hdr = (char *)malloc(n);
    if (cookie_hdr) snprintf(cookie_hdr, n, "Cookie: lct=%s", access_token);
  }

  if (hmac_hdr)   hdrs = curl_slist_append(hdrs, hmac_hdr);
  if (access_hdr) hdrs = curl_slist_append(hdrs, access_hdr);
  if (cookie_hdr) hdrs = curl_slist_append(hdrs, cookie_hdr);

  for (i = 0; i < extra_header_count; i++) {
    if (extra_headers && extra_headers[i])
      hdrs = curl_slist_append(hdrs, extra_headers[i]);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  if (timeout_ms > 0)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  if (cancel) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
  }

  ENIL_LOG("Line.post", "POST %s (%lu bytes)",
           path, (unsigned long)strlen(body));

  rc = curl_easy_perform(curl);
  if (rc == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  free(hmac_hdr);
  free(access_hdr);
  free(cookie_hdr);

  /* User-initiated cancel (e.g. the QR login window was closed mid long-poll):
   * the transfer-info callback returned non-zero. This is not a fault, so it
   * must NOT trip the sticky LINE health gate — doing so would short-circuit
   * the next login attempt's first call. Return an empty result quietly. */
  if (rc == CURLE_ABORTED_BY_CALLBACK) {
    ENIL_LOG("Line.post", "aborted by cancel: %s", path);
    enil_buf_free(&buf);
    return result;
  }

  if (rc != CURLE_OK) {
    char msg[160];
    ENIL_LOG("Line.post", "transport error %s: %s",
             curl_easy_strerror(rc), path);
    snprintf(msg, sizeof(msg),
             "cannot reach LINE (%s). Restart the app or sign in again to retry.",
             curl_easy_strerror(rc));
    enil_health_set_failure(ENIL_ERR_LINE, msg);
    enil_buf_free(&buf);
    return result;
  }

  ENIL_LOG("Line.post", "-> HTTP %ld: %s", result.status, path);

  /* Categorise the HTTP outcome. Account/auth and gateway failures set the
   * sticky LINE gate — the account-lockout risk from repeated bad-auth pings
   * outweighs the UX cost of a manual reconnect. The one message-local
   * exception (an unavailable historical E2EE public key) is handled below.
   * The /api/auth/tokenRefresh call uses the same plumbing, so an expired
   * refresh-token shows up here as a 401 too. */
  if (result.status == 401 || result.status == 403) {
    enil_health_set_failure(ENIL_ERR_LINE,
      "rejected access token. Sign in again via QR to recover.");
    free(buf.data);
    return result;
  }
  if (result.status >= 500 && result.status < 600) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "server error (HTTP %ld). Restart the app or sign in again to retry.",
             result.status);
    enil_health_set_failure(ENIL_ERR_LINE, msg);
    free(buf.data);
    return result;
  }
  /* A public-key lookup can legitimately fail for one historical message
   * after its peer has rotated that key out of LINE's lookup service.  This is
   * message-local, not evidence that the account or gateway is unhealthy.
   * Return the 400 to the decrypt caller (which leaves that row failed and
   * advances to the next message) without poisoning later LINE requests. */
  if (result.status == 400 &&
      strcmp(path,
             "/api/talk/thrift/Talk/TalkService/getE2EEPublicKey") == 0) {
    ENIL_LOG("Line.post",
             "public key unavailable (HTTP 400); continuing sync");
    free(buf.data);
    return result;
  }
  if (result.status != 0 && (result.status < 200 || result.status >= 300)) {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "unexpected HTTP %ld. Restart the app or sign in again to retry.",
             result.status);
    enil_health_set_failure(ENIL_ERR_LINE, msg);
    free(buf.data);
    return result;
  }
  result.body = buf.data;
  return result;
}

