#ifndef ENIL_DB_H
#define ENIL_DB_H

#include <sqlite3.h>
#include "cJSON.h"
#include "enil_api_types.h"

typedef enum {
  ENIL_DECRYPT_PENDING   = 0,
  ENIL_DECRYPT_PLAINTEXT = 1,
  ENIL_DECRYPT_SUCCESS   = 2,
  ENIL_DECRYPT_FAILED    = 3,
  ENIL_DECRYPT_NOT_TEXT  = 4,
} ENILDecryptStatus;

sqlite3 *enil_db_open(const char *path);
void     enil_db_close(sqlite3 *db);
int      enil_db_create_tables(sqlite3 *db);

/* db_meta — local "state of this database" (NOT mirrored from LINE). The SSE
 * cursor localRev lives here: it describes how far this DB has been advanced,
 * which is database state, not session/auth state. Mirrors the old
 * enil_session_*_local_rev API so the swap is mechanical:
 *   get → 0 when absent or non-positive
 *   has → 1 only when a value has ever been persisted (distinguishes
 *         "never synced" from "synced to revision 0")
 *   set → SQLITE_OK on success (DB-layer convention; NOT the session 1/0). */
long long enil_db_get_local_rev(sqlite3 *db);
int       enil_db_has_local_rev(sqlite3 *db);
int       enil_db_set_local_rev(sqlite3 *db, long long local_rev);

int    enil_db_talk_profile_upsert(sqlite3 *db, const talk_profile_t *p);
int    enil_db_talk_profile_get(sqlite3 *db, talk_profile_t *out);

int    enil_db_talk_contact_upsert(sqlite3 *db, const talk_contact_t *c);
int    enil_db_talk_contact_get_all(sqlite3 *db, talk_contact_t **out, int *out_count);
/* 1 if a row exists for the mid (or on error), 0 if definitively absent. */
int    enil_db_chat_exists(sqlite3 *db, const char *chat_mid);
int    enil_db_message_box_exists(sqlite3 *db, const char *chat_mid);
int    enil_db_contact_exists(sqlite3 *db, const char *mid);

int    enil_db_chat_upsert(sqlite3 *db, const talk_chat_t *c);

int    enil_db_message_box_upsert(sqlite3 *db, const talk_message_box_t *mb);
/* Live SSE SEND/RECEIVE update for one message. Unlike the full-sync upsert,
 * incoming peer messages increment the current unreadCount instead of replacing
 * it with 1; duplicate message ids do not increment. Outgoing messages clear
 * unreadCount, matching the existing "sent means seen" behavior. */
int    enil_db_message_box_upsert_live_message(sqlite3 *db,
                                               const char *chat_mid,
                                               int mid_type,
                                               const char *last_message_id,
                                               long long last_message_time,
                                               int incoming,
                                               const char *message_id);
int    enil_db_message_box_get_all(sqlite3 *db, talk_message_box_t **out, int *out_count);

int    enil_db_chat_summary_get_all(sqlite3 *db, chat_summary_t **out, int *out_count);
int    enil_db_chat_summary_get_one(sqlite3 *db, const char *chatMid, chat_summary_t *out);

int    enil_db_message_upsert(sqlite3 *db, const talk_message_t *m);

/* Typed row reader for the chat WebView. Enriches each row with is_outgoing
 * (vs my_mid), text_html (rendered), and sticker_path/width/height. Caller
 * receives an array of message_row_t to be freed via message_row_free + the
 * outer array.
 *
 * One page of messages older than `before` (0 = newest), ascending.
 * *out_has_more = 1 if older history remains beyond this page. */
int    enil_db_message_rows_get_page(sqlite3 *db, const char *chat_id,
                                     const char *my_mid, long long before,
                                     int limit, message_row_t **out,
                                     int *out_count, int *out_has_more);

cJSON *enil_db_get_message_by_id(sqlite3 *db, const char *message_id);

/* Returns malloc'd "avatars/<from_mid>.jpg" if that file exists under the
 * DB's directory; otherwise malloc'd "". Returns NULL only on alloc
 * failure. Caller frees. Used by both the full-page render and the SSE
 * incremental-append path so the avatar logic stays in one place. */
char  *enil_db_avatar_relpath(sqlite3 *db, const char *from_mid);
cJSON *enil_db_get_messages_needing_decrypt(sqlite3 *db);
cJSON *enil_db_get_messages_needing_replace(sqlite3 *db);
cJSON *enil_db_get_message_sticons(sqlite3 *db, const char *message_id);
int    enil_db_set_message_text(sqlite3 *db, const char *message_id,
                                const char *text, ENILDecryptStatus status);
int    enil_db_set_message_plaintext(sqlite3 *db, const char *message_id,
                                     const char *text, const char *raw_json,
                                     const char *replace_json,
                                     ENILDecryptStatus status);
int    enil_db_set_message_raw_json(sqlite3 *db, const char *message_id,
                                    const char *raw_json, const char *replace_json);
int    enil_db_set_message_enc_km(sqlite3 *db, const char *message_id,
                                   const char *enc_km);

/* Mark a single message row is_deleted=1 (unsend). */
int    enil_db_message_mark_deleted(sqlite3 *db, const char *message_id);

/* Apply one SSE op-139/op-140 reaction event. is_remove=1 drops the reactor's
 * entry. Otherwise either predefined_type (2..7 for built-in NICE/LOVE/FUN/
 * AMAZING/SAD/OMG) or product_id+emoji_id (paid sticonshop reaction) is set
 * on the new entry. Stored shape is a JSON array, NULL when empty. */
int    enil_db_message_reactions_update(sqlite3    *db,
                                        const char *message_id,
                                        const char *reactor_mid,
                                        int         is_remove,
                                        int         predefined_type,
                                        const char *product_id,
                                        const char *emoji_id,
                                        long long   created_time);

/* Clear unread count and set lastSeenMessageId on the message_box row. */
int    enil_db_message_box_mark_seen(sqlite3 *db, const char *chat_mid,
                                      const char *message_id);

/* Record a peer's read-up-to timestamp from SSE op 55. */
int    enil_db_message_box_set_peer_read(sqlite3 *db, const char *chat_mid,
                                          const char *reader_mid,
                                          long long read_up_to_time);

/* Returns peerLastReadTime in ms for a chat (the high-water-mark of the
 * peer's read receipts). 0 if the row is missing or the column is NULL.
 * Meaningful only for 1:1 chats — in groups the column is overwritten by
 * whichever member read last. */
long long enil_db_message_box_get_peer_last_read_time(sqlite3 *db,
                                                       const char *chat_mid);

/* Returns malloc'd id of the last outgoing, non-deleted message in `chat_mid`
 * whose createdTime is <= the chat's peerLastReadTime, or NULL if none. Used
 * by the read-marker push path to resolve "where should the line sit now". */
char *enil_db_last_read_outgoing_msg_id(sqlite3 *db, const char *chat_mid,
                                         const char *my_mid);

/* Per-member group read state. The pair (reader_mid, read_time) is the
 * reader's high-water-mark; rows are overwritten in place via INSERT OR
 * REPLACE with a monotonicity guard so out-of-order SSE events are dropped.
 * Caller must restrict use to c/C/r/R-prefixed chats — 1:1 chats use the
 * peerLastRead* columns on message_boxes_v2. */
int    enil_db_message_box_set_group_reader(sqlite3 *db, const char *chat_mid,
                                             const char *reader_mid,
                                             long long read_up_to_time);

/* One reader's resolved view: mid, display name from contacts_v2, and the
 * id of the last outgoing message they've read. Strings are owned by the
 * struct; free with enil_group_reader_free / enil_group_readers_free. */
typedef struct {
  char *reader_mid;
  char *display_name;
  char *last_read_message_id;
} enil_group_reader_t;

void enil_group_reader_free(enil_group_reader_t *r);
void enil_group_readers_free(enil_group_reader_t *arr, int count);

/* Return every reader of `chat_mid` (excluding `my_mid`) along with the id
 * of the last outgoing message they've read. Readers with no resolvable
 * message (no outgoing message at-or-before their readTime) are skipped.
 * Caller frees the result with enil_group_readers_free. */
int    enil_db_message_box_get_group_readers(sqlite3 *db, const char *chat_mid,
                                              const char *my_mid,
                                              enil_group_reader_t **out,
                                              int *out_count);

/* Single-reader fast path for the SSE op-55 group push. Always returns
 * SQLITE_OK on a successful query; *out is zeroed when the reader has no
 * resolvable read message yet (caller checks out->last_read_message_id). */
int    enil_db_message_box_get_group_reader(sqlite3 *db, const char *chat_mid,
                                             const char *reader_mid,
                                             const char *my_mid,
                                             enil_group_reader_t *out);

/* Return the server-authoritative lastDeliveredMessageId for a chat, or NULL
 * if unknown. Caller must free(). */
char  *enil_db_message_box_last_delivered(sqlite3 *db, const char *chat_mid);

/* Return the account's OWN lastSeenMessageId for a chat (read position), or
 * NULL if unknown. Drives the gray "Seen" marker. Caller must free(). */
char  *enil_db_message_box_last_seen(sqlite3 *db, const char *chat_mid);

/* Server-authoritative unread count for a chat (0 = caught up / missing). The
 * shared "caught up?" signal behind both the blue dot and the "Seen" marker. */
int    enil_db_message_box_unread_count(sqlite3 *db, const char *chat_mid);

/* Delete chats_v2 + message_boxes_v2 + messages_v2 (and children) for a chat. */
int    enil_db_chat_delete(sqlite3 *db, const char *chat_mid);

/* Returns scalar media metadata. raw_json is intentionally not included. */
cJSON *enil_db_get_messages_needing_media(sqlite3 *db);
/* Same shape for a single message (by id), or NULL if no such row. */
cJSON *enil_db_get_message_media_info(sqlite3 *db, const char *message_id);
int    enil_db_set_media_info(sqlite3 *db, const char *message_id,
                               const char *media_path,
                               int orig_width, int orig_height,
                               const char *thumb_path,
                               int thumb_width, int thumb_height);

/* Stickers / Sticons (v2 typed schema) */
int    enil_db_sticker_package_upsert  (sqlite3 *db, const sticker_package_t *p);
int    enil_db_sticker_package_discover(sqlite3 *db, const char *id);

int    enil_db_sticon_package_upsert  (sqlite3 *db, const sticon_package_t *p);
int    enil_db_sticon_package_discover(sqlite3 *db, const char *id);

cJSON *enil_db_sticon_packages_needing_meta(sqlite3 *db);
cJSON *enil_db_sticon_packages_needing_alt (sqlite3 *db);
/* Per-pack meta state: 0 none, 1 owned-needs-expand, 2 discovered-needs-alt. */
int    enil_db_sticon_package_meta_state(sqlite3 *db, const char *pkg_id);
int    enil_db_set_sticon_alt_text(sqlite3 *db, const char *sticon_id,
                                    const char *package_id,
                                    const char *alt_text);
int    enil_db_sticon_package_set_resource_type(sqlite3 *db, const char *id,
                                                 int resource_type);

int    enil_db_sticker_upsert(sqlite3 *db, const char *sticker_id,
                               const char *package_id,
                               const char *option, const char *hash);
int    enil_db_sticon_upsert (sqlite3 *db, const char *sticon_id,
                               const char *package_id, const char *alt_text);
cJSON *enil_db_get_stickers_needing_download(sqlite3 *db, int purchased);
cJSON *enil_db_get_sticons_needing_download (sqlite3 *db, int purchased);
int enil_db_sticker_rows_get(sqlite3 *db, sticker_row_t **out, int *out_count);
int enil_db_sticon_rows_get(sqlite3 *db, sticon_row_t **out, int *out_count);
int    enil_db_set_sticker_image_path(sqlite3 *db, const char *sticker_id,
                                       const char *package_id,
                                       const char *image_path,
                                       int image_width, int image_height);
int    enil_db_set_sticker_thumb_path(sqlite3 *db, const char *sticker_id,
                                       const char *package_id,
                                       const char *thumb_path,
                                       int thumb_width, int thumb_height);
int    enil_db_set_sticon_image_path(sqlite3 *db, const char *sticon_id,
                                      const char *package_id,
                                      const char *image_path,
                                      int image_width, int image_height);
int    enil_db_set_sticon_thumb_path(sqlite3 *db, const char *sticon_id,
                                      const char *package_id,
                                      const char *thumb_path,
                                      int thumb_width, int thumb_height);
int enil_db_get_sticker_image_info(sqlite3 *db, const char *sticker_id,
                                    char *buf, size_t buf_size,
                                    int *image_width, int *image_height);
int enil_db_get_sticker_image_info_for_package(sqlite3 *db,
                                                const char *package_id,
                                                const char *sticker_id,
                                                char *buf, size_t buf_size,
                                                int *image_width,
                                                int *image_height);

/* LINE resourceType enum (shared by stickerResourceType / sticonResourceType).
 * Only the ANIMATION case affects our send + download decisions; the single
 * definition lives here so the send path and the download SQL agree. */
#define ENIL_RESOURCE_TYPE_ANIMATION 2

typedef struct {
  char *version;      /* malloc'd; may be NULL */
  char *option;       /* malloc'd; may be NULL */
  char *hash;         /* malloc'd; may be NULL */
  int   resource_type; /* LINE resourceType enum; ENIL_RESOURCE_TYPE_ANIMATION == animated */
} ENILStickerMeta;

int  enil_db_get_sticker_meta(sqlite3 *db, const char *package_id,
                               const char *sticker_id, ENILStickerMeta *out);
void enil_db_sticker_meta_free(ENILStickerMeta *m);

/* Append one row to talk_exceptions. raw_json is the full SSE data payload. */
int enil_db_talk_exception_record(sqlite3    *db,
                                  long long   received_at,
                                  int         code,
                                  const char *reason,
                                  const char *raw_json);

/* Append one row to sse_events_v2 (audit log of received SSE events).
 * op_type < 0 and revision <= 0 are stored as NULL. handled: 1='YES',
 * 0='NO'. Callers should skip keep-alive "ping" events. */
int enil_db_sse_event_record(sqlite3    *db,
                             long long   received_at,
                             const char *event_type,
                             int         op_type,
                             const char *op_name,
                             long long   revision,
                             const char *chat_id,
                             int         handled,
                             const char *raw_json);

#endif
