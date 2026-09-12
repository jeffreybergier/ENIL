#ifndef ENIL_WORKER_H
#define ENIL_WORKER_H

#include "cJSON.h"

/* Process-global worker endpoint + shared secret. Set once during app startup
 * (from ENILKeychain) and any time Preferences saves a new value. Both must
 * be non-empty before any worker call; an unset client logs and returns
 * NULL/0 from every public entry point. Strings are duplicated internally
 * so callers retain ownership of their inputs. */
void   enil_worker_set_credentials(const char *url, const char *secret);

/* Returns a malloc'd HMAC string or NULL on error. Caller must free. */
char  *enil_worker_sign(const char *path, const char *body, const char *access_token);

/* Posts payload JSON to a worker e2ee path. Returns parsed response or NULL.
 * Caller must cJSON_Delete the result. */
cJSON *enil_worker_decrypt(const char *path, cJSON *payload);

/* Returned by enil_worker_encrypt_user / enil_worker_encrypt_group. */
typedef struct {
  char *ciphertext;           /* base64; free with enil_worker_encrypt_free */
  char *worker_restore_state; /* JSON string; free with enil_worker_encrypt_free */
} ENILWorkerEncryptResult;

typedef struct {
  const char *worker_restore_state; /* JSON string, may be NULL */
  const char *private_key;          /* base64 */
  int         private_key_id;
  const char *peer_mid;
  const char *peer_public_key;      /* base64 */
  int         peer_key_id;
  const char *from_mid;
  const char *to_mid;
  int         sender_key_id;
  int         content_type;
  long long   sequence_number;
  const char *plaintext;            /* base64-encoded JSON payload */
} ENILWorkerEncryptUserParams;

typedef struct {
  const char *worker_restore_state; /* JSON string, may be NULL */
  const char *group_mid;
  cJSON      *group_key;            /* caller owns; must not be NULL */
  const char *my_private_key;       /* base64 */
  int         my_private_key_id;
  const char *sender_mid;
  int         sender_key_id;
  int         group_key_id;
  const char *creator_public_key;   /* base64 */
  const char *sender_public_key;    /* base64 */
  const char *from_mid;
  const char *to_mid;               /* == group_mid */
  int         content_type;
  long long   sequence_number;
  const char *plaintext;            /* base64-encoded JSON payload */
} ENILWorkerEncryptGroupParams;

/* Returns 1 on success, 0 on failure. On success caller must call
 * enil_worker_encrypt_free on the result. */
int  enil_worker_encrypt_user(const ENILWorkerEncryptUserParams *p,
                               ENILWorkerEncryptResult *out);
int  enil_worker_encrypt_group(const ENILWorkerEncryptGroupParams *p,
                                ENILWorkerEncryptResult *out);
void enil_worker_encrypt_free(ENILWorkerEncryptResult *r);

/* ---- QR login: portable Curve25519 keygen (POST /keygen) ---------------- */
typedef struct {
  int    key_id;
  char  *public_key;           /* base64; free with enil_worker_keygen_free */
  char  *worker_restore_state; /* JSON string; free with enil_worker_keygen_free */
} ENILWorkerKeygenResult;

/* Generates a portable Curve25519 key for QR login. worker_restore_state may
 * be NULL/empty for a fresh session. Returns 1 on success; on success the
 * caller must call enil_worker_keygen_free. */
int  enil_worker_keygen(const char *worker_restore_state,
                         ENILWorkerKeygenResult *out);
void enil_worker_keygen_free(ENILWorkerKeygenResult *r);

/* ---- QR login: E2EE keychain unwrap (POST /e2ee/unwrap-keychain) -------- */
typedef struct {
  cJSON *keys;                 /* array of {keyId:int, exportedKey:string} */
  char  *worker_restore_state; /* JSON string */
} ENILWorkerUnwrapResult;

/* Unwraps the encrypted E2EE keychain returned by LINE after a QR scan.
 * peer_public_key / encrypted_keychain are base64. Returns 1 on success;
 * on success the caller must call enil_worker_unwrap_keychain_free. */
int  enil_worker_unwrap_keychain(const char *worker_restore_state,
                                  int         qr_key_id,
                                  const char *peer_public_key,
                                  const char *encrypted_keychain,
                                  ENILWorkerUnwrapResult *out);
void enil_worker_unwrap_keychain_free(ENILWorkerUnwrapResult *r);

/* Result from enil_worker_create_group_key.
 * enc_keys is a cJSON array of {mid, keyId, encryptedKey} objects. */
typedef struct {
  char  *worker_restore_state;
  cJSON *enc_keys;
} ENILWorkerCreateGroupKeyResult;

/* Generates a random group shared key and encrypts it for each member.
 * members: cJSON array of {mid, keyId, publicKey} objects (caller owns).
 * Returns 1 on success; caller must call enil_worker_create_group_key_free. */
int  enil_worker_create_group_key(const char *worker_restore_state,
                                   const char *my_private_key,
                                   int         my_private_key_id,
                                   cJSON      *members,
                                   ENILWorkerCreateGroupKeyResult *out);
void enil_worker_create_group_key_free(ENILWorkerCreateGroupKeyResult *r);

#endif
