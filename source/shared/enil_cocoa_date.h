#ifndef ENIL_COCOA_DATE_H
#define ENIL_COCOA_DATE_H

#include <stddef.h>

/* Localized timestamp formatting via CFDateFormatter. Implemented in
 * enil_cocoa.m; pure-C header so portable callers stay framework-free. */

/* Format a LINE epoch-milliseconds timestamp as a localized
 * "<medium date> <short time>" string into the caller-provided buffer.
 * Returns 0 on success, -1 on invalid args or formatting failure. */
int enil_format_date_string(long long epoch_ms, char *out, size_t out_size);

#endif /* ENIL_COCOA_DATE_H */
