#ifndef ENIL_SSE_INTERNAL_H
#define ENIL_SSE_INTERNAL_H

#include <pthread.h>
#include <time.h>
#include "enil_sse.h"

/* Private state shared by the Chrome SSE lifecycle and native poller. */
struct ENILSSEClient {
  char          *access_token;
  char          *session_path;
  sqlite3       *db;
  enil_health_t *health;
  long long      local_rev;
  volatile int   stop;
  volatile int   reconnect;
  volatile int   interrupt;
  int            delivery_failed;
  volatile time_t last_activity;
  ENILSSEEventFn fn;
  void          *ctx;
  pthread_t      thread;
  int            thread_started;
};

long long enil_sse_event_revision(const char *type, const char *data);
int enil_sse_save_local_rev(ENILSSEClient *client, long long rev);
void enil_native_poll(ENILSSEClient *client);

#endif
