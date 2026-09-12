#ifndef ENIL_HTTP_H
#define ENIL_HTTP_H

#include <stddef.h>
#include <curl/curl.h>

typedef struct {
  char  *data;
  size_t size;
} ENILBuf;

/* Must be called at startup with the path to cacert.pem before any requests. */
void        enil_cainfo_set(const char *path);

/* Returns a curl handle with CURLOPT_CAINFO pre-set. Every libcurl handle in
 * this codebase MUST go through one of these two helpers so cross-cutting
 * options stay centralised. */
CURL *enil_curl_new_raw(void);
CURL *enil_curl_new(ENILBuf *buf);
void  enil_buf_free(ENILBuf *buf);

/* GET url and write body to dest_path. Returns 0 on success (HTTP 200). */
int   enil_curl_download_file(const char *url, const char *dest_path);

/* GET url and return body as a malloc'd string, or NULL on failure/non-200. */
char *enil_curl_get(const char *url);

#endif
