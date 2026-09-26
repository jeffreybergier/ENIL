#ifndef ENIL_ACCOUNT_H
#define ENIL_ACCOUNT_H

#include <stddef.h>
#include <sqlite3.h>
#include "enil_api_types.h"
#include "enil_health.h"

typedef enum {
  ENIL_ACCOUNT_START_OK = 0,
  ENIL_ACCOUNT_START_INVALID_SESSION = 1,
  ENIL_ACCOUNT_START_DB_OPEN = 2,
  ENIL_ACCOUNT_START_DB_CREATE = 3
} enil_account_start_result_t;

typedef struct {
  const unsigned char *jpeg_data;
  size_t               jpeg_len;
  int                  orig_width;
  int                  orig_height;
  const unsigned char *thumb_data;
  size_t               thumb_len;
  int                  thumb_width;
  int                  thumb_height;
} enil_account_image_send_t;

typedef enum {
  ENIL_ACCOUNT_SSE_TALK_ACTION_NONE = 0,
  ENIL_ACCOUNT_SSE_TALK_ACTION_REAUTH_NEEDED = 1,
  ENIL_ACCOUNT_SSE_TALK_ACTION_REFRESH_RECONNECT = 2,
  ENIL_ACCOUNT_SSE_TALK_ACTION_SYNC_NEEDED = 3,
  ENIL_ACCOUNT_SSE_TALK_ACTION_ERROR = 4
} enil_account_sse_talk_action_t;

typedef struct {
  char message_id[128];
  char from_mid[256];
  char to_mid[256];
  int  to_type;
  int  op_type;
  int  has_message;
} enil_account_sse_message_info_t;

typedef struct {
  char *chat_id;
  char *js;
} enil_account_sse_js_update_t;

typedef struct {
  int handled;
  int should_stop_sse;
  int should_start_sync;
  int talk_exception_code;
  enil_account_sse_talk_action_t talk_action;
  char *affected_chat_id;
  enil_account_sse_js_update_t *js_updates;
  int js_update_count;
  int js_update_capacity;
} enil_account_sse_result_t;

void enil_account_free(void *p);

int enil_account_upsert_response_message(sqlite3 *db, const char *chat_id,
                                         talk_message_t *msg);

/* Returns 1 only after this account's sync and revision commit succeed. */
int enil_account_sync_all(enil_health_t *health, sqlite3 *db,
                           const char *access_token, const char *my_mid,
                           const char *session_path);

enil_account_start_result_t enil_account_start_core(const char *enil_dir,
                                                    sqlite3 **db_io,
                                                    char **access_token_out,
                                                    char **mid_out,
                                                    int *sse_enabled_out,
                                                    int *e2ee_recovered_out);

char *enil_account_validated_mid_for_session(const char *path);
char *enil_account_display_name_for_session(const char *path);

int enil_account_is_one_to_one_mid(const char *mid);
int enil_account_worker_failed(void);
int enil_account_line_failed(void);
int enil_account_any_service_failed(void);

unsigned char *enil_account_qr_modules_for_string(const char *utf8,
                                                  int *out_size);

char *enil_account_chat_page_html(sqlite3 *db, const char *chat_id,
                                  const char *my_mid,
                                  const char *message_css,
                                  const char *empty_css,
                                  int page_size,
                                  long long *out_oldest,
                                  int *out_has_more);

char *enil_account_prepend_messages_js(sqlite3 *db, const char *chat_id,
                                       const char *my_mid,
                                       long long before,
                                       int page_size,
                                       int *out_has_more,
                                       long long *out_oldest);

int enil_account_prepare_image(const char *source_path,
                               const char *media_path,
                               const char *thumb_path,
                               int *orig_width_out,
                               int *orig_height_out,
                               int *thumb_width_out,
                               int *thumb_height_out);

int enil_account_send_text(enil_health_t *health, sqlite3 *db,
                           const char *session_path, const char *chat_id,
                           const char *text, char **out_message_id);

int enil_account_send_inline_sticon(enil_health_t *health, sqlite3 *db,
                                    const char *session_path,
                                    const char *chat_id,
                                    const char *text,
                                    int count,
                                    const char * const *package_ids,
                                    const char * const *sticon_ids,
                                    const char * const *alt_texts,
                                    char **out_message_id);

int enil_account_send_sticker(enil_health_t *health, sqlite3 *db,
                              const char *session_path,
                              const char *chat_id,
                              const char *sticker_id,
                              const char *package_id,
                              char **out_message_id);

int enil_account_send_image(enil_health_t *health, sqlite3 *db,
                            const char *session_path,
                            const char *chat_id,
                            const enil_account_image_send_t *params,
                            char **out_message_id);

int enil_account_mark_chat_seen(enil_health_t *health,
                                const char *session_path,
                                const char *access_token,
                                const char *chat_id,
                                const char *message_id);

int enil_account_remove_chat(enil_health_t *health, sqlite3 *db,
                             const char *session_path,
                             const char *chat_id,
                             const char *last_read_message_id,
                             long long last_read_message_time);

void enil_account_sse_result_init(enil_account_sse_result_t *result);
void enil_account_sse_result_free(enil_account_sse_result_t *result);

int enil_account_sse_message_info(const char *event_data,
                                  enil_account_sse_message_info_t *info);

/* Returns 0 if applying the event failed; do not acknowledge its revision.
 * result is initialized even on failure and must be freed by the caller. */
int enil_account_process_sse_event(enil_health_t *health,
                                   sqlite3 *db,
                                   const char *access_token,
                                   const char *my_mid,
                                   const char *session_path,
                                   const char *event_type,
                                   const char *event_data,
                                   const char *matched_temp_id,
                                   enil_account_sse_result_t *result);

#endif /* ENIL_ACCOUNT_H */
