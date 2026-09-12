#ifndef ENIL_STRBUF_H
#define ENIL_STRBUF_H

#include <stddef.h>

/* ============================================================================
 * Growable byte buffer + HTML/JS/URL escaping helpers shared by the HTML page
 * renderer (enil_html.c) and the message-body formatter (enil_message_format.c).
 * Both previously carried byte-for-byte copies of this machinery; the only
 * behavioural difference was newline handling during HTML escape, which is now
 * expressed by the two entry points esb_html (leaves '\n' alone) and
 * esb_html_br ('\n' -> "<br>"). All appenders are no-ops on a failed/empty
 * buffer or NULL input, so call sites need not check after esb_init.
 * ==========================================================================*/
typedef struct { char *buf; size_t len, cap; } ENILStrBuf;

/* Allocate the backing store. Returns 1 on success, 0 on alloc failure
 * (in which case every appender below is a safe no-op). */
int  esb_init(ENILStrBuf *b);

/* Append n raw bytes / a NUL-terminated string verbatim (no escaping). */
void esb_append(ENILStrBuf *b, const char *s, size_t n);
void esb_append_str(ENILStrBuf *b, const char *s);

/* Append a decimal int. */
void esb_int(ENILStrBuf *b, int v);

/* HTML-escape (&, <, >, "): esb_html* leave newlines, esb_html_br* map
 * '\n' to "<br>". The _n forms process exactly n bytes; the others strlen. */
void esb_html(ENILStrBuf *b, const char *s);
void esb_html_n(ENILStrBuf *b, const char *s, size_t n);
void esb_html_br(ENILStrBuf *b, const char *s);
void esb_html_br_n(ENILStrBuf *b, const char *s, size_t n);

/* JS string escape (\\, ", \n, \r): no surrounding quotes. */
void esb_js(ENILStrBuf *b, const char *s);

/* RFC 3986 percent-encode. esb_url keeps only unreserved chars; esb_url_path
 * additionally keeps '/' so file:// paths stay traversable. */
void esb_url(ENILStrBuf *b, const char *s);
void esb_url_path(ENILStrBuf *b, const char *s);

/* Append a string literal without a strlen call. */
#define ESB_LIT(b, s) esb_append((b), (s), sizeof(s) - 1)

#endif
