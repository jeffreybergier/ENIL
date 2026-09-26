#ifndef ENIL_CHROME_GATEWAY_H
#define ENIL_CHROME_GATEWAY_H

#include "enil_identity.h"
#include "enil_line.h"

/* Chrome gateway wire implementation; enil_line_post_ex owns routing. */
ENILLineResponse enil_chrome_gateway_post(const enil_identity_t *identity,
                                          const char *path, const char *body,
                                          const char *access_token,
                                          const char *const *extra_headers,
                                          int extra_header_count,
                                          long timeout_ms,
                                          const volatile int *cancel);

#endif
