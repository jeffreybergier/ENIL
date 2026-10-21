/* Linux benchmark driver. Only synthetic local SQLite data is used. */
#include "enil_account.h"
#include "enil_db.h"
#include "enil_html.h"
#include <assert.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A future renderer change must not quietly turn this into a network test. */
CURLcode __wrap_curl_easy_perform(CURL *curl) {
  (void)curl; abort();
}
void enil_log(const char *tag, const char *fmt, ...) {
  (void)tag; (void)fmt;
}
/* No locale sorting is exercised by the chat-page workload. */
int enil_localized_collate(void *ctx, int na, const void *a,
                           int nb, const void *b) {
  int cmp;
  (void)ctx;
  cmp = memcmp(a, b, (size_t)(na < nb ? na : nb));
  return cmp ? cmp : na - nb;
}
/* Exercise the renderer's strftime fallback instead of unavailable CFDateFormatter. */
int enil_format_date_string(long long ms, char *out, size_t size) {
  (void)ms; (void)out; (void)size; return -1;
}
static double now_ms(void) {
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  long n;
  char *s;
  assert(f && fseek(f, 0, SEEK_END) == 0);
  n = ftell(f);
  assert(n >= 0 && fseek(f, 0, SEEK_SET) == 0);
  s = malloc((size_t)n + 1);
  assert(s && fread(s, 1, (size_t)n, f) == (size_t)n);
  s[n] = 0;
  fclose(f);
  return s;
}
int main(int argc, char **argv) {
  sqlite3 *db;
  char *css;
  int i, count, repeats;
  assert(argc >= 3);
  if (!strcmp(argv[1], "init")) {
    db = enil_db_open(argv[2]);
    assert(db && enil_db_create_tables(db) == SQLITE_OK);
    enil_db_close(db);
    return 0;
  }
  assert(argc == 8);
  assert(sqlite3_open_v2(argv[2], &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
  css = read_file(argv[3]);
  count = atoi(argv[5]); repeats = atoi(argv[6]);
  enil_html_set_font_dir(argv[7]);
  for (i = -2; i < repeats; i++) {
    long long oldest;
    int more;
    double start = now_ms();
    char *html = enil_account_chat_page_html(db, "u-benchmark", "self", css,
                                             "", count, &oldest, &more);
    double elapsed = now_ms() - start;
    assert(html && !more && oldest == 1700000000000LL);
    if (i >= 0) printf("%.6f\n", elapsed);
    if (i == repeats - 1) {
      FILE *f = fopen(argv[4], "wb");
      assert(f && fwrite(html, 1, strlen(html), f) == strlen(html));
      fclose(f);
    }
    free(html);
  }
  free(css);
  sqlite3_close(db);
  return 0;
}
