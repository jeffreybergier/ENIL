/* ============================================================================
 * enil_crypto — OBS file encryption (media uploads / downloads).
 *
 * X25519/SHA-256/AES-GCM/Letter-Sealing primitives were removed: per
 * CLAUDE.md the clean-room boundary keeps all E2EE/keygen on the Cloudflare
 * Worker. Only the symmetric OBS file channel (key supplied by LINE) is local.
 * ==========================================================================*/

#ifndef ENIL_CRYPTO_H
#define ENIL_CRYPTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * OBS file encryption (used for media uploads / downloads).
 *
 * Wire layout:    AES-256-CTR(enc_key, counter=nonce||0^4, plain) ||
 *                 HMAC-SHA256(mac_key, ciphertext)
 * Key derivation: HKDF-SHA256(salt=0^32, IKM=keyMaterial,
 *                             info="FileEncryption", 76 bytes)
 *                   → enc_key[32] || mac_key[32] || nonce[12]
 * keyMaterial:    32 random bytes. Stored as base64 in
 *                 contentMetadata.ENC_KM on the LINE message.
 * ==========================================================================*/

#define ENIL_CRYPTO_FILE_KM_LEN  32
#define ENIL_CRYPTO_FILE_TAG_LEN 32

/* Encrypt `plain` for the OBS channel. Generates a fresh random 32-byte
 * keyMaterial, writes it to km_out, then produces *enc_out (malloc'd) as
 * ciphertext || HMAC tag. Caller frees *enc_out.
 * Returns 0 on success, -1 on failure. */
int enil_crypto_file_encrypt(const unsigned char *plain, size_t plain_len,
                             unsigned char km_out[ENIL_CRYPTO_FILE_KM_LEN],
                             unsigned char **enc_out, size_t *enc_len_out);

/* Same as enil_crypto_file_encrypt but the caller supplies the keyMaterial
 * (used to encrypt a thumbnail with the same key as the main image). */
int enil_crypto_file_encrypt_with_km(const unsigned char *plain, size_t plain_len,
                                     const unsigned char km[ENIL_CRYPTO_FILE_KM_LEN],
                                     unsigned char **enc_out, size_t *enc_len_out);

/* Decrypt the inverse. km is the raw (already base64-decoded) keyMaterial;
 * enc is ciphertext || HMAC tag. *plain_out is malloc'd; caller frees.
 * Returns 0 on success, -1 on failure (bad HMAC, alloc, etc.). */
int enil_crypto_file_decrypt(const unsigned char *km, size_t km_len,
                             const unsigned char *enc, size_t enc_len,
                             unsigned char **plain_out, size_t *plain_len_out);

#ifdef __cplusplus
}
#endif

#endif
