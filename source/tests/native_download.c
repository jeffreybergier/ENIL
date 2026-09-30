#include "enil_http.h"
#include "enil_identity.h"
#include <stdarg.h>
#include <stdio.h>

const enil_identity_t *enil_identity_current(void) { return NULL; }
void enil_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  return enil_curl_download_file(argv[1], argv[2]) == 0 ? 0 : 1;
}
