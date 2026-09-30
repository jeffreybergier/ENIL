/* Test the production credential snapshot while Preferences changes it. */
#include "../shared/enil_worker.c"
#include <assert.h>

void enil_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
void enil_health_clear_failure(enil_err_source_t source) {(void)source;}
static void *replace(void *unused) {
  int i;(void)unused;
  for(i=0;i<10000;i++) {
    enil_worker_set_credentials("https://one", "one");
    enil_worker_set_credentials("https://two", "two");
  }
  return NULL;
}
static void *read_pair(void *unused) {
  int i;(void)unused;
  for(i=0;i<10000;i++) {
    char *url,*header;
    assert(worker_credentials_copy("/sign",&url,&header));
    assert((!strcmp(url,"https://one/sign") && !strcmp(header,"X-Worker-Secret: one")) ||
           (!strcmp(url,"https://two/sign") && !strcmp(header,"X-Worker-Secret: two")));
    free(url);free(header);
  }
  return NULL;
}
int main(void) {
  pthread_t writer,reader;
  char *url,*header;
  char long_secret[1024];
  memset(long_secret,'x',sizeof(long_secret)-1);long_secret[1023]=0;
  enil_worker_set_credentials("https://one","one");
  assert(worker_credentials_copy("/sign",&url,&header));
  enil_worker_set_credentials(NULL,NULL);
  assert(!strcmp(url,"https://one/sign") && !strcmp(header,"X-Worker-Secret: one"));
  free(url);free(header);
  assert(!worker_credentials_copy("/sign",&url,&header) && !url && !header);
  enil_worker_set_credentials("https://one",long_secret);
  assert(worker_credentials_copy("/sign",&url,&header));
  assert(strlen(header)==strlen("X-Worker-Secret: ")+1023);
  free(url);free(header);
  enil_worker_set_credentials("https://one","one");
  assert(!pthread_create(&writer,NULL,replace,NULL));
  assert(!pthread_create(&reader,NULL,read_pair,NULL));
  pthread_join(writer,NULL);pthread_join(reader,NULL);
  enil_worker_set_credentials(NULL,NULL);
  return 0;
}
