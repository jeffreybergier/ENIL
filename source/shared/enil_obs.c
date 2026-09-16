/* ============================================================================
 * OBS media downloader — fetches LINE attachments (images/video/audio) and
 * writes them under the per-account media/ directory.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/time.h>
#include <unistd.h>
#include <openssl/rand.h>
#include "cJSON.h"
#include "enil_b64.h"
#include "enil_http.h"
#include "enil_session.h"
#include "enil_crypto.h"
#include "enil_obs.h"
#include "enil_cocoa_log.h"

#define OBS_BASE      "https://obs.line-apps.com"
/* Thin wrapper: routes fixed-string call sites through NSLog with a
 * "Obs.<fn>" tag. Formatted sites should call ENIL_LOG directly. */
#define LOG(fn, msg)  enil_log("Obs." fn, "%s", msg)

static int is_unreserved(char c) {
  unsigned char ch = (unsigned char)c;
  if (isalnum(ch)) return 1;
  return c == '-' || c == '_' || c == '.' || c == '~';
}

static char *url_encode(const char *value) {
  static const char kHex[] = "0123456789ABCDEF";
  size_t len = value ? strlen(value) : 0;
  char *out = (char *)malloc(len * 3 + 1);
  size_t i, j = 0;

  if (!out) return NULL;
  for (i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)value[i];
    if (is_unreserved((char)ch)) {
      out[j++] = (char)ch;
      continue;
    }
    out[j++] = '%';
    out[j++] = kHex[(ch >> 4) & 0xF];
    out[j++] = kHex[ch & 0xF];
  }
  out[j] = '\0';
  return out;
}

static void log_text_preview(const char *label, const char *data, size_t len) {
  char preview[161];
  size_t i, n;

  if (!data || len == 0) {
    ENIL_LOG("Obs.download", "%s=<empty>", label);
    return;
  }

  n = len < sizeof(preview) - 1 ? len : sizeof(preview) - 1;
  for (i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)data[i];
    preview[i] = isprint(ch) ? (char)ch : '.';
  }
  preview[n] = '\0';
  ENIL_LOG("Obs.download", "%s=\"%s\" bytes=%lu",
           label, preview, (unsigned long)len);
}

static int build_obs_url(char *url, size_t url_len,
                         const char *sid, const char *oid,
                         const char *message_id, const char *obs_pop,
                         const char *suffix)
{
  char *encoded_pop = NULL;
  const char *sid_value = sid && sid[0] ? sid : "m";
  const char *oid_value = oid && oid[0] ? oid : message_id;
  const char *suffix_value = suffix ? suffix : "";
  int written;

  if (!url || !message_id) return -1;
  if (obs_pop && obs_pop[0]) {
    encoded_pop = url_encode(obs_pop);
    if (!encoded_pop) return -1;
    written = snprintf(url, url_len, "%s/r/talk/%s/%s%s?p=%s",
                       OBS_BASE, sid_value, oid_value, suffix_value,
                       encoded_pop);
    free(encoded_pop);
  } else {
    written = snprintf(url, url_len, "%s/r/talk/%s/%s%s",
                       OBS_BASE, sid_value, oid_value, suffix_value);
  }

  if (written < 0 || (size_t)written >= url_len) return -1;
  return 0;
}

/* OBS returns HTTP 202 (no body, no error) while the server is still
 * transcoding/packaging the media. It is a retryable "come back later"
 * signal, distinct from a real failure. Back off on this fixed schedule
 * (seconds) and retry; anything other than 200/202 is a hard failure and
 * is not retried. Runs on the serial sync pthread, so the worst case adds
 * 5+10+30+60 = 105s to that one queued download before giving up. */
static int obs_get(const char *label, const char *url,
                   const char *access_hdr, const char *meta_hdr,
                   const char *app_hdr, ENILBuf *buf,
                   char *content_type_buf, size_t content_type_len)
{
  static const int kBackoff[] = { 5, 10, 30, 60 };
  const int kRetries = (int)(sizeof(kBackoff) / sizeof(kBackoff[0]));
  int attempt;
  const enil_identity_t *identity = enil_identity_current();

  if (!identity) return -1;
  if (!label || !url || !access_hdr || !app_hdr || !buf) return -1;
  if (content_type_buf && content_type_len > 0) content_type_buf[0] = '\0';

  for (attempt = 0; attempt <= kRetries; attempt++) {
    CURL *curl;
    struct curl_slist *hdrs = NULL;
    CURLcode rc;
    long http_status = 0;
    char *content_type = NULL;

    if (attempt > 0) {
      int delay = kBackoff[attempt - 1];
      char tag[64];
      snprintf(tag, sizeof(tag), "Obs.%s", label);
      ENIL_LOG(tag,
               "HTTP 202 (transcoding) — retry %d/%d in %ds url=%s",
               attempt, kRetries, delay, url);
      enil_buf_free(buf);          /* discard the empty 202 body */
      buf->data = NULL; buf->size = 0;
      sleep((unsigned)delay);
    }

    curl = enil_curl_new(buf);
    if (!curl) {
      LOG("download", "curl init failed");
      return -1;
    }

    hdrs = curl_slist_append(hdrs, access_hdr);
    if (meta_hdr) hdrs = curl_slist_append(hdrs, meta_hdr);
    hdrs = curl_slist_append(hdrs, app_hdr);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, identity->user_agent);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
      if (content_type && content_type_buf && content_type_len > 0) {
        strncpy(content_type_buf, content_type, content_type_len - 1);
        content_type_buf[content_type_len - 1] = '\0';
      }
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc == CURLE_OK && http_status == 200) return 0;

    if (rc == CURLE_OK && http_status == 202) continue; /* not ready — back off */

    /* Hard failure (curl error or non-200/202 status) — do not retry. */
    {
      char tag[64];
      snprintf(tag, sizeof(tag), "Obs.%s", label);
      ENIL_LOG(tag,
               "HTTP %ld curl=%s response_bytes=%lu url=%s",
               http_status, curl_easy_strerror(rc),
               (unsigned long)buf->size, url);
    }
    log_text_preview("response", buf->data, buf->size);
    return -1;
  }

  {
    char tag[64];
    snprintf(tag, sizeof(tag), "Obs.%s", label);
    ENIL_LOG(tag,
             "HTTP 202 still not ready after %d retries — giving up url=%s",
             kRetries, url);
  }
  return -1;
}

/* --- X-Talk-Meta ---
 * Encodes: base64(JSON.stringify({message: base64(thrift_bytes)}))
 * Thrift layout:
 *   11 00 04 [i32:id_len] [id_bytes]  -- string field 4 = messageId
 *   15 00 1B 0C 00 00 00 00           -- list<struct> field 27, size 0
 *   00                                -- stop
 */
static char *build_talk_meta(const char *message_id) {
  size_t id_len  = strlen(message_id);
  size_t buf_len = 16 + id_len;
  unsigned char *buf = (unsigned char *)malloc(buf_len);
  char *inner_b64, *json, *outer_b64;
  size_t json_len;
  size_t o = 0;

  if (!buf) return NULL;

  buf[o++] = 11;
  buf[o++] = 0; buf[o++] = 4;
  buf[o++] = (unsigned char)(id_len >> 24);
  buf[o++] = (unsigned char)(id_len >> 16);
  buf[o++] = (unsigned char)(id_len >> 8);
  buf[o++] = (unsigned char)(id_len);
  memcpy(buf + o, message_id, id_len); o += id_len;
  buf[o++] = 15;
  buf[o++] = 0; buf[o++] = 27;
  buf[o++] = 12;
  buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
  buf[o]   = 0;

  inner_b64 = enil_b64_encode(buf, buf_len);
  free(buf);
  if (!inner_b64) return NULL;

  json_len = strlen("{\"message\":\"") + strlen(inner_b64) + strlen("\"}");
  json = (char *)malloc(json_len + 1);
  if (!json) { free(inner_b64); return NULL; }
  snprintf(json, json_len + 1, "{\"message\":\"%s\"}", inner_b64);
  free(inner_b64);

  outer_b64 = enil_b64_encode((unsigned char *)json, strlen(json));
  free(json);
  return outer_b64;
}

static int write_bytes(const char *path, const unsigned char *data,
                       size_t len)
{
  FILE *fp;

  if (!path || !data) {
    LOG("write", "NULL argument");
    return -1;
  }

  fp = fopen(path, "wb");
  if (!fp) {
    LOG("write", "cannot open dest_path");
    return -1;
  }

  if (fwrite(data, 1, len, fp) != len) {
    fclose(fp);
    LOG("write", "short write");
    return -1;
  }

  fclose(fp);
  return 0;
}

static unsigned char *decrypt_payload(const char *enc_km,
                                      const unsigned char *encrypted,
                                      size_t enc_len,
                                      size_t *plain_len)
{
  unsigned char *km_buf, *plain = NULL;
  int km_len;

  if (!enc_km || !encrypted || !plain_len) return NULL;

  km_buf = (unsigned char *)malloc(strlen(enc_km) * 3 / 4 + 4);
  if (!km_buf) return NULL;

  km_len = enil_b64_decode(enc_km, km_buf);
  if (km_len <= 0) {
    LOG("download", "ENC_KM base64 decode failed");
    free(km_buf);
    return NULL;
  }
  if (enil_crypto_file_decrypt(km_buf, (size_t)km_len,
                               encrypted, enc_len,
                               &plain, plain_len) != 0) {
    free(km_buf);
    return NULL;
  }
  free(km_buf);
  return plain;
}

/* --- Upload --- */

/* Read encryptedAccessTokens["2"] from session.json. Returns a malloc'd token
 * string on success, NULL on failure. */
static char *load_obs_token(const char *session_path) {
  cJSON *session = enil_session_read(session_path);
  cJSON *enc_tokens, *t;
  char *out = NULL;
  if (!session) return NULL;
  enc_tokens = cJSON_GetObjectItem(session, "encryptedAccessTokens");
  t = cJSON_GetObjectItem(enc_tokens, "2");
  if (cJSON_IsString(t) && t->valuestring) out = strdup(t->valuestring);
  cJSON_Delete(session);
  return out;
}

/* Format a UUID-style 36-char string into out (37 bytes including '\0').
 * Returns 0 on success, -1 on RAND failure. */
static int gen_reqid_uuid(char *out) {
  unsigned char b[16];
  if (RAND_bytes(b, 16) != 1) return -1;
  snprintf(out, 37,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
    b[10],b[11],b[12],b[13],b[14],b[15]);
  return 0;
}

/* Returns "image" for sid "m", else "file" (matches matrix-line obsTypeFromSID). */
static const char *obs_type_for_sid(const char *sid) {
  if (sid && strcmp(sid, "m") == 0) return "image";
  return "file";
}

/* Build the X-Obs-Params header value:
 *   base64({"ver":"2.0","name":"<ms_epoch>","type":"<obs_type>"})
 * Returns a malloc'd base64 string, or NULL on failure. */
static char *build_obs_params(const char *obs_type) {
  struct timeval tv;
  long long ms;
  char json[160];
  int n;
  gettimeofday(&tv, NULL);
  ms = (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
  n = snprintf(json, sizeof(json),
               "{\"ver\":\"2.0\",\"name\":\"%lld\",\"type\":\"%s\"}",
               ms, obs_type ? obs_type : "file");
  if (n < 0 || (size_t)n >= sizeof(json)) return NULL;
  return enil_b64_encode((unsigned char *)json, (size_t)n);
}

/* Header capture for x-obs-oid. userdata is a (char **) where the duped value
 * is stored when the header is seen. */
static size_t oid_header_cb(char *buffer, size_t size, size_t nitems,
                            void *userdata) {
  static const char kPrefix[] = "x-obs-oid:";
  size_t total = size * nitems;
  size_t prefix_len = sizeof(kPrefix) - 1;
  char **slot = (char **)userdata;
  const char *p, *end;
  size_t value_len;
  char *out;

  if (total <= prefix_len) return total;
  if (strncasecmp(buffer, kPrefix, prefix_len) != 0) return total;

  p = buffer + prefix_len;
  end = buffer + total;
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  while (end > p && (end[-1] == '\r' || end[-1] == '\n' ||
                     end[-1] == ' '  || end[-1] == '\t')) end--;
  value_len = (size_t)(end - p);
  if (value_len == 0) return total;

  out = (char *)malloc(value_len + 1);
  if (!out) return total;
  memcpy(out, p, value_len);
  out[value_len] = '\0';
  free(*slot);
  *slot = out;
  return total;
}

/* Allocate "Name: Value" header strings. Caller frees. */
static char *make_header(const char *name, const char *value) {
  size_t n;
  char *h;
  if (!name || !value) return NULL;
  n = strlen(name) + 2 + strlen(value) + 1;
  h = (char *)malloc(n);
  if (!h) return NULL;
  snprintf(h, n, "%s: %s", name, value);
  return h;
}

/* POST `data` to `url`. If out_oid is non-NULL, capture x-obs-oid from the
 * response headers into *out_oid (malloc'd). Returns 0 on HTTP 200/201, -1
 * otherwise. */
static int obs_post(const char *label, const char *url,
                    const unsigned char *data, size_t len,
                    const char *obs_token, const char *obs_params_b64,
                    char **out_oid) {
  CURL *curl;
  struct curl_slist *hdrs = NULL;
  CURLcode rc;
  long http_status = 0;
  ENILBuf body = {NULL, 0};
  char *access_hdr = NULL;
  char *app_hdr    = NULL;
  char *params_hdr = NULL;
  char *captured_oid = NULL;
  int result = -1;
  char tag[64];
  const enil_identity_t *identity = enil_identity_current();

  if (!identity) return -1;
  snprintf(tag, sizeof(tag), "Obs.%s", label ? label : "?");

  if (!url || !obs_token || !obs_params_b64) {
    ENIL_LOG(tag, "NULL argument");
    return -1;
  }

  ENIL_LOG(tag, "POST %s (%lu bytes)", url, (unsigned long)len);

  access_hdr = make_header("X-Line-Access", obs_token);
  app_hdr    = make_header("X-Line-Application", identity->application);
  params_hdr = make_header("X-Obs-Params", obs_params_b64);
  if (!access_hdr || !app_hdr || !params_hdr) {
    ENIL_LOG(tag, "header alloc failed");
    goto done;
  }

  curl = enil_curl_new(&body);
  if (!curl) { ENIL_LOG(tag, "curl init failed"); goto done; }

  hdrs = curl_slist_append(hdrs, access_hdr);
  hdrs = curl_slist_append(hdrs, app_hdr);
  hdrs = curl_slist_append(hdrs, "x-lal: en_US");
  hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
  hdrs = curl_slist_append(hdrs, params_hdr);

  curl_easy_setopt(curl, CURLOPT_USERAGENT, identity->user_agent);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)data);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
  if (out_oid) {
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, oid_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &captured_oid);
  }

  rc = curl_easy_perform(curl);
  if (rc == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || (http_status != 200 && http_status != 201)) {
    ENIL_LOG(tag, "HTTP %ld curl=%s url=%s",
             http_status, curl_easy_strerror(rc), url);
    log_text_preview("response", body.data, body.size);
    goto done;
  }
  ENIL_LOG(tag, "-> HTTP %ld: %s", http_status, url);

  if (out_oid) {
    if (!captured_oid) { ENIL_LOG(tag, "no x-obs-oid in response"); goto done; }
    *out_oid = captured_oid;
    captured_oid = NULL;
  }
  result = 0;

done:
  enil_buf_free(&body);
  free(access_hdr); free(app_hdr); free(params_hdr);
  free(captured_oid);
  return result;
}

/* ============================================================================
 * Upload bytes to OBS at /r/talk/<sid>/reqid-<uuid>. Returns the OID assigned
 * by the server via the x-obs-oid response header.
 * ==========================================================================*/
int enil_obs_upload(const char           *session_path,
                    const unsigned char  *data,
                    size_t                len,
                    const char           *sid,
                    char                **out_oid) {
  char *obs_token = NULL;
  char *params    = NULL;
  char  reqid[37];
  char  url[1024];
  int   rc = -1;

  if (!session_path || !data || !sid || !out_oid) {
    LOG("upload", "NULL argument"); return -1;
  }
  *out_oid = NULL;

  if (!enil_session_bind_identity(session_path)) return -1;
  obs_token = load_obs_token(session_path);
  if (!obs_token) { LOG("upload", "no OBS token in session"); goto done; }

  if (gen_reqid_uuid(reqid) != 0) { LOG("upload", "RAND_bytes failed"); goto done; }
  if (snprintf(url, sizeof(url), "%s/r/talk/%s/reqid-%s",
               OBS_BASE, sid, reqid) >= (int)sizeof(url)) {
    LOG("upload", "URL truncated"); goto done;
  }

  params = build_obs_params(obs_type_for_sid(sid));
  if (!params) { LOG("upload", "build_obs_params failed"); goto done; }

  rc = obs_post("upload", url, data, len, obs_token, params, out_oid);

done:
  free(obs_token);
  free(params);
  return rc;
}

/* ============================================================================
 * Upload bytes to OBS at a caller-chosen OID, /r/talk/<sid>/<oid>. Used for
 * thumbnail previews where the OID is "<media_oid>__ud-preview".
 * ==========================================================================*/
int enil_obs_upload_with_oid(const char          *session_path,
                              const unsigned char *data,
                              size_t               len,
                              const char          *sid,
                              const char          *oid) {
  char *obs_token = NULL;
  char *params    = NULL;
  char  url[1024];
  int   rc = -1;

  if (!session_path || !data || !sid || !oid) {
    LOG("upload_oid", "NULL argument"); return -1;
  }

  if (!enil_session_bind_identity(session_path)) return -1;
  obs_token = load_obs_token(session_path);
  if (!obs_token) { LOG("upload_oid", "no OBS token in session"); goto done; }

  if (snprintf(url, sizeof(url), "%s/r/talk/%s/%s",
               OBS_BASE, sid, oid) >= (int)sizeof(url)) {
    LOG("upload_oid", "URL truncated"); goto done;
  }

  params = build_obs_params(obs_type_for_sid(sid));
  if (!params) { LOG("upload_oid", "build_obs_params failed"); goto done; }

  /* The "<oid>__ud-preview" thumbnail POST occasionally dies mid-transfer
     (curl "Failed sending data to the peer"). It is best-effort — the
     message still lands without it — but a couple of cheap retries
     usually recover the preload thumb. Only the preview path retries;
     the main-image PUT (OID "m"/msgId) keeps its single attempt. */
  {
    static const char kPreviewSuffix[] = "__ud-preview";
    static const int  kPreviewBackoff[] = { 5, 10 };
    const int kPreviewRetries =
      (int)(sizeof(kPreviewBackoff) / sizeof(kPreviewBackoff[0]));
    size_t olen = strlen(oid);
    size_t slen = sizeof(kPreviewSuffix) - 1;
    int is_preview = (olen >= slen) &&
      strcmp(oid + olen - slen, kPreviewSuffix) == 0;
    int max_retries = is_preview ? kPreviewRetries : 0;
    int attempt;

    for (attempt = 0; attempt <= max_retries; attempt++) {
      if (attempt > 0) {
        int delay = kPreviewBackoff[attempt - 1];
        ENIL_LOG("Obs.upload_oid",
                 "preview POST failed — retry %d/%d in %ds oid=%s",
                 attempt, kPreviewRetries, delay, oid);
        sleep((unsigned)delay);
      }
      rc = obs_post("upload_oid", url, data, len, obs_token, params, NULL);
      if (rc == 0) break;
    }
  }

done:
  free(obs_token);
  free(params);
  return rc;
}

/* --- Main --- */

/* ============================================================================
 * Download a single OBS attachment to dest_path. Handles HMAC signing,
 * obsPop routing, and E2EE keychain unwrap when e2ee_version is set.
 * Returns 0 on success, -1 on any failure.
 * ==========================================================================*/
int enil_obs_download_message(const char *session_path,
                               const char *message_id,
                               const char *sid,
                               const char *oid,
                               const char *obs_pop,
                               const char *e2ee_version,
                               const char *enc_km,
                               size_t plain_size,
                               const char *dest_path)
{
  cJSON *session, *enc_tokens, *token_item;
  char obs_token_buf[4096];
  char url[1024];
  const enil_identity_t *identity;
  char content_type_buf[128];
  char *talk_meta = NULL;
  char *access_hdr = NULL;
  char *meta_hdr = NULL;
  char *app_hdr = NULL;
  char object_info_url[1060];
  ENILBuf buf = {NULL, 0};
  unsigned char *plain;
  size_t plain_len;
  const unsigned char *encrypted;
  int result = -1;

  content_type_buf[0] = '\0';

  if (!session_path || !message_id || !dest_path) {
    LOG("download", "NULL argument"); return -1;
  }

  if (!enil_session_bind_identity(session_path)) return -1;
  identity = enil_identity_current();

  /* Read OBS token from session.json */
  session = enil_session_read(session_path);
  if (!session) { LOG("download", "cannot read session"); return -1; }
  enc_tokens = cJSON_GetObjectItem(session, "encryptedAccessTokens");
  token_item = cJSON_GetObjectItem(enc_tokens, "2");
  if (!cJSON_IsString(token_item)) {
    LOG("download", "missing encryptedAccessTokens[2] - run sync first");
    cJSON_Delete(session);
    return -1;
  }
  strncpy(obs_token_buf, token_item->valuestring, sizeof(obs_token_buf) - 1);
  obs_token_buf[sizeof(obs_token_buf) - 1] = '\0';
  cJSON_Delete(session);

  if (build_obs_url(url, sizeof(url), sid, oid, message_id, obs_pop, NULL) != 0) {
    LOG("download", "build URL failed");
    return -1;
  }

  if ((enc_km && enc_km[0]) || (e2ee_version && e2ee_version[0])) {
    talk_meta = build_talk_meta(message_id);
    if (!talk_meta) { LOG("download", "build_talk_meta failed"); return -1; }
  }

  {
    size_t n;
    n = strlen("X-Line-Access: ")     + strlen(obs_token_buf) + 1;
    access_hdr = (char *)malloc(n); if (access_hdr) snprintf(access_hdr, n, "X-Line-Access: %s", obs_token_buf);
    if (talk_meta) {
      n = strlen("X-Talk-Meta: ") + strlen(talk_meta) + 1;
      meta_hdr = (char *)malloc(n);
      if (meta_hdr) snprintf(meta_hdr, n, "X-Talk-Meta: %s", talk_meta);
    }
    n = strlen("X-Line-Application: ") + strlen(identity->application) + 1;
    app_hdr    = (char *)malloc(n); if (app_hdr)    snprintf(app_hdr,    n, "X-Line-Application: %s", identity->application);
  }

  if (!access_hdr || (talk_meta && !meta_hdr) || !app_hdr) {
    LOG("download", "header alloc failed");
    free(access_hdr); free(meta_hdr); free(app_hdr);
    free(talk_meta);
    return -1;
  }
  free(talk_meta);

  if (e2ee_version && e2ee_version[0]) {
    if (build_obs_url(object_info_url, sizeof(object_info_url),
                      sid, oid, message_id, obs_pop,
                      "/object_info.obs") != 0) {
      LOG("object_info", "build URL failed");
      free(access_hdr); free(meta_hdr); free(app_hdr);
      return -1;
    }
    if (obs_get("object_info", object_info_url, access_hdr, meta_hdr, app_hdr,
                &buf, content_type_buf, sizeof(content_type_buf)) != 0) {
      enil_buf_free(&buf);
      free(access_hdr); free(meta_hdr); free(app_hdr);
      return -1;
    }
    enil_buf_free(&buf);
    buf.data = NULL;
    buf.size = 0;
  }

  if (obs_get("download", url, access_hdr,
              (enc_km && enc_km[0]) || (e2ee_version && e2ee_version[0]) ? meta_hdr : NULL,
              app_hdr, &buf, content_type_buf,
              sizeof(content_type_buf)) != 0) {
    enil_buf_free(&buf);
    free(access_hdr); free(meta_hdr); free(app_hdr);
    return -1;
  }
  free(access_hdr); free(meta_hdr); free(app_hdr);

  /* Verify and decrypt */
  encrypted = (unsigned char *)buf.data;
  if (!enc_km || !enc_km[0]) {
    size_t write_len = (plain_size > 0 && plain_size <= buf.size)
                       ? plain_size : buf.size;
    result = write_bytes(dest_path, encrypted, write_len);
    enil_buf_free(&buf);
    return result;
  }

  plain = decrypt_payload(enc_km, encrypted, buf.size, &plain_len);
  enil_buf_free(&buf);
  if (!plain) return -1;

  result = write_bytes(dest_path, plain, plain_len);
  free(plain);
  return result;
}
