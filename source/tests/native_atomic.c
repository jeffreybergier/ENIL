#include "enil_atomic.h"
#include "enil_health.h"
#include <pthread.h>
#include <assert.h>
#include <sched.h>

static volatile int ready;
static int payload;
static void *writer(void *unused) {
  int i; (void)unused;
  for (i=1;i<=10000;i++) {
    while (enil_atomic_load(&ready)) sched_yield();
    payload=i;
    enil_atomic_store(&ready,1);
  }
  return NULL;
}
void enil_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
void enil_status_post(const char *key,const char *message,int n) {(void)key;(void)message;(void)n;}
void enil_status_post_error(const char *key,const char *message) {(void)key;(void)message;}
static void *health_writer(void *arg) {
  int i;
  enil_health_bind(arg);
  for(i=0;i<10000;i++) {
    enil_health_set_failure(ENIL_ERR_WORKER,"test");
    enil_health_set_failure(ENIL_ERR_LINE,"test");
    enil_health_clear_failure(ENIL_ERR_WORKER);
    enil_health_clear_failure(ENIL_ERR_LINE);
  }
  return NULL;
}
int main(void) {
  pthread_t a,b;
  enil_health_t *health=enil_health_create("shared");
  int i;
  assert(health && !pthread_create(&a,NULL,writer,NULL));
  for(i=1;i<=10000;i++) {
    while(!enil_atomic_load(&ready)) sched_yield();
    assert(payload==i);
    enil_atomic_store(&ready,0);
  }
  pthread_join(a,NULL);
  enil_health_bind(health);
  assert(!pthread_create(&a,NULL,health_writer,health));
  assert(!pthread_create(&b,NULL,health_writer,health));
  for(i=0;i<10000;i++) {
    int state=enil_health_any_failed();
    assert(state==0 || state==1);
  }
  pthread_join(a,NULL);pthread_join(b,NULL);
  assert(!enil_health_any_failed());
  enil_health_destroy(health);
  return 0;
}
