/* ============================================================================
 * Message body formatter — expands REPLACE markers, inline sticons, mentions,
 * and emoji into the display string consumed by the WebView renderer.
 * ==========================================================================*/

#include "enil_message_format.h"
#include "enil_db.h"
#include "enil_line.h"
#include <limits.h>
#include "enil_cocoa_log.h"
#include "enil_render_scale.h"
#include "enil_strbuf.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Render a sticon at 1/divisor of its bitmap size. Normal inline sticons use
 * the inline scale constant; sticon-only messages use the jumbo constant.
 * The source assets are large, so
 * downscaling the box keeps sticons text-sized while preserving crispness on
 * Retina (plenty of oversampling headroom). */
static void append_sticon_img(ENILStrBuf *b, const char *path,
                              int width, int height, double divisor,
                              const char *marker, size_t marker_len) {
  char dim[96];
  if (!path || !path[0] || divisor <= 0.0) return;
  esb_append_str(b, "<img class=\"enil-sticon-inline\" src=\"");
  esb_html_br(b, path);
  esb_append_str(b, "\" alt=\"");
  esb_html_br_n(b, marker, marker_len);
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

/* Convert UTF-16 code-unit boundaries to UTF-8 byte boundaries. Never split
 * a surrogate pair or accept malformed UTF-8. */
static int utf16_boundary(const char *text, int target, size_t *offset) {
  size_t p = 0;
  int units = 0;
  while (units < target && text[p]) {
    unsigned char c = (unsigned char)text[p];
    unsigned int cp;
    int n, j;
    if (c < 0x80) { n = 1; cp = c; }
    else if (c >= 0xc2 && c <= 0xdf) { n = 2; cp = c & 31; }
    else if (c >= 0xe0 && c <= 0xef) { n = 3; cp = c & 15; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 4; cp = c & 7; }
    else return 0;
    for (j = 1; j < n; j++) {
      c = (unsigned char)text[p + j];
      if ((c & 0xc0) != 0x80) return 0;
      cp = (cp << 6) | (c & 63);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
        (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return 0;
    units += cp > 0xffff ? 2 : 1;
    p += n;
  }
  if (units != target) return 0;
  *offset = p;
  return 1;
}

/* A positional span must still describe a marker, never arbitrary prose.
 * This also makes the fallback for old ENIL byte offsets conservative. */
static int marker_span(const char *text, size_t len, size_t s, size_t e) {
  size_t i;
  if (s >= e || e > len) return 0;
  if (e - s == 1 && text[s] == '$') return 1;
  if (e - s < 3 || text[s] != '(' || text[e - 1] != ')') return 0;
  for (i = s + 1; i + 1 < e; i++)
    if (text[i] == '(' || text[i] == ')' ||
        text[i] == '\n' || text[i] == '\r') return 0;
  return 1;
}

static int resource_span(const char *text, size_t len, cJSON *r,
                         size_t *start, size_t *end) {
  cJSON *s = cJSON_GetObjectItem(r, "S");
  cJSON *e = cJSON_GetObjectItem(r, "E");
  int a, b;
  if (!cJSON_IsNumber(s) || !cJSON_IsNumber(e) ||
      !(s->valuedouble >= 0 && s->valuedouble <= INT_MAX) ||
      !(e->valuedouble > s->valuedouble && e->valuedouble <= INT_MAX))
    return 0;
  a = (int)s->valuedouble; b = (int)e->valuedouble;
  if (a != s->valuedouble || b != e->valuedouble) return 0;
  if (!cJSON_IsTrue(cJSON_GetObjectItem(r, "byte_offsets")) &&
      utf16_boundary(text, a, start) && utf16_boundary(text, b, end) &&
      marker_span(text, len, *start, *end)) return 1;
  *start = (size_t)a; *end = (size_t)b;
  return marker_span(text, len, *start, *end);
}

static int run_is_blank(const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    char c = s[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return 0;
  }
  return 1;
}

/* Validate every range before replacing anything. Invalid or overlapping
 * metadata leaves the whole message readable, with literal markers. */
static char *format_text_html(const char *text, cJSON *resources) {
  ENILStrBuf html;
  cJSON *r;
  size_t len, cursor = 0, start, end;
  int valid = 1, jumbo = 1, count = 0;
  double divisor;
  if (!text || !text[0]) return strdup("");
  if (!esb_init(&html)) return NULL;
  len = strlen(text);
  cJSON_ArrayForEach(r, resources) {
    const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(r, "image_path"));
    if (!resource_span(text, len, r, &start, &end) || start < cursor) {
      valid = 0; break;
    }
    if (!run_is_blank(text + cursor, start - cursor) || !img || !img[0])
      jumbo = 0;
    cursor = end;
    count++;
  }
  if (!valid || !count) { esb_html_br(&html, text); return html.buf; }
  if (!run_is_blank(text + cursor, len - cursor)) jumbo = 0;
  divisor = jumbo ? ENIL_STICON_JUMBO_SCALE_DIV : ENIL_STICON_SCALE_DIV;
  cursor = 0;
  cJSON_ArrayForEach(r, resources) {
    char path[512];
    int width = 0, height = 0;
    resource_span(text, len, r, &start, &end);
    esb_html_br_n(&html, text + cursor, start - cursor);
    if (resolve_resource(r, path, sizeof(path), &width, &height))
      append_sticon_img(&html, path, width, height, divisor,
                        text + start, end - start);
    else
      esb_html_br_n(&html, text + start, end - start);
    cursor = end;
  }
  esb_html_br_n(&html, text + cursor, len - cursor);
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
                                         const char * const *sticon_ids,
                                         const char * const *alt_texts) {
  cJSON *resources;
  char *html;
  int i;
  ENILLineSticonResource *spans = enil_line_build_sticon_resources(
    text, count, package_ids, sticon_ids, alt_texts, NULL, NULL);
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
    if (spans) {
      cJSON_AddNumberToObject(row, "S", spans[i].S);
      cJSON_AddNumberToObject(row, "E", spans[i].E);
      cJSON_AddBoolToObject(row, "byte_offsets", 1);
    }
    cJSON_AddStringToObject(row, "image_path", path);
    cJSON_AddNumberToObject(row, "image_width", w);
    cJSON_AddNumberToObject(row, "image_height", h);
    cJSON_AddItemToArray(resources, row);
  }
  html = format_text_html(text, resources);
  free(spans);
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
