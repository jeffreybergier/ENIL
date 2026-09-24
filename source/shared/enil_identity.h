#ifndef ENIL_IDENTITY_H
#define ENIL_IDENTITY_H

#include "cJSON.h"

/* Exact session-owned identity and protocol selection. */
typedef struct {
  char profile_id[16];
  char transport[24];
  char application[160];
  char user_agent[256];
  char system_name[64];
  char model_name[64];
  char gateway_version[32];
  /* Native QR login preference; absent in older snapshots means false. */
  int auto_login_required;
} enil_identity_t;

#define ENIL_LINE_GATEWAY "https://line-chrome-gw.line-apps.com"
#define ENIL_LINE_ORIGIN "chrome-extension://ophjlpahpchlmihnnnihgmmeilfjmjjc"

int enil_identity_default(const char *profile_id, enil_identity_t *out);
/* Missing identity means the frozen legacy Chrome profile. An invalid or
 * unsupported identity is an error, never an implicit switch to Chrome. */
int enil_identity_parse(const cJSON *json, enil_identity_t *out);
cJSON *enil_identity_to_json(const enil_identity_t *identity);

/* Like the existing per-thread account health binding, this carries context
 * through token-only Talk helpers. Owns a COPY, freed on thread exit. Every
 * account operation binds before network work; an unbound request must fail.
 * NULL clears it. Binding failure also clears the previous identity. */
int enil_identity_bind(const enil_identity_t *identity);
const enil_identity_t *enil_identity_current(void);

#endif
