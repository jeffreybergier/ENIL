/* ============================================================================
 * Standard base64 encode/decode. See enil_b64.h for the API contract.
 *
 * Backed by OpenSSL's EVP base64 (the same library this codebase already uses
 * for AES-CTR / HMAC in enil_crypto.c) rather than a hand-rolled transform.
 *
 * EVP_EncodeBlock is a clean one-shot: it NUL-terminates and emits no line
 * breaks. EVP_DecodeBlock has one sharp edge that every correct caller must
 * handle: it always writes 3 * (n/4) bytes — including the zero bytes that '='
 * padding decodes to — and never reports the true plaintext length. We correct
 * for that exactly once, in evp_decode_exact, so the public decoders return the
 * real byte count.
 * ==========================================================================*/

#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include "enil_b64.h"

/* Decode `n4` base64 characters (must be a multiple of 4) from `in` into `out`,
 * then subtract the 1–2 '=' padding bytes EVP leaves in the count to recover
 * the true decoded length. `out` must hold at least 3 * (n4/4) bytes. Returns
 * the decoded byte count, or -1 on a malformed group. */
static int evp_decode_exact(const char *in, size_t n4, unsigned char *out) {
  int decoded;
  if (n4 == 0) return 0;
  decoded = EVP_DecodeBlock(out, (const unsigned char *)in, (int)n4);
  if (decoded < 0) return -1;
  if (in[n4 - 1] == '=') decoded--;
  if (in[n4 - 2] == '=') decoded--;
  return decoded;
}

char *enil_b64_encode(const unsigned char *data, size_t len) {
  size_t out_cap = ((len + 2) / 3) * 4 + 1;   /* 4 chars per 3 bytes + NUL */
  char *out = (char *)malloc(out_cap);
  if (!out) return NULL;
  /* EVP_EncodeBlock writes the NUL terminator itself and returns the length. */
  EVP_EncodeBlock((unsigned char *)out, data, (int)len);
  return out;
}

int enil_b64_decode(const char *in, unsigned char *out) {
  size_t len = in ? strlen(in) : 0;
  /* Decode whole 4-character groups only — tolerant of a trailing partial
   * quantum, matching the previous behaviour. */
  return evp_decode_exact(in, len - (len % 4), out);
}

int enil_b64_decode_alloc(const char *in, unsigned char **out) {
  size_t in_len = in ? strlen(in) : 0;
  unsigned char *buf;
  int decoded;
  if (in_len % 4 != 0) return -1;
  /* EVP_DecodeBlock writes 3 * (in_len/4) bytes before padding correction. */
  buf = (unsigned char *)malloc(in_len / 4 * 3 + 1);
  if (!buf) return -1;
  decoded = evp_decode_exact(in, in_len, buf);
  if (decoded < 0) { free(buf); return -1; }
  *out = buf;
  return decoded;
}
