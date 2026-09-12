/* ============================================================================
 * libcurl wrapper — synchronous GET/POST, file downloads, and CA bundle setup.
 * ==========================================================================*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "enil_http.h"
#include "enil_cocoa_log.h"

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
  if (!url) return NULL;
  curl = enil_curl_new(&buf);
  if (!curl) { ENIL_LOG("Http.get", "curl init failed: %s", url); return NULL; }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
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

/* ============================================================================
 * Stream a URL to dest_path. Removes the file and returns -1 if not HTTP 200.
 * ==========================================================================*/
int enil_curl_download_file(const char *url, const char *dest_path) {
  CURL *curl;
  FILE *f;
  long  status = 0;
  CURLcode rc;

  if (!url || !dest_path) return -1;
  f = fopen(dest_path, "wb");
  if (!f) {
    ENIL_LOG("Http.download", "fopen failed: %s", dest_path);
    return -1;
  }

  curl = enil_curl_new_raw();
  if (!curl) {
    ENIL_LOG("Http.download", "curl init failed: %s", url);
    fclose(f);
    return -1;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL); /* default fwrite */
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  fclose(f);

  if (rc != CURLE_OK) {
    ENIL_LOG("Http.download", "transport error %s: %s",
             curl_easy_strerror(rc), url);
    remove(dest_path);
    return -1;
  }
  if (status != 200) {
    ENIL_LOG("Http.download", "HTTP %ld: %s", status, url);
    remove(dest_path);
    return -1;
  }
  return 0;
}
