#ifndef ENIL_LTSM_H
#define ENIL_LTSM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Offline, synchronous crypto service. No network or Worker credentials are
 * used. operation and request_json use the existing Worker's POST contract
 * (/sign, /keygen, /e2ee/... and /transport/legy/...); no HTTP server is started.
 * Calls are serialized internally and may run on any background thread. Do not
 * call from a signal handler or reenter from a callback. No account/key state
 * is retained between calls: pass the returned workerRestoreState back just as
 * with the Worker. Inputs are borrowed only for this call.
 *
 * On success returns 1 and an owned UTF-8 JSON string in *response_json. Release
 * it with enil_ltsm_free(). On failure returns 0, sets *response_json to NULL,
 * and writes a non-sensitive diagnostic into error (if error_size > 0).
 * Authentication/decoding failures never return unauthenticated plaintext and
 * never fall back to a remote service. Maximum request size is 9 MiB.
 */
int enil_ltsm_request(const char *operation, const char *request_json,
                     char **response_json, char *error, size_t error_size);

/* Cancellable variant. cancel is borrowed, called on the calling thread while
 * waiting for serialization and periodically during WASM execution. It must be
 * nonblocking, must not reenter the library, and returns nonzero to cancel.
 * cancel_context must remain alive until this function returns. Cancellation
 * discards the entire request and returns 0 without publishing a response. */
typedef int (*ENILLTSMCancel)(void *cancel_context);
int enil_ltsm_request_ex(const char *operation, const char *request_json,
                        char **response_json, char *error, size_t error_size,
                        ENILLTSMCancel cancel, void *cancel_context);

/* Wipe and release a response returned by enil_ltsm_request; NULL is allowed. */
void enil_ltsm_free(char *response_json);

/* Build identity, not an availability/network check. Borrowed static string. */
const char *enil_ltsm_version(void);

#ifdef __cplusplus
}
#endif
#endif
