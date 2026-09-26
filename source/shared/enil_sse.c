/* ============================================================================
 * Event client lifecycle and Chrome SSE stream. Native operation polling
 * lives in enil_native_poll.c and uses the same event callback and cursor.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include "enil_sse_internal.h"
#include "enil_line.h"
#include "enil_http.h"
#include "enil_session.h"
#include "enil_db.h"
#include "enil_api_json.h"
#include "enil_cocoa_log.h"
#include "enil_health.h"
#include "cJSON.h"

#define SSE_PATH "/api/operation/receive"

/* Idle watchdog. The gateway sends a `ping` event every ~20s, so any healthy
 * stream produces bytes well inside this window. If no byte arrives for this
 * long the socket is a black hole (sleep/wake, NAT rebind, network drop) —
 * curl's recv() would otherwise block until the OS TCP stack gives up, which
 * can be minutes. progress_cb compares against last_activity and forces a
 * reconnect. Kept comfortably above the 20s ping interval to avoid tearing
 * down a connection that is merely between pings. */
#define SSE_IDLE_TIMEOUT 45

/* Match the native Thrift reply bound. Grow on demand so ordinary pings do
 * not reserve megabytes, and never silently truncate a message for parsing. */
#define SSE_MAX_EVENT_SIZE (8U * 1024U * 1024U)
#define SSE_MAX_LINE_SIZE (SSE_MAX_EVENT_SIZE + 7U) /* data: space, plus CR in CRLF */

#define LOG(fn, msg) enil_log("Sse." fn, "%s", msg)

/* SSE parse state — lives on the stack of sse_thread */
typedef struct {
  ENILSSEClient *client;
  char          *line_buf;
  size_t         line_len, line_cap;
  char           event_type[64];
  char          *data_buf;
  size_t         data_len, data_cap;
  int            has_data;
} ENILSSEState;

/* ------------------------------------------------------------------ */

static int append_sse(ENILSSEState *s, char **buf, size_t *len, size_t *cap,
                       const char *text, size_t count, size_t limit)
{
  size_t needed, grown;
  char *next;
  const char *error = "SSE event exceeds the supported size.";
  if (count > limit - *len) goto failed;
  needed = *len + count + 1;
  if (needed > *cap) {
    grown = *cap ? *cap : 256;
    while (grown < needed) grown *= 2;
    if (grown > limit + 1) grown = limit + 1;
    next = (char *)realloc(*buf, grown);
    if (!next) {
      error = "Cannot allocate SSE event buffer.";
      goto failed;
    }
    *buf = next;
    *cap = grown;
  }
  memcpy(*buf + *len, text, count);
  *len += count;
  (*buf)[*len] = '\0';
  return 1;
failed:
  s->client->delivery_failed = 1;
  /* Keep the cursor, but stop reconnecting to the same oversized event. */
  enil_health_set_failure(ENIL_ERR_LINE, error);
  return 0;
}

static void free_sse_state(ENILSSEState *s)
{
  free(s->line_buf);
  free(s->data_buf);
}

long long enil_sse_event_revision(const char *type, const char *data)
{
  cJSON *root, *item;
  long long rev;
  if (!type || !data) return 0;
  root = cJSON_Parse(data);
  if (!root) return 0;
  item = strcmp(type, "fullSync") == 0
    ? cJSON_GetObjectItem(root, "nextRevision")
    : cJSON_GetObjectItem(root, "revision");
  rev = enil_json_coerce_int64(item);
  cJSON_Delete(root);
  return rev > 0 ? rev : 0;
}

int enil_sse_save_local_rev(ENILSSEClient *c, long long rev)
{
  if (!c) return 0;
  if (rev <= c->local_rev) return 1;
  if (c->db && enil_db_set_local_rev(c->db, rev) != SQLITE_OK) {
    LOG("localRev", "could not save event revision; retaining previous cursor");
    return 0;
  }
  ENIL_LOG("Sse.localRev",
           "updating localRev %lld -> %lld", c->local_rev, rev);
  c->local_rev = rev;
  return 1;
}

static void dispatch_event(ENILSSEState *s)
{
  ENILSSEEvent  ev;
  ENILSSEClient *c = s->client;
  long long      rev;

  if (!s->event_type[0] && !s->has_data) return;

  ev.type = s->event_type[0] ? s->event_type : "message";
  ev.data = s->data_buf ? s->data_buf : "";

  ENIL_LOG("Sse.dispatchEvent", "%s (%lu bytes)", ev.type, (unsigned long)s->data_len);

  if ((c->fn && !c->fn(&ev, c->ctx)) || enil_health_any_failed() || c->stop) {
    c->delivery_failed = 1;
    return;
  }

  if (strcmp(ev.type, "fullSync") == 0) {
    /* The account schedules a data sync. Keep the old cursor until that
     * succeeds, including when the app exits or the sync fails. */
    c->stop = 1;
  } else {
    rev = enil_sse_event_revision(ev.type, ev.data);
    if (!enil_sse_save_local_rev(c, rev)) c->delivery_failed = 1;
  }

  if (strcmp(ev.type, "reconnect") == 0) {
    c->reconnect = 1;
  }

  s->event_type[0] = '\0';
  if (s->data_buf) s->data_buf[0] = '\0';
  s->data_len = 0;
  s->has_data = 0;
}

static void process_line(ENILSSEState *s)
{
  const char *val;

  if (s->line_len == 0) {
    dispatch_event(s);
    return;
  }

  if (strncmp(s->line_buf, "event:", 6) == 0) {
    val = s->line_buf + 6;
    if (*val == ' ') val++;
    if (strlen(val) >= sizeof(s->event_type)) {
      s->client->delivery_failed = 1;
      enil_health_set_failure(ENIL_ERR_LINE, "SSE event type exceeds the supported size.");
      return;
    }
    strcpy(s->event_type, val);
  } else if (strncmp(s->line_buf, "data:", 5) == 0) {
    val = s->line_buf + 5;
    if (*val == ' ') val++;
    /* SSE joins consecutive data fields with a newline, including empty
     * fields. Preserve whitespace within JSON instead of concatenating it. */
    if (s->has_data && !append_sse(s, &s->data_buf, &s->data_len, &s->data_cap,
                                   "\n", 1, SSE_MAX_EVENT_SIZE)) return;
    if (!append_sse(s, &s->data_buf, &s->data_len, &s->data_cap,
                    val, strlen(val), SSE_MAX_EVENT_SIZE)) return;
    s->has_data = 1;
  }
  /* ignore id: and comment lines */
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  ENILSSEState *s = (ENILSSEState *)userdata;
  size_t total = size * nmemb;
  size_t i;
  char c;

  /* Any byte — including a keep-alive ping — proves the socket is live. The
   * idle watchdog in progress_cb reads this stamp. */
  if (total) s->client->last_activity = time(NULL);

  for (i = 0; i < total; i++) {
    c = ptr[i];
    if (c == '\n') {
      if (s->line_len > 0 && s->line_buf[s->line_len - 1] == '\r')
        s->line_len--;
      if (s->line_buf) s->line_buf[s->line_len] = '\0';
      process_line(s);
      s->line_len = 0;
      if (s->client->stop || s->client->reconnect || s->client->delivery_failed) return 0;
    } else if (!append_sse(s, &s->line_buf, &s->line_len, &s->line_cap,
                            &c, 1, SSE_MAX_LINE_SIZE)) {
      return 0;
    }
  }

  /* Returning 0 aborts the transfer — used for reconnect and stop */
  if (s->client->reconnect || s->client->stop) return 0;
  return total;
}

/* libcurl progress callback — secondary abort path for idle connections */
static int progress_cb(void *userdata, double dltotal, double dlnow,
                       double ultotal, double ulnow)
{
  ENILSSEClient *c = (ENILSSEClient *)userdata;
  (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;

  /* Stop / explicit kick: abort now. */
  if (c->stop || c->reconnect) return 1;

  /* Idle watchdog. libcurl invokes this callback ~once per second even while
   * blocked waiting for data, so a dead socket is caught within
   * SSE_IDLE_TIMEOUT seconds rather than hanging on recv() indefinitely.
   * Setting reconnect makes the sse_thread loop re-establish immediately with
   * the current localRev (no 2s disconnect sleep). */
  if (c->last_activity != 0 &&
      (long)(time(NULL) - c->last_activity) > SSE_IDLE_TIMEOUT) {
    ENIL_LOG("Sse.progress",
             "idle %lds (>%ds) — forcing reconnect",
             (long)(time(NULL) - c->last_activity), SSE_IDLE_TIMEOUT);
    c->reconnect = 1;
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */

static void *sse_thread(void *arg)
{
  ENILSSEClient     *c = (ENILSSEClient *)arg;
  char               url[2048];
  char              *access_hdr = NULL;
  char              *cookie_hdr = NULL;
  struct curl_slist *hdrs       = NULL;
  CURL              *curl       = NULL;
  ENILSSEState       state;
  size_t             n;
  CURLcode           rc;
  enil_identity_t identity;
  char application_header[192], version_header[64];

  /* Bind this account's health so the reconnect gate below and any LINE work
   * the event callback drives are scoped to this account, not the process. */
  enil_health_bind(c->health);
  if (!enil_session_bind_identity(c->session_path)) {
    enil_health_set_failure(ENIL_ERR_LINE, "Cannot load client identity");
    return NULL;
  }
  identity = *enil_identity_current();
  if (!strcmp(identity.transport, "native-thrift")) { enil_native_poll(c); return NULL; }
  snprintf(application_header, sizeof(application_header), "X-Line-Application: %s",
           identity.application);
  snprintf(version_header, sizeof(version_header), "X-Line-Chrome-Version: %s",
           identity.gateway_version);

  while (!c->stop) {
    /* Top-of-loop health gate. If a previous event handler (or any other
     * thread) put either subsystem into the failed state, do NOT reconnect.
     * Per user policy SSE stays closed until the user explicitly recovers
     * (re-save worker credentials, restart app, etc.). This is the second
     * line of defence — sse_event_cb in ENILAccount.m also requests a stop
     * via performSelectorOnMainThread when a failure trips mid-event, but
     * a synchronous check here means we never start a fresh long-poll into
     * a known-broken backend. */
    if (enil_health_any_failed()) {
      ENIL_LOG("Sse.thread",
               "health failed before reconnect — staying stopped");
      break;
    }

    memset(&state, 0, sizeof(state));
    state.client = c;

    /* SSE GET is sent unsigned — matrix-line does the same and the LINE
       gateway accepts it. Skipping the worker /sign round-trip removes a
       Cloudflare hop (and its failure mode) from every ~140s reconnect. */
    curl = enil_curl_new_raw();
    if (!curl) {
      LOG("thread", "enil_curl_new_raw failed; retrying in 5s");
      sleep(5);
      continue;
    }

    {
      char  *partials = c->session_path
        ? enil_session_get_partial_full_syncs_json(c->session_path)
        : strdup("{}");
      char  *encoded  = curl_easy_escape(curl,
                                         partials ? partials : "{}", 0);
      snprintf(url, sizeof(url),
               "%s%s?version=%s&localRev=%lld&lastPartialFullSyncs=%s",
               ENIL_LINE_GATEWAY, SSE_PATH, identity.gateway_version, c->local_rev,
               encoded ? encoded : "%7B%7D");
      if (encoded) curl_free(encoded);
      free(partials);
    }

    n = strlen("X-Line-Access: ") + strlen(c->access_token) + 1;
    access_hdr = (char *)malloc(n);
    if (access_hdr) snprintf(access_hdr, n, "X-Line-Access: %s",
                             c->access_token);

    n = strlen("Cookie: lct=") + strlen(c->access_token) + 1;
    cookie_hdr = (char *)malloc(n);
    if (cookie_hdr) snprintf(cookie_hdr, n, "Cookie: lct=%s",
                             c->access_token);

    hdrs = curl_slist_append(NULL, "Accept: text/event-stream");
    hdrs = curl_slist_append(hdrs,  "Cache-Control: no-cache");
    hdrs = curl_slist_append(hdrs,  "Pragma: no-cache");
    hdrs = curl_slist_append(hdrs, version_header);
    hdrs = curl_slist_append(hdrs, application_header);
    hdrs = enil_line_language_headers(hdrs);
    hdrs = curl_slist_append(hdrs,  "Origin: " ENIL_LINE_ORIGIN);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, identity.user_agent);
    if (access_hdr) hdrs = curl_slist_append(hdrs, access_hdr);
    if (cookie_hdr) hdrs = curl_slist_append(hdrs, cookie_hdr);

    curl_easy_setopt(curl, CURLOPT_URL,              url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,       hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &state);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA,     c);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    /* SSE is long-lived. Do not set total transfer timeout. */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   20L);
    /* Defense in depth alongside the idle watchdog: let the kernel probe a
     * silent peer so a dead connection (sleep/wake, app pause) is noticed at
     * the TCP layer even if the progress callback path is starved. */
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,    1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,     30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL,    15L);

    /* Seed the idle watchdog so a fresh connection isn't judged stale before
     * its first byte (the connect itself is bounded by CONNECTTIMEOUT). */
    c->last_activity = time(NULL);

    ENIL_LOG("Sse.thread", "connecting localRev=%lld", c->local_rev);

    c->delivery_failed = 0;
    rc = curl_easy_perform(curl);
    free_sse_state(&state);

    {
      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      /* Unsigned-GET probe: a 401/403 means the gateway rejected the
         request for lack of X-Hmac. curl_easy_perform still returns
         CURLE_OK in that case, so the generic disconnect log below would
         hide it. Log it loudly — if this fires every reconnect, the
         unsigned experiment failed and signing must be restored (3 hops
         per reconnect is worse than the original 2). */
      if (http_code == 401 || http_code == 403) {
        ENIL_LOG("Sse.thread",
                 "REJECTED http=%ld with no X-Hmac — "
                 "unsigned SSE GET not accepted; restore signing",
                 http_code);
      } else if (http_code >= 400) {
        ENIL_LOG("Sse.thread", "http=%ld (non-auth error)", http_code);
      }
    }

    curl_slist_free_all(hdrs); hdrs = NULL;
    curl_easy_cleanup(curl);   curl = NULL;
    free(access_hdr); access_hdr = NULL;
    free(cookie_hdr); cookie_hdr = NULL;

    if (c->stop) break;

    if (c->reconnect) {
      c->reconnect = 0;
      /* reconnect immediately with updated localRev */
      continue;
    }

    ENIL_LOG("Sse.thread",
             "disconnected (%s); reconnecting in 2s",
             curl_easy_strerror(rc));
    sleep(2);
  }

  ENIL_LOG("Sse.thread", "stopped");
  return NULL;
}

/* ------------------------------------------------------------------ */

/* ============================================================================
 * Allocate an SSE client. access_token is required; db is the persistence
 * target for localRev advances; session_path feeds the lastPartialFullSyncs
 * query param; local_rev seeds the initial query param.
 * ==========================================================================*/
ENILSSEClient *enil_sse_create(const char *access_token, long long local_rev,
                               const char *session_path, sqlite3 *db,
                               enil_health_t *health)
{
  ENILSSEClient *c;
  if (!access_token) return NULL;
  c = (ENILSSEClient *)calloc(1, sizeof(ENILSSEClient));
  if (!c) return NULL;
  c->access_token = strdup(access_token);
  c->session_path = session_path ? strdup(session_path) : NULL;
  c->db           = db;
  c->health       = health;
  c->local_rev    = local_rev;
  return c;
}

/* ============================================================================
 * Spawn the background SSE thread. Events are delivered to fn(ctx, ...).
 * Returns nonzero on success.
 * ==========================================================================*/
int enil_sse_start(ENILSSEClient *c, ENILSSEEventFn fn, void *ctx)
{
  int rc;
  if (!c || !fn) return 0;
  c->fn   = fn;
  c->ctx  = ctx;
  c->stop = 0;
  rc = pthread_create(&c->thread, NULL, sse_thread, c);
  c->thread_started = (rc == 0);
  if (rc != 0)
    ENIL_LOG("Sse.start", "pthread_create failed: %d", rc);
  return rc == 0;
}

/* ============================================================================
 * Force the running stream to drop and re-establish immediately with the
 * current localRev. Non-blocking: it only sets the reconnect flag, which the
 * progress callback notices within ~1s (it fires even while recv() is blocked)
 * and aborts the transfer, after which sse_thread reconnects. Use this for
 * instant recovery on system wake instead of stopSSE+startSSE, which would
 * pthread_join a possibly-wedged worker thread on the caller's thread.
 * Safe to call on NULL or a not-yet-started client. Safe from any thread —
 * reconnect is a volatile int, written/read without a lock like stop. */
void enil_sse_kick(ENILSSEClient *c)
{
  if (!c) return;
  c->reconnect = 1;
  c->interrupt = 1;
}

/* ============================================================================
 * Stop the SSE thread, join it, and release the client. Safe to call on NULL.
 * ==========================================================================*/
void enil_sse_free(ENILSSEClient *c)
{
  if (!c) return;
  c->stop = 1;
  c->interrupt = 1;
  if (c->thread_started) pthread_join(c->thread, NULL);
  free(c->access_token);
  free(c->session_path);
  free(c);
}
