/* Offline regression: nearby same-sender messages remain independent. */
#include "enil_html.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int enil_format_date_string(long long ms, char *out, size_t size) {
  (void)ms; (void)out; (void)size; return -1;
}

static int occurrences(const char *s, const char *needle) {
  int n = 0;
  while ((s = strstr(s, needle))) { n++; s += strlen(needle); }
  return n;
}

int main(void) {
  ENILHTMLMessage msgs[3];
  char *html, *js;
  int i;
  memset(msgs, 0, sizeof(msgs));
  for (i = 0; i < 3; i++) {
    msgs[i].message_id = i == 0 ? "a" : i == 1 ? "b" : "c";
    msgs[i].sender_name = "Alex";
    msgs[i].text = "Hello";
    msgs[i].created_at = 1700000000000LL + i * 1000;
  }
  msgs[0].reactions_json =
    "[{\"predefinedReactionType\":2},{\"predefinedReactionType\":2},"
    "{\"predefinedReactionType\":2}]";
  html = enil_html_page_with_css_more(msgs, 3, "", "", 0, 0,
                                      NULL, 0, "b");
  assert(html);
  assert(strstr(html, "width=device-width,initial-scale=1"));
  assert(!strstr(html, "document.documentElement.style.zoom"));
  assert(occurrences(html, "class=\"enil-message incoming run-solo\"") == 3);
  assert(occurrences(html, "class=\"enil-sender\"") == 3);
  assert(occurrences(html, "class=\"enil-time\"") == 3);
  assert(strstr(html, "data-msg-id=\"b\"><span class=\"enil-seen-label\""));
  assert(strstr(html, "class=\"rxn-count\">3</span>"));
  free(html);
  js = enil_html_js_prepend(msgs, 3, 0);
  assert(js && occurrences(js, "enil-message incoming run-solo") == 3);
  free(js);
  msgs[0].content_type = 1;
  msgs[0].thumb_path = "photo.png";
  msgs[0].thumb_width = 640;
  msgs[0].thumb_height = 480;
  js = enil_html_js_append(&msgs[0]);
  assert(js && strstr(js, "enil-photo-message"));
  assert(!strstr(js, "width:320px"));
  free(js);
  return 0;
}
