#ifndef ENIL_LINE_H
#define ENIL_LINE_H

#include "cJSON.h"
#include "enil_api_types.h"

typedef struct {
  long  status;
  char *body; /* malloc'd; call enil_line_response_free when done */
} ENILLineResponse;

/* Process-global language selected by the app bundle (en or ja). Call once
 * at launch before starting network threads. Unsupported languages fall back
 * to English; NULL/empty leaves the current setting in place. This controls
 * both LINE language headers and the ShopService locale language. */
void enil_line_set_language(const char *language);
const char *enil_line_language(void);
struct curl_slist;
/* Append Accept-Language and X-LAL to a request's curl header list. */
struct curl_slist *enil_line_language_headers(struct curl_slist *headers);

/* Requires this operation's identity to be bound via enil_session_bind_identity
 * (or enil_identity_bind during QR login). Missing binding sends no request. */
ENILLineResponse enil_line_post(
  const char *path,
  const char *body,
  const char *access_token);

/* Extended POST: extra_headers is an array of "Name: value" strings (may be
 * NULL/0). timeout_ms is the overall transfer timeout (0 = no timeout — used
 * for the QR/PIN long-poll calls). cancel is an optional cooperative-cancel
 * flag (may be NULL): when it is non-NULL and becomes non-zero mid-flight, the
 * in-progress transfer is aborted within ~1s via libcurl's transfer-info
 * callback. A cancelled transfer returns status 0 / body NULL and, unlike a
 * real transport error, does NOT trip the sticky LINE health gate — it is a
 * user action, not a fault. enil_line_post is this with NULL/0/0/NULL. */
ENILLineResponse enil_line_post_ex(
  const char *path,
  const char *body,
  const char *access_token,
  const char *const *extra_headers,
  int                extra_header_count,
  long               timeout_ms,
  const volatile int *cancel);

void enil_line_response_free(ENILLineResponse *r);

/* Reads session_path, calls LINE /api/auth/tokenRefresh, writes updated
 * tokens back to session_path. Returns malloc'd new access token, or NULL.
 * If out_line_code is non-NULL, on failure it receives the LINE error code
 * from the response body (e.g. 10201 AUTH_INVALID_REQUEST = refresh token
 * is permanently dead; only a fresh QR scan can recover). 0 on success or
 * when no code could be parsed (network error, bad JSON, etc.). */
char *enil_line_token_refresh(const char *session_path, int *out_line_code);

/* Calls LINE acquireEncryptedAccessToken (scope=2) to obtain an OBS media
 * token, writes it to session_path as "obsToken", and returns it malloc'd.
 * Returns NULL on failure (non-fatal; OBS downloads simply won't work). */
char *enil_line_acquire_obs_token(const char *session_path,
                                   const char *access_token);

typedef struct {
  char           *message_id; /* convenience: == message->id; malloc'd */
  long long       created_at; /* convenience: == message->createdTime */
  talk_message_t *message;    /* full server response; NULL on parse failure.
                                 Owned — freed by enil_line_send_result_free. */
} ENILLineSendResult;

/* Encrypts text and sends it to chat_mid (u-prefix = 1:1, c-prefix = group).
 * Reads session_path for keys/tokens; updates reqSeq and workerRestoreState.
 * Returns 1 on success, 0 on failure. On success caller must call
 * enil_line_send_result_free. */
int  enil_line_send_text(const char *session_path,
                          const char *chat_mid,
                          const char *text,
                          ENILLineSendResult *out);

void enil_line_send_result_free(ENILLineSendResult *r);

typedef struct {
  const char *package_id;
  const char *sticker_id;
  const char *version; /* may be NULL */
  const char *option;  /* may be NULL */
  const char *hash;    /* may be NULL */
} ENILLineSendStickerParams;

/* Sends an unencrypted sticker message (contentType=7). */
int enil_line_send_sticker(const char *session_path,
                            const char *chat_mid,
                            const ENILLineSendStickerParams *sticker,
                            ENILLineSendResult *out);

typedef struct {
  const char *package_id;   /* productId in LINE API */
  const char *sticon_id;
  int         S;            /* UTF-8 byte start offset of marker in text */
  int         E;            /* UTF-8 byte end offset (exclusive) */
  const char *version;      /* may be NULL or "" */
  const char *resource_type; /* may be NULL; defaults to "STATIC" */
} ENILLineSticonResource;

/* Build a heap array of ENILLineSticonResource by scanning `text` left-to-right
 * for `$` (no alt) or `(altText)` markers. Caller frees with free(). Returns
 * NULL on alloc failure or invalid args.
 *
 * `resource_types` and `versions` are parallel, optional arrays: pass the
 * pack's resourceType string ("ANIMATION"/"STATIC") and latestVersion so an
 * animated sticon renders animated on real LINE clients. Either may be NULL
 * (and individual entries may be NULL), in which case the resource defaults to
 * STATIC / empty version. The struct stores the supplied pointers without
 * copying — they must outlive the subsequent send call (same contract as
 * package_ids / sticon_ids). */
ENILLineSticonResource *enil_line_build_sticon_resources(
    const char *text, int count,
    const char * const *package_ids,
    const char * const *sticon_ids,
    const char * const *alt_texts,
    const char * const *resource_types,
    const char * const *versions);

/* Wrapper for TalkService.sendChatRemoved. Loads session, bumps reqSeq, calls
 * the RPC, persists reqSeq. `last_read_message_id` may be NULL. Returns 1 on
 * success. */
int enil_line_send_chat_removed(const char *session_path,
                                 const char *chat_mid,
                                 const char *last_read_message_id,
                                 long long   last_read_message_time);

typedef struct {
  const unsigned char *jpeg_data;
  size_t               jpeg_len;
  int                  orig_width;
  int                  orig_height;
  const unsigned char *thumb_data;     /* may be NULL — thumbnail skipped */
  size_t               thumb_len;
  int                  thumb_width;
  int                  thumb_height;
  const char          *file_name;      /* e.g. "image.jpg"; defaults if NULL */
} ENILLineSendImageParams;

/* Send a plaintext image (contentType=1, no E2EE / flow=1 path). Posts the
 * sendMessage RPC first, then POSTs the JPEG bytes to /r/talk/m/<msgId> and
 * (best-effort) the thumbnail to /r/talk/m/<msgId>__ud-preview.
 * Returns 1 on success. On success the caller must call
 * enil_line_send_result_free. */
int enil_line_send_image_plain(const char                    *session_path,
                                const char                    *chat_mid,
                                const ENILLineSendImageParams *params,
                                ENILLineSendResult            *out);

/* Unified image send. Calls TalkService.determineMediaMessageFlow and picks
 * the E2EE (flow=2) or plaintext (flow=1) path. Default branch on probe
 * failure is plaintext. Returns 1 on success. */
int enil_line_send_image(const char                    *session_path,
                          const char                    *chat_mid,
                          const ENILLineSendImageParams *params,
                          ENILLineSendResult            *out);

/* Encrypts and sends an inline sticon message (contentType=0 with REPLACE metadata).
 * text: plain text with $ or (altText) markers at each sticon position.
 * resources: ordered array of sticon identities, one per marker.
 * Returns 1 on success. On success caller must call enil_line_send_result_free. */
int enil_line_send_inline_sticon(const char *session_path,
                                  const char *chat_mid,
                                  const char *text,
                                  const ENILLineSticonResource *resources,
                                  int resource_count,
                                  ENILLineSendResult *out);

/* LINE mid classification. The kind is encoded in the mid's first character
 * (case-insensitive): u=1:1 user, c=group, r=room, s=square. This is the
 * single source of truth for the prefix rule — do not re-derive it inline. */
typedef enum {
  ENIL_MID_UNKNOWN = 0,
  ENIL_MID_USER,   /* u — 1:1 chat */
  ENIL_MID_GROUP,  /* c — group chat */
  ENIL_MID_ROOM,   /* r — multi-user room */
  ENIL_MID_SQUARE  /* s — OpenChat square */
} enil_mid_kind_t;

enil_mid_kind_t enil_mid_kind(const char *mid);

/* 1 if the mid is a 1:1 user chat, 0 otherwise (group/room/square/unknown). */
int enil_mid_is_one_to_one(const char *mid);

/* LINE message toType for a destination mid: 0 for 1:1, 2 otherwise. */
int enil_mid_to_type(const char *mid);

#endif
