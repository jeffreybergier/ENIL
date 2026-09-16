#ifndef ENIL_SSE_H
#define ENIL_SSE_H

#include <sqlite3.h>
#include "enil_health.h"

typedef struct ENILSSEClient ENILSSEClient;

typedef struct {
  const char *type; /* "message", "ping", "fullSync", etc. */
  const char *data; /* raw JSON string, may be empty */
} ENILSSEEvent;

typedef void (*ENILSSEEventFn)(const ENILSSEEvent *ev, void *ctx);

/* access_token/session_path are copied; local_rev seeds the localRev query
 * param. db (not owned — must outlive the client) is where advancing localRev
 * values are persisted; session_path supplies the client identity and
 * lastPartialFullSyncs query param. health is the owning account's health object (not owned/copied —
 * must outlive the client); the SSE thread binds it so its reconnect gate and
 * any LINE work it drives are scoped to that account. May be NULL. */
ENILSSEClient *enil_sse_create(const char *access_token, long long local_rev,
                               const char *session_path, sqlite3 *db,
                               enil_health_t *health);
int            enil_sse_start(ENILSSEClient *c, ENILSSEEventFn fn, void *ctx);
/* Force an immediate reconnect of a running stream. Non-blocking; safe on NULL
 * and from any thread. See enil_sse.c for the rationale (wake recovery). */
void           enil_sse_kick(ENILSSEClient *c);
void           enil_sse_free(ENILSSEClient *c);

#endif
