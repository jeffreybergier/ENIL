/* ============================================================================
 * libcurl wrapper — synchronous GET/POST, file downloads, and CA bundle setup.
 * ==========================================================================*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include "enil_http.h"
#include "enil_identity.h"
#include "enil_cocoa_log.h"

/* Finite requests must eventually yield even on a dead mobile socket.
 * Streaming callers explicitly opt out of the total/low-speed limits. */
#ifndef ENIL_HTTP_TIMEOUT_SECONDS
#define ENIL_HTTP_TIMEOUT_SECONDS 120L
#endif
#ifndef ENIL_HTTP_STALL_SECONDS
#define ENIL_HTTP_STALL_SECONDS 30L
#endif
static pthread_key_t s_cancel_key;
static pthread_once_t s_cancel_once = PTHREAD_ONCE_INIT;
static void cancel_key_create(void) { (void)pthread_key_create(&s_cancel_key, NULL); }
void enil_http_bind_cancel(const volatile int *cancel) {
  pthread_once(&s_cancel_once, cancel_key_create);
  pthread_setspecific(s_cancel_key, (void *)cancel);
}
const volatile int *enil_http_cancel_flag(void) {
  pthread_once(&s_cancel_once, cancel_key_create);
  return (const volatile int *)pthread_getspecific(s_cancel_key);
}
int enil_http_cancelled(void) {
  const volatile int *cancel = enil_http_cancel_flag();
  return cancel && *cancel;
}
static int download_cancel(void *ctx, curl_off_t a, curl_off_t b,
                           curl_off_t c, curl_off_t d) {
  (void)a; (void)b; (void)c; (void)d;
  return ctx && *(const volatile int *)ctx;
}
static void apply_download_cancel(CURL *curl) {
  const volatile int *cancel = enil_http_cancel_flag();
  if (!cancel) return;
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_cancel);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
}

static char *s_cainfo = NULL;
static pthread_once_t s_curl_once = PTHREAD_ONCE_INIT;

static void curl_init_once(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void ensure_curl_ready(void) {
  pthread_once(&s_curl_once, curl_init_once);
}

/* ============================================================================
 * Set the CA bundle path used for TLS verification. Pass NULL to clear.
 * ==========================================================================*/
void enil_cainfo_set(const char *path) {
  free(s_cainfo);
  s_cainfo = path ? strdup(path) : NULL;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  ENILBuf *buf = (ENILBuf *)userdata;
  size_t n = size * nmemb;
  char *tmp = realloc(buf->data, buf->size + n + 1);
  if (!tmp) {
    ENIL_LOG("Http.write_cb", "realloc failed (%zu bytes)",
             buf->size + n + 1);
    return 0;
  }
  buf->data = tmp;
  memcpy(buf->data + buf->size, ptr, n);
  buf->size += n;
  buf->data[buf->size] = '\0';
  return n;
}

/* ============================================================================
 * Create a bare curl handle with CURLOPT_CAINFO pre-set. Every libcurl handle
 * in this codebase MUST go through this helper or enil_curl_new(buf) below so
 * the CA bundle (and any future cross-cutting option) stays consistent.
 * ==========================================================================*/
CURL *enil_curl_new_raw(void) {
  CURL *curl;
  ensure_curl_ready();
  curl = curl_easy_init();
  if (!curl) return NULL;
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, ENIL_HTTP_TIMEOUT_SECONDS);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, ENIL_HTTP_STALL_SECONDS);
  if (s_cainfo && s_cainfo[0])
    curl_easy_setopt(curl, CURLOPT_CAINFO, s_cainfo);
  return curl;
}

/* ============================================================================
 * Create a curl handle pre-wired to append response bytes into buf.
 * Initialises libcurl on first call. Returns NULL on failure.
 * ==========================================================================*/
CURL *enil_curl_new(ENILBuf *buf) {
  CURL *curl = enil_curl_new_raw();
  if (!curl) return NULL;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
  return curl;
}

/* ============================================================================
 * Release the response buffer owned by an ENILBuf and zero its fields.
 * ==========================================================================*/
void enil_buf_free(ENILBuf *buf) {
  if (!buf) return;
  free(buf->data);
  buf->data = NULL;
  buf->size = 0;
}

/* ============================================================================
 * Synchronous HTTPS GET. Returns malloc'd response body on HTTP 200, else NULL.
 * ==========================================================================*/
char *enil_curl_get(const char *url) {
  ENILBuf buf = {NULL, 0};
  CURL *curl;
  long  status = 0;
  CURLcode rc;
  if (!url || enil_http_cancelled()) return NULL;
  curl = enil_curl_new(&buf);
  if (!curl) { ENIL_LOG("Http.get", "curl init failed: %s", url); return NULL; }
  if (enil_identity_current())
    curl_easy_setopt(curl, CURLOPT_USERAGENT, enil_identity_current()->user_agent);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  apply_download_cancel(curl);
  rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    ENIL_LOG("Http.get", "transport error %s: %s",
             curl_easy_strerror(rc), url);
    enil_buf_free(&buf);
    return NULL;
  }
  if (status != 200) {
    ENIL_LOG("Http.get", "HTTP %ld: %s", status, url);
    enil_buf_free(&buf);
    return NULL;
  }
  return buf.data;
}

/* Write beside the destination and publish only a complete, closed response.
 * A killed process may leave a temporary file, but never a poisoned cache hit. */
int enil_curl_download_file(const char *url, const char *dest_path) {
  CURL *curl = NULL;
  FILE *f = NULL;
  char *temporary;
  int fd, result = -1;
  long status = 0;
  CURLcode rc;

  if (!url || !dest_path || enil_http_cancelled()) return -1;
  temporary = malloc(strlen(dest_path) + sizeof(".download-XXXXXX"));
  if (!temporary) return -1;
  sprintf(temporary, "%s.download-XXXXXX", dest_path);
  fd = mkstemp(temporary);
  if (fd < 0) goto done;
  f = fdopen(fd, "wb");
  if (!f) { close(fd); goto done; }
  curl = enil_curl_new_raw();
  if (!curl) goto done;
  if (enil_identity_current())
    curl_easy_setopt(curl, CURLOPT_USERAGENT, enil_identity_current()->user_agent);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  apply_download_cancel(curl);

  rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (rc != CURLE_OK || status != 200) {
    ENIL_LOG("Http.download", "HTTP %ld / %s: %s", status,
             curl_easy_strerror(rc), url);
    goto done;
  }
  /* Buffered writes can fail at close even when curl reports success. */
  if (fclose(f) != 0) { f = NULL; goto done; }
  f = NULL;
  if (rename(temporary, dest_path) == 0) result = 0;

done:
  if (curl) curl_easy_cleanup(curl);
  if (f) fclose(f);
  if (result != 0) {
    unlink(temporary);
    ENIL_LOG("Http.download", "download not published: %s", dest_path);
  }
  free(temporary);
  return result;
}
