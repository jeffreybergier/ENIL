/* ============================================================================
 * Message body formatter — expands REPLACE markers, inline sticons, mentions,
 * and emoji into the display string consumed by the WebView renderer.
 * ==========================================================================*/

#include "enil_message_format.h"
#include "enil_db.h"
#include "enil_cocoa_log.h"
#include "enil_render_scale.h"
#include "enil_strbuf.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int is_boundary(char ch) {
  unsigned char c = (unsigned char)ch;
  if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') || c == '_')
    return 0;
  return 1;
}

/* Render a sticon at 1/divisor of its bitmap size. Normal inline sticons use
 * 5; a sticon-only ("jumbo") message uses 2. The source assets are large, so
 * downscaling the box keeps sticons text-sized while preserving crispness on
 * Retina (plenty of oversampling headroom). */
static void append_sticon_img(ENILStrBuf *b, const char *path,
                              int width, int height, double divisor) {
  char dim[96];
  if (!path || !path[0] || divisor <= 0.0) return;
  esb_append_str(b, "<img class=\"enil-sticon-inline\" src=\"");
  esb_html_br(b, path);
  esb_append_str(b, "\"");
  if (width > 0 && height > 0) {
    int w = (int)(width  / divisor + 0.5);   /* round-to-nearest, explicit cast */
    int h = (int)(height / divisor + 0.5);
    snprintf(dim, sizeof(dim), " width=\"%d\" height=\"%d\" style=\"width:%dpx;height:%dpx\"",
             w, h, w, h);
    esb_append_str(b, dim);
  } else {
    /* Dimensions unknown: scale the intrinsic bitmap via WebKit zoom (CSS
     * percentages resolve against the line box, not the image). */
    snprintf(dim, sizeof(dim), " style=\"zoom:%g\"", 1.0 / divisor);
    esb_append_str(b, dim);
  }
  esb_append_str(b, ">");
}

static int resolve_resource(cJSON *resource, char *path, size_t path_size,
                            int *width, int *height) {
  const char *image_path, *pkg_id, *sticon_id;
  if (!resource) {
    ENIL_LOG("MessageFormat.resolve_resource", "NULL resource");
    return 0;
  }
  image_path = cJSON_GetStringValue(cJSON_GetObjectItem(resource, "image_path"));
  pkg_id     = cJSON_GetStringValue(cJSON_GetObjectItem(resource, "package_id"));
  sticon_id  = cJSON_GetStringValue(cJSON_GetObjectItem(resource, "sticon_id"));
  if (!image_path || !image_path[0]) {
    ENIL_LOG("MessageFormat.resolve_resource",
             "missing image for pkg=%s sticon=%s",
             pkg_id ? pkg_id : "(null)", sticon_id ? sticon_id : "(null)");
    return 0;
  }
  if (!path || path_size == 0) return 0;
  snprintf(path, path_size, "%s", image_path);
  if (width)
    *width = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(resource, "image_width"));
  if (height)
    *height = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(resource, "image_height"));
  return 1;
}

static int is_standalone_dollar(const char *text, size_t cursor) {
  size_t len;
  char prev, next;
  if (!text || text[cursor] != '$') return 0;
  len = strlen(text);
  prev = cursor > 0 ? text[cursor - 1] : ' ';
  next = cursor + 1 < len ? text[cursor + 1] : ' ';
  return is_boundary(prev) && is_boundary(next);
}

static int run_is_blank(const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    char c = s[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return 0;
  }
  return 1;
}

/* True if resource slot `idx` would resolve to an image (mirrors the
 * image_path check in resolve_resource, without logging). */
static int marker_resolves(cJSON *resources, int idx) {
  cJSON *r = cJSON_GetArrayItem(resources, idx);
  const char *img = r ? cJSON_GetStringValue(cJSON_GetObjectItem(r, "image_path")) : NULL;
  return img && img[0] ? 1 : 0;
}

/* True when every marker resolves to a sticon and all remaining text is
 * whitespace — i.e. the message is sticons-only and qualifies for jumbo
 * sizing. Mirrors the marker rules in format_text_html so the decision
 * matches what actually renders. */
static int sticons_only(const char *text, cJSON *resources) {
  size_t cursor = 0, start = 0, len;
  int idx = 0, n_sticons = 0, n_res;
  if (!text || !text[0] || !cJSON_IsArray(resources)) return 0;
  n_res = cJSON_GetArraySize(resources);
  if (n_res == 0) return 0;
  len = strlen(text);
  while (cursor < len) {
    const char *close = (text[cursor] == '(') ? strchr(text + cursor + 1, ')') : NULL;
    int is_dollar = is_standalone_dollar(text + start, cursor - start);
    if (!close && !is_dollar) { cursor++; continue; }
    if (!run_is_blank(text + start, cursor - start)) return 0;
    if (idx >= n_res || !marker_resolves(resources, idx)) return 0;
    idx++;
    n_sticons++;
    cursor = close ? (size_t)(close - text) + 1 : cursor + 1;
    start = cursor;
  }
  if (!run_is_blank(text + start, len - start)) return 0;
  return n_sticons > 0;
}

static char *format_text_html(const char *text, cJSON *resources) {
  double divisor = sticons_only(text, resources)
                 ? ENIL_STICON_JUMBO_SCALE_DIV : ENIL_STICON_SCALE_DIV;
  ENILStrBuf html;
  const char *base = text;
  size_t start = 0, cursor = 0, text_len;
  int resource_index = 0;
  int has_resources;
  if (!text || !text[0]) return strdup("");
  if (!esb_init(&html)) return NULL;
  has_resources = cJSON_IsArray(resources) && cJSON_GetArraySize(resources) > 0;
  text_len = strlen(base);
  while (cursor < text_len) {
    if (has_resources && base[cursor] == '(') {
      const char *close = strchr(base + cursor + 1, ')');
      if (close) {
        size_t close_pos = (size_t)(close - base);
        char path[512];
        int width = 0, height = 0, resolved = 0;
        if (resource_index < cJSON_GetArraySize(resources)) {
          resolved = resolve_resource(cJSON_GetArrayItem(resources, resource_index),
                                      path, sizeof(path), &width, &height);
          resource_index++;
        } else {
          ENIL_LOG("MessageFormat.format_text_html",
                   "more (label) markers than sticon resources (idx=%d size=%d)",
                   resource_index, cJSON_GetArraySize(resources));
        }
        esb_html_br_n(&html, base + start, cursor - start);
        if (resolved)
          append_sticon_img(&html, path, width, height, divisor);
        else
          esb_html_br_n(&html, base + cursor, close_pos - cursor + 1);
        start = close_pos + 1;
        cursor = start;
        continue;
      }
    }
    if (has_resources && is_standalone_dollar(base + start, cursor - start)) {
      char path[512];
      int width = 0, height = 0, resolved = 0;
      if (resource_index < cJSON_GetArraySize(resources)) {
        resolved = resolve_resource(cJSON_GetArrayItem(resources, resource_index),
                                    path, sizeof(path), &width, &height);
        resource_index++;
      } else {
        ENIL_LOG("MessageFormat.format_text_html",
                 "more $ markers than sticon resources (idx=%d size=%d)",
                 resource_index, cJSON_GetArraySize(resources));
      }
      esb_html_br_n(&html, base + start, cursor - start);
      if (resolved)
        append_sticon_img(&html, path, width, height, divisor);
      else
        esb_html_br_n(&html, base + cursor, 1);
      start = cursor + 1;
      cursor = start;
      continue;
    }
    cursor++;
  }
  if (html.len == 0) esb_html_br(&html, base);
  else if (base[start]) esb_html_br(&html, base + start);
  return html.buf;
}

/* ============================================================================
 * Render message text to HTML, replacing $-markers with the stored sticon
 * resources for message_id. Caller must free the returned string.
 * ==========================================================================*/
char *enil_message_text_html(sqlite3 *db, const char *message_id,
                             const char *text) {
  cJSON *resources;
  char *html;
  if (!message_id || !message_id[0])
    return format_text_html(text, NULL);
  resources = enil_db_get_message_sticons(db, message_id);
  html = format_text_html(text, resources);
  cJSON_Delete(resources);
  return html;
}

/* ============================================================================
 * Like enil_message_text_html but takes explicit (package_id, sticon_id)
 * arrays rather than looking up by message id — used for outgoing previews.
 * ==========================================================================*/
char *enil_message_text_html_for_sticons(sqlite3 *db, const char *text,
                                         int count,
                                         const char * const *package_ids,
                                         const char * const *sticon_ids) {
  cJSON *resources;
  char *html;
  int i;
  resources = cJSON_CreateArray();
  for (i = 0; i < count; i++) {
    char path[512];
    int w = 0, h = 0;
    cJSON *row = cJSON_CreateObject();
    const char *pkg = package_ids ? package_ids[i] : NULL;
    const char *sid = sticon_ids ? sticon_ids[i] : NULL;
    path[0] = '\0';
    if (pkg && sid)
      enil_db_get_sticker_image_info_for_package(db, pkg, sid, path, sizeof(path), &w, &h);
    cJSON_AddStringToObject(row, "image_path", path);
    cJSON_AddNumberToObject(row, "image_width", w);
    cJSON_AddNumberToObject(row, "image_height", h);
    cJSON_AddItemToArray(resources, row);
  }
  html = format_text_html(text, resources);
  cJSON_Delete(resources);
  return html;
}

/* ============================================================================
 * Resolve a sticker's on-disk path and dimensions. Prefers a package-scoped
 * match, falls back to a global sticker id lookup. Returns 1 on hit, 0 on miss.
 * ==========================================================================*/
int enil_message_sticker_image_info(sqlite3 *db, const char *sticker_id,
                                    const char *package_id,
                                    char *path, size_t path_size,
                                    int *width, int *height) {
  int ok = 0;
  if (!db || !path || path_size == 0 || !width || !height) return 0;
  path[0] = '\0';
  *width = 0;
  *height = 0;
  if (sticker_id && sticker_id[0] && package_id && package_id[0])
    ok = enil_db_get_sticker_image_info_for_package(db, package_id, sticker_id,
                                                    path, path_size, width, height);
  if (!ok && sticker_id && sticker_id[0])
    ok = enil_db_get_sticker_image_info(db, sticker_id, path, path_size, width, height);
  return ok;
}

/* ============================================================================
 * Free a buffer returned by enil_message_text_html*. Safe to pass NULL.
 * ==========================================================================*/
void enil_message_format_free(char *p) {
  free(p);
}
