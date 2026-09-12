#ifndef ENIL_OBS_H
#define ENIL_OBS_H

#include <stddef.h>

/* Downloads a single OBS media file.
 *
 * session_path : path to session.json (reads encryptedAccessTokens["2"])
 * message_id   : LINE message ID string
 * sid          : contentMetadata.SID  (NULL → defaults to "m")
 * oid          : contentMetadata.OID  (NULL → defaults to message_id)
 * obs_pop      : contentMetadata.OBS_POP query param (NULL → omitted)
 * e2ee_version : contentMetadata.e2eeVersion (non-NULL → validate object)
 * enc_km       : contentMetadata.ENC_KM base64 key material (NULL → no decrypt)
 * plain_size   : expected plaintext byte count (0 → unknown)
 * dest_path    : file path to write bytes (parent dir must exist)
 *
 * Returns 0 on success, -1 on failure. */
int enil_obs_download_message(const char *session_path,
                               const char *message_id,
                               const char *sid,
                               const char *oid,
                               const char *obs_pop,
                               const char *e2ee_version,
                               const char *enc_km,
                               size_t plain_size,
                               const char *dest_path);

/* Upload bytes to OBS at a random reqid path, returning the server-assigned
 * OID. sid selects the upload endpoint:
 *   "emi" — encrypted image    (PR2 target)
 *   "emv" — encrypted video    (out of scope)
 *   "ema" — encrypted audio    (out of scope)
 *   "emf" — encrypted file     (out of scope)
 * On success *out_oid is set to a malloc'd null-terminated string the caller
 * must free(). Returns 0 on success, -1 on failure. */
int enil_obs_upload(const char           *session_path,
                    const unsigned char  *data,
                    size_t                len,
                    const char           *sid,
                    char                **out_oid);

/* Upload bytes to OBS at a caller-chosen OID. Used for thumbnail previews
 * where oid is "<media_oid>__ud-preview". Returns 0 on success, -1 on
 * failure. */
int enil_obs_upload_with_oid(const char          *session_path,
                              const unsigned char *data,
                              size_t               len,
                              const char          *sid,
                              const char          *oid);

#endif
