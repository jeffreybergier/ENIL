#include "enil_account.h"

#include "cJSON.h"
#include "enil_cocoa_image.h"
#include "enil_cocoa_log.h"
#include "enil_cocoa_progress.h"
#include "enil_db.h"
#include "enil_html.h"
#include "enil_line.h"
#include "enil_qrlogin.h"
#include "enil_session.h"
#include "enil_sync.h"
#include "enil_talkserv.h"
#include "qrcodegen.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void enil_account_free(void *p)
{
  free(p);
}

static char *enil_account_strdup(const char *s)
{
  char *out;
  size_t n;
  if (!s) return NULL;
  n = strlen(s);
  out = (char *)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, s, n + 1);
  return out;
}

static char *enil_account_path_join(const char *dir, const char *name)
{
  size_t dl, nl, need;
  char *out;
  if (!dir || !name) return NULL;
  dl = strlen(dir);
  nl = strlen(name);
  need = dl + (dl > 0 && dir[dl - 1] == '/' ? 0 : 1) + nl + 1;
  out = (char *)malloc(need);
  if (!out) return NULL;
  if (dl > 0 && dir[dl - 1] == '/')
    snprintf(out, need, "%s%s", dir, name);
  else
    snprintf(out, need, "%s/%s", dir, name);
  return out;
}

int enil_account_upsert_response_message(sqlite3 *db, const char *chat_id,
                                         talk_message_t *msg)
{
  if (!db || !chat_id || !msg) return 0;
  free(msg->chat_id);
  msg->chat_id = enil_account_strdup(chat_id);
  return msg->chat_id && enil_db_message_upsert(db, msg) == SQLITE_OK;
}

void enil_account_sync_all(enil_health_t *health, sqlite3 *db,
                           const char *access_token, const char *my_mid,
                           const char *session_path)
{
  enil_health_bind(health);
  enil_sync_all(db, access_token, my_mid, session_path);
}

enil_account_start_result_t enil_account_start_core(const char *enil_dir,
                                                    sqlite3 **db_io,
                                                    char **access_token_out,
                                                    char **mid_out,
                                                    int *sse_enabled_out,
                                                    int *e2ee_recovered_out)
{
  char *session_path;
  char *db_path;
  char *access_token = NULL;
  char *mid = NULL;
  sqlite3 *db;

  if (access_token_out) *access_token_out = NULL;
  if (mid_out) *mid_out = NULL;
  if (sse_enabled_out) *sse_enabled_out = 0;
  if (e2ee_recovered_out) *e2ee_recovered_out = 0;
  if (!enil_dir || !db_io) return ENIL_ACCOUNT_START_INVALID_SESSION;

  session_path = enil_account_path_join(enil_dir, "session.json");
  if (!session_path) return ENIL_ACCOUNT_START_INVALID_SESSION;

  if (!enil_session_validate(session_path, &access_token, &mid)) {
    free(session_path);
    return ENIL_ACCOUNT_START_INVALID_SESSION;
  }

  if (sse_enabled_out)
    *sse_enabled_out = enil_session_get_sse_enabled(session_path);

  if (e2ee_recovered_out)
    *e2ee_recovered_out = enil_qrlogin_recover_e2ee(enil_dir) ? 1 : 0;
  else
    (void)enil_qrlogin_recover_e2ee(enil_dir);

  db_path = enil_account_path_join(enil_dir, "enil.sqlite");
  if (!db_path) {
    free(session_path);
    free(access_token);
    free(mid);
    return ENIL_ACCOUNT_START_DB_OPEN;
  }

  enil_db_close(*db_io);
  *db_io = NULL;
  db = enil_db_open(db_path);
  free(db_path);
  if (!db) {
    free(session_path);
    free(access_token);
    free(mid);
    return ENIL_ACCOUNT_START_DB_OPEN;
  }

  *db_io = db;
  if (enil_db_create_tables(db) != SQLITE_OK) {
    free(session_path);
    free(access_token);
    free(mid);
    return ENIL_ACCOUNT_START_DB_CREATE;
  }

  free(session_path);
  if (access_token_out)
    *access_token_out = access_token;
  else
    free(access_token);
  if (mid_out)
    *mid_out = mid;
  else
    free(mid);
  return ENIL_ACCOUNT_START_OK;
}

char *enil_account_validated_mid_for_session(const char *path)
{
  char *tok = NULL;
  char *mid = NULL;
  if (!path) return NULL;
  if (!enil_session_validate(path, &tok, &mid)) {
    free(tok);
    free(mid);
    return NULL;
  }
  free(tok);
  return mid;
}

char *enil_account_display_name_for_session(const char *path)
{
  cJSON *ss;
  cJSON *dn;
  char *out = NULL;
  if (!path) return NULL;
  ss = enil_session_read(path);
  if (!ss) return NULL;
  dn = cJSON_GetObjectItem(ss, "displayName");
  if (cJSON_IsString(dn) && dn->valuestring)
    out = enil_account_strdup(dn->valuestring);
  cJSON_Delete(ss);
  return out;
}

int enil_account_is_one_to_one_mid(const char *mid)
{
  return enil_mid_is_one_to_one(mid);
}

int enil_account_worker_failed(void)
{
  return enil_health_is_failed(ENIL_ERR_WORKER);
}

int enil_account_line_failed(void)
{
  return enil_health_is_failed(ENIL_ERR_LINE);
}

int enil_account_any_service_failed(void)
{
  return enil_health_any_failed();
}

unsigned char *enil_account_qr_modules_for_string(const char *utf8,
                                                  int *out_size)
{
  uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
  uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
  int size;
  unsigned char *modules;
  int x, y;

  if (out_size) *out_size = 0;
  if (!utf8 || !utf8[0]) return NULL;
  if (!qrcodegen_encodeText(utf8, tmp, qr, qrcodegen_Ecc_MEDIUM,
                            qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                            qrcodegen_Mask_AUTO, true)) {
    return NULL;
  }

  size = qrcodegen_getSize(qr);
  if (size <= 0) return NULL;
  modules = (unsigned char *)malloc((size_t)(size * size));
  if (!modules) return NULL;

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      modules[y * size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
    }
  }

  if (out_size) *out_size = size;
  return modules;
}

static char *enil_account_empty_chat_html(const char *css, const char *message)
{
  const char *prefix = "<html><head>" ENIL_HTML_ZOOM_SCRIPT "<style>";
  const char *middle = "</style></head><body><p>";
  const char *suffix = "</p></body></html>";
  size_t need;
  char *out;

  if (!css) css = "";
  if (!message) message = "";
  need = strlen(prefix) + strlen(css) + strlen(middle) + strlen(message) +
         strlen(suffix) + 1;
  out = (char *)malloc(need);
  if (!out) return NULL;
  snprintf(out, need, "%s%s%s%s%s", prefix, css, middle, message, suffix);
  return out;
}

static void enil_account_html_message_from_row(ENILHTMLMessage *dst,
                                               const message_row_t *row,
                                               int include_reactions)
{
  memset(dst, 0, sizeof(*dst));
  dst->message_id     = row->message_id;
  dst->sender_name    = row->sender_name;
  dst->text           = row->text;
  dst->text_html      = row->text_html;
  dst->media_path     = row->media_path;
  dst->thumb_path     = row->thumb_path;
  dst->sticker_path   = row->sticker_path;
  dst->avatar_path    = row->avatar_path;
  dst->reactions_json = include_reactions ? row->reactions_json : NULL;
  dst->created_at     = row->created_at;
  dst->content_type   = row->content_type;
  dst->is_outgoing    = row->is_outgoing;
  dst->is_deleted     = row->is_deleted;
  dst->encrypted_unresolved =
    (row->decrypt_status == ENIL_DECRYPT_PENDING ||
     row->decrypt_status == ENIL_DECRYPT_FAILED) ? 1 : 0;
  dst->sticker_width  = row->sticker_width;
  dst->sticker_height = row->sticker_height;
  dst->orig_width     = row->orig_width;
  dst->orig_height    = row->orig_height;
  dst->thumb_width    = row->thumb_width;
  dst->thumb_height   = row->thumb_height;
}

static void enil_account_free_message_rows(message_row_t *rows, int count)
{
  int i;
  if (!rows) return;
  for (i = 0; i < count; i++) message_row_free(&rows[i]);
  free(rows);
}

char *enil_account_chat_page_html(sqlite3 *db, const char *chat_id,
                                  const char *my_mid,
                                  const char *message_css,
                                  const char *empty_css,
                                  int page_size,
                                  long long *out_oldest,
                                  int *out_has_more)
{
  message_row_t *rows = NULL;
  int n = 0;
  int more = 0;
  int i;
  ENILHTMLMessage *hm;
  long long peer_read_time = 0;
  ENILHTMLReader *hr = NULL;
  enil_group_reader_t *gr = NULL;
  int n_gr = 0;
  enil_mid_kind_t kind;
  char *seen_msg_id;
  char *html;

  if (out_oldest) *out_oldest = 0;
  if (out_has_more) *out_has_more = 0;
  if (!db || !chat_id || !chat_id[0]) return enil_account_strdup("");
  if (page_size <= 0) page_size = 40;

  enil_db_message_rows_get_page(db, chat_id, my_mid, 0, page_size,
                                &rows, &n, &more);

  if (n == 0 && enil_mid_is_one_to_one(chat_id)) {
    free(rows);
    return enil_account_empty_chat_html(empty_css,
                                        "Send a message to start the chat");
  }
  if (out_has_more) *out_has_more = more ? 1 : 0;
  if (out_oldest && n > 0) *out_oldest = rows[0].created_at;

  hm = (ENILHTMLMessage *)calloc((size_t)n, sizeof(*hm));
  if (!hm) {
    enil_account_free_message_rows(rows, n);
    return enil_account_strdup("");
  }
  for (i = 0; i < n; i++)
    enil_account_html_message_from_row(&hm[i], &rows[i], 1);

  kind = enil_mid_kind(chat_id);
  if (kind == ENIL_MID_USER) {
    peer_read_time = enil_db_message_box_get_peer_last_read_time(db, chat_id);
  } else if (my_mid && (kind == ENIL_MID_GROUP || kind == ENIL_MID_ROOM)) {
    if (enil_db_message_box_get_group_readers(db, chat_id, my_mid,
                                               &gr, &n_gr) == SQLITE_OK &&
        n_gr > 0) {
      hr = (ENILHTMLReader *)calloc((size_t)n_gr, sizeof(*hr));
      if (hr) {
        for (i = 0; i < n_gr; i++) {
          hr[i].reader_mid           = gr[i].reader_mid;
          hr[i].display_name         = gr[i].display_name;
          hr[i].last_read_message_id = gr[i].last_read_message_id;
        }
      }
    }
  }

  seen_msg_id = enil_db_message_box_last_seen(db, chat_id);
  html = enil_html_page_with_css_more(hm, n, message_css, "../", more,
                                      peer_read_time, hr, hr ? n_gr : 0,
                                      seen_msg_id);
  free(seen_msg_id);
  free(hm);
  free(hr);
  enil_group_readers_free(gr, n_gr);
  enil_account_free_message_rows(rows, n);

  return html ? html : enil_account_strdup("");
}

char *enil_account_prepend_messages_js(sqlite3 *db, const char *chat_id,
                                       const char *my_mid,
                                       long long before,
                                       int page_size,
                                       int *out_has_more,
                                       long long *out_oldest)
{
  message_row_t *rows = NULL;
  int n = 0;
  int more = 0;
  int i;
  ENILHTMLMessage *hm;
  char *js;

  if (out_has_more) *out_has_more = 0;
  if (out_oldest) *out_oldest = before;
  if (!db || !chat_id || !chat_id[0] || before <= 0) return NULL;
  if (page_size <= 0) page_size = 40;

  enil_db_message_rows_get_page(db, chat_id, my_mid, before, page_size,
                                &rows, &n, &more);
  if (n <= 0) {
    free(rows);
    return NULL;
  }

  hm = (ENILHTMLMessage *)calloc((size_t)n, sizeof(*hm));
  if (!hm) {
    enil_account_free_message_rows(rows, n);
    return NULL;
  }

  for (i = 0; i < n; i++)
    enil_account_html_message_from_row(&hm[i], &rows[i], 0);

  js = enil_html_js_prepend(hm, n, more);
  free(hm);

  if (out_oldest) *out_oldest = rows[0].created_at;
  if (out_has_more) *out_has_more = more ? 1 : 0;
  enil_account_free_message_rows(rows, n);
  return js;
}

int enil_account_prepare_image(const char *source_path,
                               const char *media_path,
                               const char *thumb_path,
                               int *orig_width_out,
                               int *orig_height_out,
                               int *thumb_width_out,
                               int *thumb_height_out)
{
  ENILThumbInfo info;
  ENILThumbInfo tinfo;

  if (orig_width_out) *orig_width_out = 0;
  if (orig_height_out) *orig_height_out = 0;
  if (thumb_width_out) *thumb_width_out = 0;
  if (thumb_height_out) *thumb_height_out = 0;
  if (!source_path || !media_path || !thumb_path) return 0;

  memset(&info, 0, sizeof(info));
  if (enil_image_jpeg_create(source_path, media_path, 0, &info) != 0) {
    ENIL_LOG("ENILAccount.sendImage", "JPEG transcode failed: %s", source_path);
    return 0;
  }

  memset(&tinfo, 0, sizeof(tinfo));
  if (enil_thumb_generate(media_path, thumb_path, &tinfo) != 0) {
    ENIL_LOG("ENILAccount.sendImage", "thumbnail failed");
    return 0;
  }

  if (orig_width_out) *orig_width_out = info.orig_width;
  if (orig_height_out) *orig_height_out = info.orig_height;
  if (thumb_width_out) *thumb_width_out = tinfo.thumb_width;
  if (thumb_height_out) *thumb_height_out = tinfo.thumb_height;
  return 1;
}

static int enil_account_finish_send(sqlite3 *db, const char *chat_id,
                                    ENILLineSendResult *result,
                                    char **out_message_id)
{
  int ok;
  if (out_message_id) *out_message_id = NULL;
  ok = result && result->message &&
       enil_account_upsert_response_message(db, chat_id, result->message);
  if (ok && out_message_id)
    *out_message_id = enil_account_strdup(result->message_id);
  return ok && (!out_message_id || *out_message_id);
}

int enil_account_send_text(enil_health_t *health, sqlite3 *db,
                           const char *session_path, const char *chat_id,
                           const char *text, char **out_message_id)
{
  ENILLineSendResult result;
  int ok;

  if (out_message_id) *out_message_id = NULL;
  enil_health_bind(health);
  memset(&result, 0, sizeof(result));
  ok = enil_line_send_text(session_path, chat_id, text, &result);
  if (ok) ok = enil_account_finish_send(db, chat_id, &result, out_message_id);
  enil_line_send_result_free(&result);
  return ok;
}

int enil_account_send_inline_sticon(enil_health_t *health, sqlite3 *db,
                                    const char *session_path,
                                    const char *chat_id,
                                    const char *text,
                                    int count,
                                    const char * const *package_ids,
                                    const char * const *sticon_ids,
                                    const char * const *alt_texts,
                                    char **out_message_id)
{
  const char **resource_types;
  const char **versions;
  ENILLineSticonResource *line_items;
  ENILLineSendResult result;
  int ok = 0;
  int i;

  if (out_message_id) *out_message_id = NULL;
  if (count <= 0 || !package_ids || !sticon_ids) return 0;
  enil_health_bind(health);

  resource_types = (const char **)calloc((size_t)count, sizeof(char *));
  versions = (const char **)calloc((size_t)count, sizeof(char *));
  if (!resource_types || !versions) {
    free(resource_types);
    free(versions);
    return 0;
  }

  for (i = 0; i < count; i++) {
    ENILStickerMeta m;
    memset(&m, 0, sizeof(m));
    enil_db_get_sticker_meta(db, package_ids[i], sticon_ids[i], &m);
    resource_types[i] =
      (m.resource_type == ENIL_RESOURCE_TYPE_ANIMATION) ? "ANIMATION" : "STATIC";
    versions[i] = enil_account_strdup(m.version ? m.version : "");
    enil_db_sticker_meta_free(&m);
    if (!versions[i]) goto done;
  }

  line_items = enil_line_build_sticon_resources(text, count, package_ids,
                                                sticon_ids, alt_texts,
                                                resource_types, versions);
  if (!line_items) goto done;

  memset(&result, 0, sizeof(result));
  ok = enil_line_send_inline_sticon(session_path, chat_id, text, line_items,
                                    count, &result);
  if (ok) ok = enil_account_finish_send(db, chat_id, &result, out_message_id);
  enil_line_send_result_free(&result);
  free(line_items);

done:
  for (i = 0; i < count; i++) free((void *)versions[i]);
  free(resource_types);
  free(versions);
  return ok;
}

int enil_account_send_sticker(enil_health_t *health, sqlite3 *db,
                              const char *session_path,
                              const char *chat_id,
                              const char *sticker_id,
                              const char *package_id,
                              char **out_message_id)
{
  ENILStickerMeta meta;
  ENILLineSendStickerParams sp;
  ENILLineSendResult result;
  int ok;

  if (out_message_id) *out_message_id = NULL;
  enil_health_bind(health);

  memset(&meta, 0, sizeof(meta));
  memset(&sp, 0, sizeof(sp));
  memset(&result, 0, sizeof(result));
  enil_db_get_sticker_meta(db, package_id, sticker_id, &meta);
  sp.package_id = package_id;
  sp.sticker_id = sticker_id;
  sp.version    = meta.version ? meta.version : "";
  sp.option     = meta.option  ? meta.option  : "";
  sp.hash       = meta.hash    ? meta.hash    : "";

  ok = enil_line_send_sticker(session_path, chat_id, &sp, &result);
  if (ok) ok = enil_account_finish_send(db, chat_id, &result, out_message_id);
  enil_db_sticker_meta_free(&meta);
  enil_line_send_result_free(&result);
  return ok;
}

int enil_account_send_image(enil_health_t *health, sqlite3 *db,
                            const char *session_path,
                            const char *chat_id,
                            const enil_account_image_send_t *params,
                            char **out_message_id)
{
  ENILLineSendImageParams sp;
  ENILLineSendResult result;
  int ok;

  if (out_message_id) *out_message_id = NULL;
  if (!params) return 0;
  enil_health_bind(health);

  memset(&sp, 0, sizeof(sp));
  sp.jpeg_data    = params->jpeg_data;
  sp.jpeg_len     = params->jpeg_len;
  sp.orig_width   = params->orig_width;
  sp.orig_height  = params->orig_height;
  sp.thumb_data   = params->thumb_data;
  sp.thumb_len    = params->thumb_len;
  sp.thumb_width  = params->thumb_width;
  sp.thumb_height = params->thumb_height;
  sp.file_name    = "image.jpg";

  memset(&result, 0, sizeof(result));
  ok = enil_line_send_image(session_path, chat_id, &sp, &result);
  if (ok) ok = enil_account_finish_send(db, chat_id, &result, out_message_id);
  enil_line_send_result_free(&result);
  return ok;
}

int enil_account_mark_chat_seen(enil_health_t *health,
                                const char *access_token,
                                const char *chat_id,
                                const char *message_id)
{
  enil_health_bind(health);
  if (!access_token || !chat_id || !message_id) return 0;
  if (talk_send_chat_checked(access_token, chat_id, message_id) != 0) {
    ENIL_LOG("ENILAccount.markChatSeen", "RPC failed for %s", chat_id);
    return 0;
  }
  return 1;
}

int enil_account_remove_chat(enil_health_t *health, sqlite3 *db,
                             const char *session_path,
                             const char *chat_id,
                             const char *last_read_message_id,
                             long long last_read_message_time)
{
  int ok;
  enil_health_bind(health);
  ok = enil_line_send_chat_removed(session_path, chat_id,
                                   last_read_message_id,
                                   last_read_message_time);
  if (ok) {
    enil_db_chat_delete(db, chat_id);
  } else {
    ENIL_LOG("ENILAccount.removeChat", "RPC failed for %s",
             chat_id ? chat_id : "(null)");
  }
  return ok;
}

void enil_account_sse_result_init(enil_account_sse_result_t *result)
{
  if (!result) return;
  memset(result, 0, sizeof(*result));
}

void enil_account_sse_result_free(enil_account_sse_result_t *result)
{
  int i;
  if (!result) return;
  for (i = 0; i < result->js_update_count; i++) {
    free(result->js_updates[i].chat_id);
    enil_html_free(result->js_updates[i].js);
  }
  free(result->js_updates);
  free(result->affected_chat_id);
  enil_account_sse_result_init(result);
}

int enil_account_sse_message_info(const char *event_data,
                                  enil_account_sse_message_info_t *info)
{
  ENILSSEMessageInfo mi;
  int ok;

  if (!info) return 0;
  memset(info, 0, sizeof(*info));
  info->to_type = -1;

  memset(&mi, 0, sizeof(mi));
  ok = enil_sync_sse_message_info(event_data, &mi);
  if (!ok) return 0;

  snprintf(info->message_id, sizeof(info->message_id), "%s", mi.message_id);
  snprintf(info->from_mid, sizeof(info->from_mid), "%s", mi.from_mid);
  snprintf(info->to_mid, sizeof(info->to_mid), "%s", mi.to_mid);
  info->to_type = mi.to_type;
  info->op_type = mi.op_type;
  info->has_message = mi.has_message;
  return ok;
}

static int enil_account_sse_control_handled(const char *event_type)
{
  if (!event_type) return 0;
  return (strcmp(event_type, "fullSync")         == 0 ||
          strcmp(event_type, "partialFullSync")  == 0 ||
          strcmp(event_type, "reconnect")        == 0 ||
          strcmp(event_type, "connInfoRevision") == 0 ||
          strcmp(event_type, "talkException")    == 0) ? 1 : 0;
}

static void enil_account_sse_set_affected_chat(enil_account_sse_result_t *result,
                                               const char *chat_id)
{
  char *copy;
  if (!result || !chat_id || !chat_id[0]) return;
  copy = enil_account_strdup(chat_id);
  if (!copy) return;
  free(result->affected_chat_id);
  result->affected_chat_id = copy;
}

static void enil_account_sse_add_js(enil_account_sse_result_t *result,
                                    const char *chat_id,
                                    char *js)
{
  enil_account_sse_js_update_t *updates;
  char *chat_copy;
  int new_cap;

  if (!js) return;
  if (!result || !chat_id || !chat_id[0]) {
    enil_html_free(js);
    return;
  }

  if (result->js_update_count >= result->js_update_capacity) {
    new_cap = result->js_update_capacity ? result->js_update_capacity * 2 : 4;
    updates = (enil_account_sse_js_update_t *)realloc(result->js_updates,
      (size_t)new_cap * sizeof(*updates));
    if (!updates) {
      enil_html_free(js);
      return;
    }
    result->js_updates = updates;
    result->js_update_capacity = new_cap;
  }

  chat_copy = enil_account_strdup(chat_id);
  if (!chat_copy) {
    enil_html_free(js);
    return;
  }

  result->js_updates[result->js_update_count].chat_id = chat_copy;
  result->js_updates[result->js_update_count].js = js;
  result->js_update_count++;
}

/* Append the event to sse_events_v2. Skips keep-alive pings. msg_handled is
 * the dispatcher's verdict for "message" events; control events (fullSync,
 * reconnect, etc.) are handled=YES since we always act on them. */
static void enil_account_record_sse_event(sqlite3 *db,
                                          const char *event_type,
                                          const char *event_data,
                                          int msg_handled)
{
  cJSON *root;
  long long revision = 0;
  int op_type = -1, handled;
  const char *op_name = NULL;
  char chat_id[256] = "";

  if (!db || !event_type || !event_data) return;
  if (strcmp(event_type, "ping") == 0) return;

  if (strcmp(event_type, "message") == 0) {
    ENILSSEMessageInfo mi;
    memset(&mi, 0, sizeof(mi));
    enil_sync_sse_message_info(event_data, &mi);
    op_type = mi.op_type;
    op_name = enil_op_type_name(mi.op_type);
    if (mi.to_mid[0]) snprintf(chat_id, sizeof(chat_id), "%s", mi.to_mid);
    handled = msg_handled;
  } else {
    handled = enil_account_sse_control_handled(event_type);
  }

  root = cJSON_Parse(event_data);
  if (root) {
    const char *key = strcmp(event_type, "fullSync") == 0
      ? "nextRevision" : "revision";
    cJSON *r = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(r)) revision = (long long)r->valuedouble;
    else if (cJSON_IsString(r) && r->valuestring)
      revision = strtoll(r->valuestring, NULL, 10);
    cJSON_Delete(root);
  }

  enil_db_sse_event_record(db, (long long)time(NULL),
                           event_type, op_type, op_name, revision,
                           chat_id[0] ? chat_id : NULL, handled,
                           event_data);
}

static void enil_account_sse_build_message_updates(sqlite3 *db,
                                                   const char *my_mid,
                                                   const char *event_data,
                                                   const char *matched_temp_id,
                                                   enil_account_sse_result_t *result)
{
  char chat_id_buf[256] = "";
  ENILSSEMessageInfo msg_info;
  int renderable;
  char *js_raw;

  memset(&msg_info, 0, sizeof(msg_info));
  enil_sync_sse_message_info(event_data, &msg_info);

  /* Only the SEND/RECEIVE ops produce a renderable bubble. Other ops
   * (for example, 43 NOTIFIED_SEND_CONTENT_RECEIPT after a media send) can
   * carry the same `message` payload. Rendering those would duplicate the
   * bubble after pending sends have already been consumed by op 25. */
  renderable = msg_info.has_message &&
    (msg_info.op_type == ENIL_OP_SEND_MESSAGE ||
     msg_info.op_type == ENIL_OP_RECEIVE_MESSAGE);
  js_raw = renderable
    ? enil_sync_js_for_sse_message(db, my_mid, event_data, matched_temp_id,
                                   chat_id_buf, sizeof(chat_id_buf))
    : NULL;
  if (js_raw && chat_id_buf[0])
    enil_account_sse_add_js(result, chat_id_buf, js_raw);
  else
    enil_html_free(js_raw);

  /* SEND/RECEIVE both advance lastDeliveredTime (RECEIVE also bumps unread),
   * so the chat floats to the top of the list. chat_id_buf is only set on
   * renderable SEND/RECEIVE ops, so reactions/deletes carrying a message
   * payload do not trigger a list reseat. */
  if (chat_id_buf[0])
    enil_account_sse_set_affected_chat(result, chat_id_buf);

  /* Incoming peer messages keep our preserved lastSeenMessageId anchored just
   * above the unread burst. The DB write already bumped unreadCount and kept
   * lastSeenMessageId put, so emit a marker push after the append JS. */
  if (msg_info.op_type == ENIL_OP_RECEIVE_MESSAGE && chat_id_buf[0] &&
      enil_db_message_box_unread_count(db, chat_id_buf) > 0) {
    char *seen_id = enil_db_message_box_last_seen(db, chat_id_buf);
    char *seen_js = enil_html_js_set_seen_marker(seen_id);
    enil_account_sse_add_js(result, chat_id_buf, seen_js);
    free(seen_id);
  }

  if (msg_info.op_type == ENIL_OP_SEND_REACTION ||
      msg_info.op_type == ENIL_OP_NOTIFIED_SEND_REACTION) {
    char rxn_chat_buf[256] = "";
    char *rxn_js = enil_sync_js_for_sse_reaction(db, event_data,
                                                 rxn_chat_buf,
                                                 sizeof(rxn_chat_buf));
    enil_account_sse_add_js(result, rxn_chat_buf, rxn_js);
  }

  if (msg_info.op_type == ENIL_OP_NOTIFIED_READ_MESSAGE && my_mid) {
    char read_chat_buf[256] = "";
    char *read_js = enil_sync_js_for_sse_read_receipt(db, my_mid,
                                                      event_data,
                                                      read_chat_buf,
                                                      sizeof(read_chat_buf));
    if (!read_js) {
      read_js = enil_sync_js_for_sse_group_read_receipt(db, my_mid,
                                                        event_data,
                                                        read_chat_buf,
                                                        sizeof(read_chat_buf));
    }
    enil_account_sse_add_js(result, read_chat_buf, read_js);
  }

  if (msg_info.op_type == ENIL_OP_SEND_CHAT_CHECKED) {
    char cc_chat_buf[256] = "";
    char *cc_js = enil_sync_js_for_sse_chat_checked(db, event_data,
                                                    cc_chat_buf,
                                                    sizeof(cc_chat_buf));
    if (cc_chat_buf[0])
      enil_account_sse_set_affected_chat(result, cc_chat_buf);
    enil_account_sse_add_js(result, cc_chat_buf, cc_js);
  }

  if (msg_info.op_type == ENIL_OP_DESTROY_MESSAGE ||
      msg_info.op_type == ENIL_OP_NOTIFIED_DESTROY_MESSAGE) {
    char del_chat_buf[256] = "";
    char *del_js = enil_sync_js_for_sse_delete(db, event_data,
                                               del_chat_buf,
                                               sizeof(del_chat_buf));
    enil_account_sse_add_js(result, del_chat_buf, del_js);
  }
}

static void enil_account_sse_handle_talk_exception(sqlite3 *db,
                                                   const char *session_path,
                                                   const char *event_data,
                                                   enil_account_sse_result_t *result)
{
  cJSON *root;
  cJSON *reason_item;
  int code;
  const char *reason;

  root = cJSON_Parse(event_data);
  code = enil_sync_sse_talk_exception_code(event_data);
  reason_item = root ? cJSON_GetObjectItem(root, "reason") : NULL;
  reason = (reason_item && cJSON_IsString(reason_item))
    ? reason_item->valuestring : NULL;

  enil_db_talk_exception_record(db, (long long)time(NULL),
                                code, reason, event_data);
  ENIL_LOG("ENILAccount.sse_event_core",
           "talkException code=%d reason=%s",
           code, reason ? reason : "(none)");

  result->should_stop_sse = 1;
  result->talk_exception_code = code;
  if (code == 117) {
    if (session_path)
      enil_session_set_reauth_needed(session_path);
    result->talk_action = ENIL_ACCOUNT_SSE_TALK_ACTION_REAUTH_NEEDED;
  } else if (code == 119 || code == 10051) {
    result->talk_action = ENIL_ACCOUNT_SSE_TALK_ACTION_REFRESH_RECONNECT;
  } else if (code == 8) {
    result->talk_action = ENIL_ACCOUNT_SSE_TALK_ACTION_SYNC_NEEDED;
  } else {
    result->talk_action = ENIL_ACCOUNT_SSE_TALK_ACTION_ERROR;
  }

  if (root) cJSON_Delete(root);
}

static void enil_account_sse_handle_partial_full_sync(const char *session_path,
                                                      const char *event_data,
                                                      enil_account_sse_result_t *result)
{
  cJSON *root;
  cJSON *targets;

  root = cJSON_Parse(event_data);
  targets = root ? cJSON_GetObjectItem(root, "targetCategories") : NULL;
  if (cJSON_IsObject(targets)) {
    int changed = enil_session_update_partial_full_syncs(session_path, targets);
    if (changed) {
      ENIL_LOG("ENILAccount.sse_event_core",
               "partialFullSync advanced; triggering resync");
      result->should_start_sync = 1;
    }
  }
  if (root) cJSON_Delete(root);
}

int enil_account_process_sse_event(enil_health_t *health,
                                   sqlite3 *db,
                                   const char *access_token,
                                   const char *my_mid,
                                   const char *session_path,
                                   const char *event_type,
                                   const char *event_data,
                                   const char *matched_temp_id,
                                   enil_account_sse_result_t *result)
{
  int op_handled = 0;

  if (!result) return 0;
  enil_account_sse_result_init(result);
  if (!db || !event_type || !event_data) return 0;

  if (health) enil_health_bind(health);

  if (strcmp(event_type, "message") == 0)
    enil_status_post("sse.message", "Received message", 1);

  enil_sync_process_sse_event(db, access_token, my_mid, session_path,
                              event_type, event_data, &op_handled);
  result->handled = (strcmp(event_type, "message") == 0)
    ? op_handled : enil_account_sse_control_handled(event_type);

  if (enil_health_any_failed()) {
    ENIL_LOG("ENILAccount.sse_event_core",
             "health failed (worker=%d line=%d) - stopping SSE",
             enil_health_is_failed(ENIL_ERR_WORKER),
             enil_health_is_failed(ENIL_ERR_LINE));
    result->should_stop_sse = 1;
  }

  if (strcmp(event_type, "message") == 0) {
    enil_account_sse_build_message_updates(db, my_mid, event_data,
                                           matched_temp_id, result);
  }

  if (strcmp(event_type, "talkException") == 0) {
    enil_account_sse_handle_talk_exception(db, session_path, event_data,
                                           result);
  }

  if (strcmp(event_type, "partialFullSync") == 0) {
    enil_account_sse_handle_partial_full_sync(session_path, event_data,
                                              result);
  }

  if (strcmp(event_type, "fullSync") == 0) {
    enil_session_reset_partial_full_syncs(session_path);
  }

  enil_account_record_sse_event(db, event_type, event_data, op_handled);
  return 1;
}
