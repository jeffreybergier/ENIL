/* ============================================================================
 * Growable byte buffer + HTML/JS/URL escaping; see enil_strbuf.h. One copy of
 * the machinery the HTML renderer and message formatter both need.
 * ==========================================================================*/

#include "enil_strbuf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int esb_init(ENILStrBuf *b) {
  if (!b) return 0;
  b->buf = (char *)malloc(1024);
  b->len = 0;
  b->cap = b->buf ? 1024 : 0;
  if (b->buf) b->buf[0] = '\0';
  return b->buf != NULL;
}

static int esb_grow(ENILStrBuf *b, size_t need) {
  size_t ncap;
  char *nb;
  if (!b || !b->buf) return 0;
  if (b->len + need < b->cap) return 1;
  ncap = b->cap ? b->cap * 2 : 1024;
  while (ncap <= b->len + need) ncap *= 2;
  nb = (char *)realloc(b->buf, ncap);
  if (!nb) return 0;
  b->buf = nb;
  b->cap = ncap;
  return 1;
}

void esb_append(ENILStrBuf *b, const char *s, size_t n) {
  if (!s || !esb_grow(b, n + 1)) return;
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

void esb_append_str(ENILStrBuf *b, const char *s) {
  if (s) esb_append(b, s, strlen(s));
}

void esb_int(ENILStrBuf *b, int v) {
  char t[16];
  int n = snprintf(t, sizeof(t), "%d", v);
  if (n > 0) esb_append(b, t, (size_t)n);
}

/* Single-pass HTML escape over n bytes: copies runs of plain chars, expands
 * special chars. When `br` is set, '\n' becomes "<br>" (display text); when
 * clear, newlines pass through (script/attribute context). */
static void esb_html_core(ENILStrBuf *b, const char *s, size_t n, int br) {
  size_t i, run = 0;
  if (!s || n == 0) return;
  for (i = 0; i < n; i++) {
    const char *e = NULL;
    size_t el = 0;
    switch (s[i]) {
      case '&': e = "&amp;";  el = 5; break;
      case '<': e = "&lt;";   el = 4; break;
      case '>': e = "&gt;";   el = 4; break;
      case '"': e = "&quot;"; el = 6; break;
      case '\n': if (br) { e = "<br>"; el = 4; } break;
    }
    if (e) {
      if (i > run) esb_append(b, s + run, i - run);
      esb_append(b, e, el);
      run = i + 1;
    }
  }
  if (n > run) esb_append(b, s + run, n - run);
}

void esb_html(ENILStrBuf *b, const char *s) {
  if (s) esb_html_core(b, s, strlen(s), 0);
}

void esb_html_n(ENILStrBuf *b, const char *s, size_t n) {
  esb_html_core(b, s, n, 0);
}

void esb_html_br(ENILStrBuf *b, const char *s) {
  if (s) esb_html_core(b, s, strlen(s), 1);
}

void esb_html_br_n(ENILStrBuf *b, const char *s, size_t n) {
  esb_html_core(b, s, n, 1);
}

/* Single-pass JS string escape (no surrounding quotes). */
void esb_js(ENILStrBuf *b, const char *s) {
  const char *run;
  if (!s || !*s) return;
  run = s;
  for (; *s; s++) {
    const char *e = NULL;
    size_t el = 0;
    switch (*s) {
      case '\\': e = "\\\\"; el = 2; break;
      case '"':  e = "\\\""; el = 2; break;
      case '\n': e = "\\n";  el = 2; break;
      case '\r': e = "\\r";  el = 2; break;
    }
    if (e) {
      if (s > run) esb_append(b, run, (size_t)(s - run));
      esb_append(b, e, el);
      run = s + 1;
    }
  }
  if (s > run) esb_append(b, run, (size_t)(s - run));
}

static int is_url_unreserved(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return 1;
  if (c >= 'a' && c <= 'z') return 1;
  if (c >= '0' && c <= '9') return 1;
  return c == '-' || c == '_' || c == '.' || c == '~';
}

/* Percent-encode. When `keep_slash` is set, '/' is treated as safe so a
 * filesystem path used in a file:// URL stays traversable. */
static void esb_url_core(ENILStrBuf *b, const char *s, int keep_slash) {
  static const char hex[] = "0123456789ABCDEF";
  const unsigned char *p = (const unsigned char *)s;
  char enc[3];
  if (!s) return;
  enc[0] = '%';
  for (; *p; p++) {
    if (is_url_unreserved(*p) || (keep_slash && *p == '/')) {
      esb_append(b, (const char *)p, 1);
    } else {
      enc[1] = hex[*p >> 4];
      enc[2] = hex[*p & 15];
      esb_append(b, enc, 3);
    }
  }
}

void esb_url(ENILStrBuf *b, const char *s) {
  esb_url_core(b, s, 0);
}

void esb_url_path(ENILStrBuf *b, const char *s) {
  esb_url_core(b, s, 1);
}
