/* Offline public API and real enil_worker facade tests. No sockets, accounts,
 * platform UI, or Worker credentials. Runs on the same native slices as ENIL. */
#include "enil_ltsm.h"
#include "enil_worker.h"
#include "enil_health.h"
#include "enil_cocoa_log.h"
#include "enil_cocoa_progress.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char kRequest[]="{\"path\":\"/S4\",\"body\":\"{}\",\"accessToken\":\"synthetic-token\"}";
static const char kSignature[]="k2qU/iyTBsbcBVr82398TrK7jFI1zC5yADh7OyGmb30=";
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); return 2; } } while(0)

void enil_log(const char *tag,const char *fmt,...) { (void)tag; (void)fmt; }
void enil_status_post(const char *key,const char *message,int indeterminate) {
  (void)key; (void)message; (void)indeterminate;
}
void enil_status_post_error(const char *key,const char *message) { (void)key; (void)message; }

static int signing_ok(void) {
  char *out=NULL,error[160]; int ok;
  ok=enil_ltsm_request("/sign",kRequest,&out,error,sizeof(error));
  ok=ok && out && strstr(out,kSignature)!=NULL;
  enil_ltsm_free(out); return ok;
}
static int cancel_immediately(void *p) { (void)p; return 1; }
static int cancel_during_wasm(void *p) { return ++*(int *)p>=3; }
static void *sign_thread(void *p) {
  int i,*ok=(int *)p; *ok=1;
  for(i=0;i<5;++i) if(!signing_ok()) *ok=0;
  return NULL;
}
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int entered,release,calls,ok;
} HeldCall;
/* Deliberately stall a test callback to exercise cancellation of another
 * thread waiting for the library lock; production callbacks never block. */
static int hold_for_waiter(void *p) {
  HeldCall *h=(HeldCall *)p;
  if(++h->calls==2) {
    pthread_mutex_lock(&h->mutex); h->entered=1;
    pthread_cond_broadcast(&h->condition);
    while(!h->release) pthread_cond_wait(&h->condition,&h->mutex);
    pthread_mutex_unlock(&h->mutex);
  }
  return 0;
}
static void *held_thread(void *p) {
  HeldCall *h=(HeldCall *)p; char *out=NULL,error[160];
  h->ok=enil_ltsm_request_ex("/sign",kRequest,&out,error,sizeof(error),hold_for_waiter,h);
  enil_ltsm_free(out); return NULL;
}
static int cancel_waiter(void *p) { return ++*(int *)p>=2; }

int main(void) {
  char *out=NULL,error[160],*signature,*large; int i,calls,ok[4],cancelled;
  pthread_t threads[4],held; HeldCall hold;
  cJSON *payload,*response; ENILWorkerKeygenResult generated;
  CHECK(signing_ok());
  CHECK(!enil_ltsm_request("/sign","{} trailing",&out,error,sizeof(error)) && !out);
  CHECK(!enil_ltsm_request("/sign",NULL,&out,error,sizeof(error)) && !out);
  CHECK(!enil_ltsm_request("/sign",kRequest,NULL,error,sizeof(error)));
  large=(char *)malloc(9U*1024U*1024U+2); CHECK(large);
  memset(large,'x',9U*1024U*1024U+1); large[9U*1024U*1024U+1]=0;
  calls=!enil_ltsm_request("/sign",large,&out,error,sizeof(error)) && !out;
  free(large); CHECK(calls && !strcmp(error,"invalid LTSM request"));
  CHECK(!enil_ltsm_request_ex("/sign",kRequest,&out,error,sizeof(error),cancel_immediately,NULL) && !out);
  calls=0;
  CHECK(!enil_ltsm_request_ex("/sign",kRequest,&out,error,sizeof(error),cancel_during_wasm,&calls) && !out);
  CHECK(calls>=3 && signing_ok());
  for(i=0;i<4;++i) CHECK(pthread_create(&threads[i],NULL,sign_thread,&ok[i])==0);
  for(i=0;i<4;++i) { CHECK(pthread_join(threads[i],NULL)==0); CHECK(ok[i]); }
  memset(&hold,0,sizeof(hold));
  CHECK(pthread_mutex_init(&hold.mutex,NULL)==0);
  CHECK(pthread_cond_init(&hold.condition,NULL)==0);
  CHECK(pthread_create(&held,NULL,held_thread,&hold)==0);
  pthread_mutex_lock(&hold.mutex);
  while(!hold.entered) pthread_cond_wait(&hold.condition,&hold.mutex);
  pthread_mutex_unlock(&hold.mutex); calls=0;
  CHECK(!enil_ltsm_request_ex("/sign",kRequest,&out,error,sizeof(error),cancel_waiter,&calls) && !out);
  CHECK(calls>=2);
  pthread_mutex_lock(&hold.mutex); hold.release=1;
  pthread_cond_broadcast(&hold.condition); pthread_mutex_unlock(&hold.mutex);
  CHECK(pthread_join(held,NULL)==0 && hold.ok);
  pthread_cond_destroy(&hold.condition); pthread_mutex_destroy(&hold.mutex);

  /* The actual app facade must work with no URL/secret and preserve health and
   * cancellation semantics. Linking this executable requires no curl library. */
  CHECK(enil_worker_is_local()); enil_worker_set_credentials(NULL,NULL);
  signature=enil_worker_sign("/S4","{}","synthetic-token");
  CHECK(signature && !strcmp(signature,kSignature)); free(signature);
  CHECK(enil_worker_keygen(NULL,&generated));
  CHECK(generated.key_id>0 && generated.public_key && generated.worker_restore_state);
  enil_worker_keygen_free(&generated);
  payload=cJSON_CreateObject(); CHECK(payload); cancelled=1;
  response=enil_worker_decrypt_ex("/keygen",payload,&cancelled);
  CHECK(!response && !enil_health_is_failed(ENIL_ERR_WORKER));
  CHECK(!enil_worker_decrypt("/unknown",payload));
  CHECK(enil_health_is_failed(ENIL_ERR_WORKER));
  CHECK(!enil_worker_sign("/S4","{}","synthetic-token"));
  enil_health_clear_failure(ENIL_ERR_WORKER);
  response=enil_worker_decrypt("/keygen",payload); CHECK(response);
  cJSON_Delete(response); cJSON_Delete(payload);
  enil_worker_set_credentials(NULL,NULL);
  puts("PASS native API: threads, cancellation during execution/lock wait, failure recovery, credential-free app facade and sticky gate");
  return 0;
}
