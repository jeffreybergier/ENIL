#ifndef ENIL_B64_H
#define ENIL_B64_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Standard base64 (RFC 4648 alphabet: A–Z a–z 0–9 '+' '/', '=' padding).
 *
 * Previously hand-rolled three times (enil_line.c, enil_obs.c, enil_sync.c);
 * centralised here so the encode/decode logic — which moves E2EE ciphertext —
 * cannot drift between call sites.
 * ==========================================================================*/

/* Encode `len` bytes into a malloc'd, NUL-terminated base64 string. Returns
 * NULL on allocation failure. Caller frees. */
char *enil_b64_encode(const unsigned char *data, size_t len);

/* Decode `in` into the caller-supplied `out`, which must hold at least
 * strlen(in) * 3 / 4 bytes. Decodes whole 4-character groups only (tolerant of
 * a missing final partial quantum). Returns the number of bytes written, or
 * -1 on an invalid character. */
int enil_b64_decode(const char *in, unsigned char *out);

/* Decode `in` into a freshly malloc'd buffer stored in *out (caller frees).
 * Requires a well-formed input whose length is a multiple of 4 ('='-padded).
 * Returns the decoded byte count, or -1 on malformed input / allocation
 * failure. */
int enil_b64_decode_alloc(const char *in, unsigned char **out);

#ifdef __cplusplus
}
#endif

#endif /* ENIL_B64_H */
