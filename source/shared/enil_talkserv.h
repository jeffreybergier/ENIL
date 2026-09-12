#ifndef ENIL_TALKSERV_H
#define ENIL_TALKSERV_H

#include "cJSON.h"
#include "enil_api_types.h"

/* All returned cJSON* values must be freed with cJSON_Delete by the caller.
 * Returned char* values are malloc'd and must be freed by the caller.        */

int    talk_get_profile(const char                   *access_token,
                        const talk_get_profile_req_t *req,
                        talk_profile_t               *out);
int    talk_get_contacts(const char *access_token,
                         talk_contact_t **out, int *out_count);
/* Fetch a single contact by mid (1-element targetUserMids batch). Returns 0
 * on success; caller frees with talk_contact_free. */
int    talk_get_contact_by_mid(const char     *access_token,
                               const char     *mid,
                               talk_contact_t *out);
int    talk_get_all_chat_mids(const char                          *access_token,
                              const talk_get_all_chat_mids_req_t  *req,
                              talk_get_all_chat_mids_t            *out);
int    talk_get_chats(const char *access_token,
                      const char *const *chat_mids, int count,
                      talk_chat_t **out, int *out_count);
int    talk_get_message_boxes(const char *access_token,
                              talk_message_box_t **out, int *out_count);
int    talk_get_recent_messages(const char *access_token, const char *chat_id, int count,
                                talk_message_t **out, int *out_count);
/* TalkService.getMessageReadRange — returns the raw "data" cJSON array for
 * the given chat mids. Each entry is { chatId, ranges: { reader_mid:
 *   [{ startMessageId, endMessageId, startTime, endTime }, ...] } }. Wire body
 * is [[chat_mids...], 1]; the server accepts batched chat mids in one call.
 * Caller cJSON_Delete()s the result. Returns NULL on transport failure, empty
 * input, or non-OK response. */
cJSON *talk_get_message_read_range(const char        *access_token,
                                   const char *const *chat_mids,
                                   int                count);
/* ShopService.getOwnedProductSummaries split per-shop typed fetchers. The
 * underlying RPC body differs only by shop name; the parse splits cleanly.
 * Caller owns *out array — free each element with sticker_package_free /
 * sticon_package_free, then free() the array. */
int    sticker_package_fetch_all(const char *access_token,
                                 sticker_package_t **out, int *out_count);
int    sticon_package_fetch_all (const char *access_token,
                                 sticon_package_t **out, int *out_count);
long long TalkService_getLastOpRevision(const char *access_token);
char  *TalkService_getE2EEPublicKey(const char *mid, long long key_id, const char *access_token);

/* TalkService.sendChatChecked — mark `message_id` as the last-read marker for
 * `chat_mid`. reqSeq is always 0 (per matrix-line). Returns 0 on success. */
int    talk_send_chat_checked(const char *access_token,
                              const char *chat_mid,
                              const char *message_id);

/* TalkService.sendChatRemoved — leave/delete a chat from the user's chat
 * list. `last_read_message_id` may be NULL. Returns 0 on success. */
int    talk_send_chat_removed(const char *access_token,
                              long long   req_seq,
                              const char *chat_mid,
                              const char *last_read_message_id,
                              long long   last_read_message_time);

/* TalkService.determineMediaMessageFlow — ask the server which upload path to
 * use for media in `req->chatMid`. Returns 0 on success; caller frees `out`
 * with talk_media_flow_free. See talk_media_flow_t for flow value semantics. */
int    talk_determine_media_message_flow(const char                  *access_token,
                                         const talk_media_flow_req_t *req,
                                         talk_media_flow_t           *out);

#endif
