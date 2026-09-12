#ifndef ENIL_SYNC_H
#define ENIL_SYNC_H

#include <stddef.h>
#include "sqlite3.h"

/* Total number of phase rows the UI displays, including the trailing
 * "Complete" sentinel. The 6 work phases run in order; DONE is emitted
 * once at the end. */
#define ENIL_SYNC_PHASES 7

typedef enum {
  ENIL_SYNC_PHASE_CONNECTING          = 1,  /* token refresh + OBS token             */
  ENIL_SYNC_PHASE_ACCOUNT             = 2,  /* profile + friends + chats             */
  ENIL_SYNC_PHASE_MESSAGES            = 3,  /* recent messages per chat              */
  ENIL_SYNC_PHASE_DECRYPTING          = 4,  /* decrypt + REPLACE re-extract          */
  ENIL_SYNC_PHASE_PREPARING_DOWNLOADS = 5,  /* product + sticon download inventory   */
  ENIL_SYNC_PHASE_DOWNLOADING         = 6,  /* media + sticker image downloads       */
  ENIL_SYNC_PHASE_DONE                = 7,
} ENIL_SYNC_PHASE;

/* Progress is reported via enil_progress_post() → ENILSyncStatusNotification. */
void enil_sync_all(sqlite3    *db,
                   const char *access_token,
                   const char *my_mid,
                   const char *session_path);

/* out_handled (nullable): set to 1 if the dispatcher acted on the op,
 * 0 if it fell through to the unhandled default. Only meaningful for
 * "message" events. */
int enil_sync_process_sse_event(sqlite3    *db,
                                const char *access_token,
                                const char *my_mid,
                                const char *session_path,
                                const char *event_type,
                                const char *event_data,
                                int        *out_handled);

typedef struct {
  char message_id[128];
  char from_mid[256];
  char to_mid[256];
  int  to_type;
  int  op_type;     /* LINE operation type integer (25=SEND, 26=RECV, …) */
  int  has_message; /* 1 if the event contains a message object to render */
} ENILSSEMessageInfo;

int enil_sync_sse_talk_exception_code(const char *event_data);
int enil_sync_sse_message_info(const char *event_data,
                               ENILSSEMessageInfo *info);

/* Build appendMessage() JS for an incoming SSE message event.
   Fills chat_id_buf (if non-NULL) with the target chat id.
   Returns malloc'd JS string the caller must pass to enil_html_free(),
   or NULL if the message is not yet in the DB or is not a text/media/sticker. */
/* temp_id: if non-NULL emits updateMessage(temp_id,…); if NULL emits appendMessage(…) */
char *enil_sync_js_for_sse_message(sqlite3    *db,
                                    const char *my_mid,
                                    const char *event_data,
                                    const char *temp_id,
                                    char       *chat_id_buf,
                                    size_t      chat_id_size);

/* Build updateReactions() JS for an SSE reaction op (139/140). Resolves the
 * op's target message id (param1) to its current reactions_json + chat id.
 * Fills chat_id_buf (if non-NULL). Returns malloc'd JS (free via
 * enil_html_free), or NULL if the target message is not in the DB. */
char *enil_sync_js_for_sse_reaction(sqlite3    *db,
                                     const char *event_data,
                                     char       *chat_id_buf,
                                     size_t      chat_id_size);

/* Build markDeleted() JS for an SSE unsend op (64/65). param2 is the target
 * message id, param1 the chat mid; chat id is resolved from the DB row when
 * present, else falls back to param1. Fills chat_id_buf (if non-NULL).
 * Returns malloc'd JS (free via enil_html_free), or NULL if not an unsend
 * op / missing message id. */
char *enil_sync_js_for_sse_delete(sqlite3    *db,
                                   const char *event_data,
                                   char       *chat_id_buf,
                                   size_t      chat_id_size);

/* Build setReadMarker() JS for an SSE op 55 (NOTIFIED_READ_MESSAGE). Resolves
 * the chat's current peerLastReadTime to the corresponding outgoing message
 * id and emits a JS push so the marker hairline moves without a full reload.
 * 1:1 only; returns NULL for groups / non-55 ops. Fills chat_id_buf when
 * non-NULL. Caller frees the JS with enil_html_free. */
char *enil_sync_js_for_sse_read_receipt(sqlite3    *db,
                                         const char *my_mid,
                                         const char *event_data,
                                         char       *chat_id_buf,
                                         size_t      chat_id_size);

/* Group/room variant — emits setReadMarkerForReader(...) so the named pill
 * for op.param2 (the reader mid) moves to its new high-water-mark message
 * without a full reload. Returns NULL for 1:1 ops, non-op-55 events, or
 * readers with no resolvable outgoing message yet. */
char *enil_sync_js_for_sse_group_read_receipt(sqlite3    *db,
                                               const char *my_mid,
                                               const char *event_data,
                                               char       *chat_id_buf,
                                               size_t      chat_id_size);

/* Build setSeenMarker(...) JS for an SSE op 40 (SEND_CHAT_CHECKED) — this
 * account has seen the chat on another device. Applies the same unreadCount gate
 * as the renderer/blue dot: clears the gray "Seen" marker when caught up
 * (the normal case), else re-anchors it. Fills chat_id_buf (param1). Returns
 * NULL for non-op-40 events. Caller frees the JS with enil_html_free. */
char *enil_sync_js_for_sse_chat_checked(sqlite3    *db,
                                         const char *event_data,
                                         char       *chat_id_buf,
                                         size_t      chat_id_size);

#endif
