#include "enil_http.h"
#include "enil_identity.h"
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

const enil_identity_t *enil_identity_current(void) { return NULL; }
void enil_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
static volatile int cancelled;
static void *cancel_later(void *unused) {
  (void)unused;
  usleep(200000);
  cancelled = 1;
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t thread;
  int result;
  if (argc == 4) {
    enil_http_bind_cancel(&cancelled);
    if (pthread_create(&thread, NULL, cancel_later, NULL)) return 2;
    result = enil_curl_download_file(argv[1], argv[2]);
    pthread_join(thread, NULL);
    return result == 0 ? 0 : 1;
  }
  if (argc != 3) return 2;
  return enil_curl_download_file(argv[1], argv[2]) == 0 ? 0 : 1;
}
