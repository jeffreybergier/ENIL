#ifndef ENIL_WINDOWS_PROBE_H
#define ENIL_WINDOWS_PROBE_H
#include "enil_qrlogin.h"

/* Native Windows secure QR flow. Persists session.json in a dedicated durable
 * directory; ENILAccount handles activation. Existing tokens
 * return success without LINE requests. Runs on a background thread. */
int enil_windows_probe_run(const char *directory,
                           const enil_qrlogin_callbacks_t *callbacks);
#endif
