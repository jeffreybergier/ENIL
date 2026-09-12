/* ============================================================================
 * enil_crypto — OBS file encryption / decryption (media uploads & downloads).
 *
 * X25519 keygen/derive, SHA-256, AES-GCM and Letter-Sealing v2 message
 * decryption used to live here; per CLAUDE.md the clean-room boundary keeps
 * all E2EE/keygen on the Cloudflare Worker, so those primitives were removed.
 * Only the OBS file channel (symmetric, key supplied by LINE) stays local.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include "enil_crypto.h"
#include "enil_cocoa_log.h"

#define ENIL_CRYPTO_FILE_NONCE_LEN 12
#define ENIL_CRYPTO_HKDF_FILE_LEN  76  /* enc_key 32 || mac_key 32 || nonce 12 */

#define LOG(fn, msg) enil_log("Crypto." fn, "%s", msg)

/* ============================================================================
 * OBS file encryption / decryption.
 * ==========================================================================*/

/* HKDF-SHA256 per RFC 5869, with empty salt → 32 zero bytes (WebCrypto). */
static int file_hkdf(const unsigned char *ikm, size_t ikm_len,
                     unsigned char *out, size_t out_len) {
  static const unsigned char kZeroSalt[32] = {0};
  static const char          kInfo[]       = "FileEncryption";
  const size_t info_len = sizeof(kInfo) - 1;
  unsigned char prk[32], t_prev[32], t[32];
  unsigned char block[32 + 16 + 1];
  unsigned int  len = 32;
  size_t        written = 0, prev_len = 0, input_len, to_copy;
  unsigned char counter;

  HMAC(EVP_sha256(), kZeroSalt, 32, ikm, ikm_len, prk, &len);
  for (counter = 1; written < out_len; counter++) {
    input_len = 0;
    len = 32;
    memcpy(block, t_prev, prev_len);          input_len += prev_len;
    memcpy(block + input_len, kInfo, info_len); input_len += info_len;
    block[input_len++] = counter;
    HMAC(EVP_sha256(), prk, 32, block, input_len, t, &len);
    memcpy(t_prev, t, 32); prev_len = 32;
    to_copy = out_len - written;
    if (to_copy > 32) to_copy = 32;
    memcpy(out + written, t, to_copy);
    written += to_copy;
  }
  return 0;
}

static void file_derive_keys(const unsigned char *km, size_t km_len,
                             unsigned char enc_key[32],
                             unsigned char mac_key[32],
                             unsigned char nonce[ENIL_CRYPTO_FILE_NONCE_LEN]) {
  unsigned char out[ENIL_CRYPTO_HKDF_FILE_LEN];
  file_hkdf(km, km_len, out, sizeof(out));
  memcpy(enc_key, out,      32);
  memcpy(mac_key, out + 32, 32);
  memcpy(nonce,   out + 64, ENIL_CRYPTO_FILE_NONCE_LEN);
}

/* AES-256-CTR. counter = nonce[12] || 0x00000000. Returns malloc'd output. */
static unsigned char *file_ctr_crypt(const unsigned char enc_key[32],
                                     const unsigned char nonce[ENIL_CRYPTO_FILE_NONCE_LEN],
                                     const unsigned char *in, size_t in_len) {
  unsigned char counter[16];
  unsigned char *out;
  EVP_CIPHER_CTX *ctx;
  int n = 0;

  memcpy(counter, nonce, ENIL_CRYPTO_FILE_NONCE_LEN);
  memset(counter + ENIL_CRYPTO_FILE_NONCE_LEN, 0,
         16 - ENIL_CRYPTO_FILE_NONCE_LEN);

  out = (unsigned char *)malloc(in_len > 0 ? in_len : 1);
  if (!out) return NULL;
  ctx = EVP_CIPHER_CTX_new();
  if (!ctx) { free(out); return NULL; }

  /* CTR is symmetric — EncryptInit_ex works for both directions. */
  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, enc_key, counter) != 1 ||
      EVP_EncryptUpdate(ctx, out, &n, in, (int)in_len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    free(out);
    return NULL;
  }
  EVP_CIPHER_CTX_free(ctx);
  return out;
}

int enil_crypto_file_encrypt_with_km(const unsigned char *plain, size_t plain_len,
                                     const unsigned char km[ENIL_CRYPTO_FILE_KM_LEN],
                                     unsigned char **enc_out, size_t *enc_len_out) {
  unsigned char enc_key[32], mac_key[32], nonce[ENIL_CRYPTO_FILE_NONCE_LEN];
  unsigned char *cipher = NULL;
  unsigned char *out    = NULL;
  unsigned int  tag_len = ENIL_CRYPTO_FILE_TAG_LEN;
  size_t        total;

  if (!km || !enc_out || !enc_len_out) {
    LOG("file_encrypt_with_km", "null argument"); return -1;
  }
  if (!plain && plain_len > 0) {
    LOG("file_encrypt_with_km", "null plain with len>0"); return -1;
  }
  file_derive_keys(km, ENIL_CRYPTO_FILE_KM_LEN, enc_key, mac_key, nonce);

  cipher = file_ctr_crypt(enc_key, nonce, plain, plain_len);
  if (!cipher) { LOG("file_encrypt_with_km", "AES-CTR encrypt failed"); return -1; }

  total = plain_len + ENIL_CRYPTO_FILE_TAG_LEN;
  out = (unsigned char *)malloc(total > 0 ? total : 1);
  if (!out) { LOG("file_encrypt_with_km", "alloc failed"); free(cipher); return -1; }

  memcpy(out, cipher, plain_len);
  free(cipher);
  HMAC(EVP_sha256(), mac_key, 32, out, plain_len,
       out + plain_len, &tag_len);

  *enc_out     = out;
  *enc_len_out = total;
  return 0;
}

int enil_crypto_file_encrypt(const unsigned char *plain, size_t plain_len,
                             unsigned char km_out[ENIL_CRYPTO_FILE_KM_LEN],
                             unsigned char **enc_out, size_t *enc_len_out) {
  if (!km_out) { LOG("file_encrypt", "null km_out"); return -1; }
  if (RAND_bytes(km_out, ENIL_CRYPTO_FILE_KM_LEN) != 1) {
    LOG("file_encrypt", "RAND_bytes failed"); return -1;
  }
  return enil_crypto_file_encrypt_with_km(plain, plain_len, km_out,
                                          enc_out, enc_len_out);
}

int enil_crypto_file_decrypt(const unsigned char *km, size_t km_len,
                             const unsigned char *enc, size_t enc_len,
                             unsigned char **plain_out, size_t *plain_len_out) {
  unsigned char enc_key[32], mac_key[32], nonce[ENIL_CRYPTO_FILE_NONCE_LEN];
  unsigned char actual[ENIL_CRYPTO_FILE_TAG_LEN];
  unsigned int  actual_len = ENIL_CRYPTO_FILE_TAG_LEN;
  unsigned char *plain;
  size_t ct_len;

  if (!km || !enc || !plain_out || !plain_len_out) {
    LOG("file_decrypt", "null argument"); return -1;
  }
  if (enc_len < ENIL_CRYPTO_FILE_TAG_LEN) {
    LOG("file_decrypt", "input shorter than HMAC tag"); return -1;
  }
  ct_len = enc_len - ENIL_CRYPTO_FILE_TAG_LEN;
  file_derive_keys(km, km_len, enc_key, mac_key, nonce);

  HMAC(EVP_sha256(), mac_key, 32, enc, ct_len, actual, &actual_len);
  if (memcmp(actual, enc + ct_len, ENIL_CRYPTO_FILE_TAG_LEN) != 0) {
    LOG("file_decrypt", "HMAC verification failed");
    return -1;
  }

  plain = file_ctr_crypt(enc_key, nonce, enc, ct_len);
  if (!plain) { LOG("file_decrypt", "AES-CTR decrypt failed"); return -1; }

  *plain_out     = plain;
  *plain_len_out = ct_len;
  return 0;
}
