#ifndef ENIL_API_CALL_H
#define ENIL_API_CALL_H

#include "cJSON.h"
#include "enil_api_types.h"
#include "enil_line.h"

/* Build the request body as a cJSON value. Return NULL on failure.
 * Ownership of the returned cJSON is transferred to the dispatcher. */
typedef cJSON *(*enil_build_fn)(const void *req);

/* Parse the unwrapped response "data" field into out. Return 0 on success. */
typedef int (*enil_parse_fn)(cJSON *data, void *out);

/* Generic call dispatcher.
 *   endpoint     — descriptor (method, path, flags)
 *   access_token — required when endpoint->needs_auth is set
 *   req          — opaque pointer passed to build_fn (may be NULL)
 *   build_fn     — produces request body cJSON (may be NULL for empty body)
 *   out          — opaque pointer passed to parse_fn
 *   parse_fn     — extracts response.data into out
 *
 * Returns 0 on success, -1 on transport/HTTP/JSON error. */
int enil_api_call(
  const enil_endpoint_t *endpoint,
  const char            *access_token,
  const void            *req,
  enil_build_fn          build_fn,
  void                  *out,
  enil_parse_fn          parse_fn);

/* Validate the standard LINE envelope (HTTP 200 + {"message":"OK"}) on `r`
 * and detach its inner "data" object (caller owns the result), or NULL on any
 * failure. `tag` is the full log prefix (e.g. "TalkService.getChats"). This is
 * the one copy of the OK-envelope check; enil_api_call and the hand-rolled
 * TalkService wrappers both route through it. */
cJSON *enil_api_unwrap_ok(const char *tag, ENILLineResponse *r);

#endif
