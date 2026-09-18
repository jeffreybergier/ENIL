/* ============================================================================
 * Thin Thrift-over-JSON wrappers for LINE TalkService methods — getProfile,
 * getContacts, getChats, getRecentMessages, sendMessage, and friends.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "enil_line.h"
#include "enil_talkserv.h"
#include "enil_api_call.h"
#include "enil_api_json.h"
#include "enil_cocoa_log.h"

#define SYNC_REASON_OPERATION 3
#define BATCH_SIZE            100
/* Per-file shorthand for fixed-string call sites — routes through NSLog. */
#define LOG(fn, msg) enil_log("TalkService." fn, "%s", msg)

/* Thin wrapper that preserves the "TalkService.<fn>" log prefix; the OK-envelope
 * validation/detach logic lives once in enil_api_unwrap_ok (enil_api_call.c). */
static cJSON *parse_ok(ENILLineResponse *r, const char *fn) {
  char tag[96];
  snprintf(tag, sizeof(tag), "TalkService.%s", fn ? fn : "?");
  return enil_api_unwrap_ok(tag, r);
}

/* ============================================================================
 * Grow a typed result array to make room for one more element. Returns the
 * (possibly moved) base, or NULL on OOM; in that case the caller keeps the
 * old base to free. Doubling growth seeded at `init`. Shared by the contacts /
 * chats / message-box batch loops below, which differ only in element type,
 * seed size, and the per-loop cleanup they run on failure.
 * ==========================================================================*/
static void *grow_array(void *arr, int *cap, int n, size_t elem_size, int init) {
  int new_cap;
  void *tmp;
  if (n < *cap) return arr;
  new_cap = *cap ? *cap * 2 : init;
  tmp = realloc(arr, (size_t)new_cap * elem_size);
  if (!tmp) return NULL;
  *cap = new_cap;
  return tmp;
}

/* ============================================================================
 * TalkService.getProfile (typed)
 * ==========================================================================*/

static const enil_endpoint_t TALK_GET_PROFILE = {
  .name       = "TalkService.getProfile",
  .method     = ENIL_HTTP_POST,
  .path       = "/api/talk/thrift/Talk/TalkService/getProfile",
  .needs_hmac = 1,
  .needs_auth = 1
};

static cJSON *talk_get_profile_build(const void *req_ptr) {
  const talk_get_profile_req_t *req = (const talk_get_profile_req_t *)req_ptr;
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber(req ? req->sync_reason : 0));
  return params;
}

static int talk_get_profile_parse(cJSON *data, void *out_ptr) {
  talk_profile_t *out = (talk_profile_t *)out_ptr;
  cJSON *meta;
  if (!cJSON_IsObject(data)) return -1;
  if (enil_json_dup_string(data,     "mid",                 &out->mid)            < 0) return -1;
  enil_json_dup_string_opt(data,     "userid",              &out->user_id);
  enil_json_dup_string_opt(data,     "regionCode",          &out->region_code);
  enil_json_dup_string_opt(data,     "displayName",         &out->display_name);
  enil_json_dup_string_opt(data,     "statusMessage",       &out->status_message);
  enil_json_get_bool      (data,     "allowSearchByUserid", &out->allow_search_by_user_id);
  enil_json_get_bool      (data,     "allowSearchByEmail",  &out->allow_search_by_email);
  enil_json_dup_string_opt(data,     "picturePath",         &out->picture_path);
  enil_json_get_bool      (data,     "nftProfile",          &out->nft_profile);
  enil_json_dup_string_opt(data,     "profileId",           &out->profile_id);
  enil_json_get_int       (data,     "profileType",         &out->profile_type);
  enil_json_get_int64     (data,     "createdTimeMillis",   &out->created_time_millis);
  meta = cJSON_GetObjectItemCaseSensitive(data, "statusMessageContentMetadata");
  if (cJSON_IsObject(meta))
    out->status_message_content_metadata = cJSON_PrintUnformatted(meta);
  out->raw = cJSON_Duplicate(data, 1);
  return 0;
}

/* ============================================================================
 * TalkService.getProfile — fetches the logged-in user's profile struct.
 * Returns 0 on success.
 * ==========================================================================*/
int talk_get_profile(const char                   *access_token,
                     const talk_get_profile_req_t *req,
                     talk_profile_t               *out) {
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  return enil_api_call(&TALK_GET_PROFILE, access_token,
                       req,
                       talk_get_profile_build,
                       out,
                       talk_get_profile_parse);
}

/* ============================================================================
 * Release all malloc'd fields and raw cJSON inside a talk_profile_t.
 * ==========================================================================*/
void talk_profile_free(talk_profile_t *p) {
  if (!p) return;
  enil_field_free_all(p, talk_profile_fields, talk_profile_fields_count);
  if (p->raw) { cJSON_Delete(p->raw); p->raw = NULL; }
}

const enil_field_t talk_profile_fields[] = {
  { "mid",                          offsetof(talk_profile_t, mid),                             ENIL_F_STR  },
  { "userid",                       offsetof(talk_profile_t, user_id),                         ENIL_F_STR  },
  { "regionCode",                   offsetof(talk_profile_t, region_code),                     ENIL_F_STR  },
  { "displayName",                  offsetof(talk_profile_t, display_name),                    ENIL_F_STR  },
  { "statusMessage",                offsetof(talk_profile_t, status_message),                  ENIL_F_STR  },
  { "allowSearchByUserid",          offsetof(talk_profile_t, allow_search_by_user_id),         ENIL_F_BOOL },
  { "allowSearchByEmail",           offsetof(talk_profile_t, allow_search_by_email),           ENIL_F_BOOL },
  { "picturePath",                  offsetof(talk_profile_t, picture_path),                    ENIL_F_STR  },
  { "statusMessageContentMetadata", offsetof(talk_profile_t, status_message_content_metadata), ENIL_F_STR  },
  { "nftProfile",                   offsetof(talk_profile_t, nft_profile),                     ENIL_F_BOOL },
  { "profileId",                    offsetof(talk_profile_t, profile_id),                      ENIL_F_STR  },
  { "profileType",                  offsetof(talk_profile_t, profile_type),                    ENIL_F_INT  },
  { "createdTimeMillis",            offsetof(talk_profile_t, created_time_millis),             ENIL_F_I64  },
};
const size_t talk_profile_fields_count =
  sizeof(talk_profile_fields) / sizeof(talk_profile_fields[0]);

/* ============================================================================
 * TalkService.getContactsV2 (typed)
 * ==========================================================================*/

static cJSON *get_all_contact_ids(const char *access_token) {
  ENILLineResponse r;
  cJSON *data;
  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getAllContactIds",
    "[3]", access_token);
  data = parse_ok(&r, "getAllContactIds");
  enil_line_response_free(&r);
  if (!cJSON_IsArray(data)) {
    LOG("getAllContactIds", "expected array");
    cJSON_Delete(data);
    return NULL;
  }
  return data;
}

static cJSON *fetch_contacts_batch(cJSON *all_ids, int start,
                                   const char *access_token) {
  cJSON *params, *inner, *batch, *data;
  char *body;
  ENILLineResponse r;
  int total = cJSON_GetArraySize(all_ids);
  int end   = (start + BATCH_SIZE < total) ? start + BATCH_SIZE : total;
  int j;

  batch = cJSON_CreateArray();
  for (j = start; j < end; j++)
    cJSON_AddItemToArray(batch, cJSON_Duplicate(cJSON_GetArrayItem(all_ids, j), 1));

  params = cJSON_CreateArray();
  inner  = cJSON_CreateObject();
  cJSON_AddItemToObject(inner, "targetUserMids", batch);
  cJSON_AddItemToObject(inner, "neededContactCalendarEvents", cJSON_CreateArray());
  cJSON_AddItemToArray(params, inner);
  cJSON_AddItemToArray(params, cJSON_CreateNumber(SYNC_REASON_OPERATION));

  body = cJSON_PrintUnformatted(params);
  cJSON_Delete(params);
  if (!body) return NULL;

  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getContactsV2",
    body, access_token);
  free(body);

  data = parse_ok(&r, "getContactsV2");
  enil_line_response_free(&r);
  return data;
}

static int parse_contact_entry(cJSON *entry, talk_contact_t *out) {
  cJSON *contact = cJSON_GetObjectItem(entry, "contact");
  memset(out, 0, sizeof(*out));
  out->mid = entry->string ? strdup(entry->string) : NULL;
  if (!out->mid) return -1;
  if (!cJSON_IsObject(contact)) return 0; /* unregistered — mid only */
  out->registered             = 1;
  enil_json_dup_string_opt(contact, "displayName",           &out->display_name);
  enil_json_dup_string_opt(contact, "displayNameOverridden", &out->display_name_overridden);
  enil_json_dup_string_opt(contact, "statusMessage",         &out->status_message);
  enil_json_dup_string_opt(contact, "picturePath",           &out->picture_path);
  out->raw = cJSON_Duplicate(entry, 1);
  return 0;
}

/* ============================================================================
 * Fetch every contact via getAllContactIds + paged getContactsV3. Allocates
 * an array of talk_contact_t in *out and stores its size in *out_count.
 * ==========================================================================*/
int talk_get_contacts(const char *access_token,
                      talk_contact_t **out, int *out_count) {
  cJSON *all_ids, *batch_data;
  talk_contact_t *arr = NULL;
  int cap = 0, n = 0, total, i;

  if (!access_token || !out || !out_count) return -1;
  *out = NULL; *out_count = 0;

  all_ids = get_all_contact_ids(access_token);
  if (!all_ids) return -1;

  total = cJSON_GetArraySize(all_ids);
  ENIL_LOG("TalkService.getContacts", "%d contact IDs", total);

  for (i = 0; i < total; i += BATCH_SIZE) {
    cJSON *map, *entry;
    batch_data = fetch_contacts_batch(all_ids, i, access_token);
    if (!batch_data) continue;
    map = cJSON_GetObjectItem(batch_data, "contacts");
    if (!cJSON_IsObject(map)) { cJSON_Delete(batch_data); continue; }
    for (entry = map->child; entry; entry = entry->next) {
      if (n == cap) {
        void *tmp = grow_array(arr, &cap, n, sizeof(*arr), 16);
        if (!tmp) { cJSON_Delete(batch_data); goto done; }
        arr = tmp;
      }
      parse_contact_entry(entry, &arr[n]);
      n++;
    }
    cJSON_Delete(batch_data);
  }

done:
  cJSON_Delete(all_ids);
  *out = arr; *out_count = n;
  return 0;
}

/* ============================================================================
 * Fetch a single contact by mid via getContactsV2 with a 1-element
 * targetUserMids array. Used by the op-49 SSE handler when a peer profile
 * changes. Returns 0 on success; caller frees via talk_contact_free.
 * ==========================================================================*/
int talk_get_contact_by_mid(const char     *access_token,
                            const char     *mid,
                            talk_contact_t *out) {
  cJSON           *params, *inner, *mids_arr, *data, *map, *entry;
  char            *body;
  ENILLineResponse r;
  int              rc = -1;

  if (!access_token || !mid || !mid[0] || !out) return -1;
  memset(out, 0, sizeof(*out));

  mids_arr = cJSON_CreateArray();
  cJSON_AddItemToArray(mids_arr, cJSON_CreateString(mid));
  inner = cJSON_CreateObject();
  cJSON_AddItemToObject(inner, "targetUserMids", mids_arr);
  cJSON_AddItemToObject(inner, "neededContactCalendarEvents", cJSON_CreateArray());
  params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, inner);
  cJSON_AddItemToArray(params, cJSON_CreateNumber(SYNC_REASON_OPERATION));

  body = cJSON_PrintUnformatted(params);
  cJSON_Delete(params);
  if (!body) return -1;

  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getContactsV2",
    body, access_token);
  free(body);

  data = parse_ok(&r, "getContactsV2");
  enil_line_response_free(&r);
  if (!data) return -1;

  map = cJSON_GetObjectItem(data, "contacts");
  if (cJSON_IsObject(map)) {
    entry = cJSON_GetObjectItem(map, mid);
    if (cJSON_IsObject(entry))
      rc = parse_contact_entry(entry, out);
  }
  cJSON_Delete(data);
  return rc;
}

/* ============================================================================
 * TalkService.getAllChatMids (typed)
 * ==========================================================================*/

static int dup_string_array(cJSON *arr, char ***out, int *out_count) {
  int n, i, count = 0;
  char **buf;
  *out = NULL; *out_count = 0;
  if (!cJSON_IsArray(arr)) return 0;
  n = cJSON_GetArraySize(arr);
  if (n <= 0) return 0;
  buf = (char **)calloc((size_t)n, sizeof(char *));
  if (!buf) return -1;
  for (i = 0; i < n; i++) {
    cJSON *item = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsString(item) || !item->valuestring) continue;
    buf[count] = strdup(item->valuestring);
    if (!buf[count]) {
      while (count-- > 0) free(buf[count]);
      free(buf);
      return -1;
    }
    count++;
  }
  *out = buf; *out_count = count;
  return 0;
}

/* ============================================================================
 * Release the member/invited chat-mid arrays and raw JSON inside the struct.
 * ==========================================================================*/
void talk_get_all_chat_mids_free(talk_get_all_chat_mids_t *p) {
  int i;
  if (!p) return;
  for (i = 0; i < p->memberCount; i++)  free(p->memberChatMids[i]);
  for (i = 0; i < p->invitedCount; i++) free(p->invitedChatMids[i]);
  free(p->memberChatMids);  p->memberChatMids = NULL;  p->memberCount = 0;
  free(p->invitedChatMids); p->invitedChatMids = NULL; p->invitedCount = 0;
  if (p->raw) { cJSON_Delete(p->raw); p->raw = NULL; }
}

/* ============================================================================
 * TalkService.getAllChatMids — list of joined and invited chat mids.
 * Returns 0 on success.
 * ==========================================================================*/
int talk_get_all_chat_mids(const char                         *access_token,
                           const talk_get_all_chat_mids_req_t *req,
                           talk_get_all_chat_mids_t           *out) {
  cJSON *params, *opts, *data;
  char  *body;
  ENILLineResponse r;

  if (!access_token || !req || !out) return -1;
  memset(out, 0, sizeof(*out));

  opts = cJSON_CreateObject();
  cJSON_AddBoolToObject(opts, "withMemberChats",  req->withMemberChats  ? 1 : 0);
  cJSON_AddBoolToObject(opts, "withInvitedChats", req->withInvitedChats ? 1 : 0);
  params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, opts);
  cJSON_AddItemToArray(params, cJSON_CreateNumber(SYNC_REASON_OPERATION));
  body = cJSON_PrintUnformatted(params);
  cJSON_Delete(params);
  if (!body) return -1;

  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getAllChatMids",
    body, access_token);
  free(body);
  data = parse_ok(&r, "getAllChatMids");
  enil_line_response_free(&r);
  if (!cJSON_IsObject(data)) { cJSON_Delete(data); return -1; }

  if (dup_string_array(cJSON_GetObjectItem(data, "memberChatMids"),
                       &out->memberChatMids,  &out->memberCount) < 0 ||
      dup_string_array(cJSON_GetObjectItem(data, "invitedChatMids"),
                       &out->invitedChatMids, &out->invitedCount) < 0) {
    cJSON_Delete(data);
    talk_get_all_chat_mids_free(out);
    return -1;
  }
  out->raw = data;
  ENIL_LOG("TalkService.getAllChatMids", "%d member, %d invited", out->memberCount, out->invitedCount);
  return 0;
}

/* ============================================================================
 * TalkService.getChats (typed, batched)
 * ==========================================================================*/

static int parse_chat_entry(cJSON *entry, talk_chat_t *out) {
  cJSON *extra, *group_extra;
  memset(out, 0, sizeof(*out));
  if (enil_json_dup_string(entry, "chatMid", &out->chatMid) < 0) return -1;
  enil_json_get_int64(entry, "createdTime",       &out->createdTime);
  enil_json_get_bool (entry, "notificationDisabled", &out->notificationDisabled);
  enil_json_get_int64(entry, "favoriteTimestamp", &out->favoriteTimestamp);
  enil_json_dup_string_opt(entry, "chatName",    &out->chatName);
  enil_json_dup_string_opt(entry, "picturePath", &out->picturePath);
  enil_json_get_int  (entry, "type",             &out->type);

  extra = cJSON_GetObjectItemCaseSensitive(entry, "extra");
  if (cJSON_IsObject(extra)) {
    group_extra = cJSON_GetObjectItemCaseSensitive(extra, "groupExtra");
    if (cJSON_IsObject(group_extra)) {
      enil_json_dup_string_opt(group_extra, "creatorMid",       &out->creatorMid);
      enil_json_dup_string_opt(group_extra, "invitationTicket", &out->invitationTicket);
    }
  }
  out->raw = cJSON_Duplicate(entry, 1);
  return 0;
}

static cJSON *fetch_chats_batch(const char *const *mids, int start, int end,
                                const char *access_token) {
  cJSON *params, *opts, *batch, *data;
  char *body;
  ENILLineResponse r;
  int j;

  batch = cJSON_CreateArray();
  for (j = start; j < end; j++)
    cJSON_AddItemToArray(batch, cJSON_CreateString(mids[j]));

  opts = cJSON_CreateObject();
  cJSON_AddItemToObject(opts, "chatMids",     batch);
  cJSON_AddBoolToObject(opts, "withMembers",  1);
  cJSON_AddBoolToObject(opts, "withInvitees", 1);
  params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, opts);
  cJSON_AddItemToArray(params, cJSON_CreateNumber(SYNC_REASON_OPERATION));
  body = cJSON_PrintUnformatted(params);
  cJSON_Delete(params);
  if (!body) return NULL;

  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getChats",
    body, access_token);
  free(body);
  data = parse_ok(&r, "getChats");
  enil_line_response_free(&r);
  return data;
}

/* ============================================================================
 * TalkService.getChats — fetches typed metadata for each chat mid in batches.
 * An empty chats array is a successful lookup with no accessible matches.
 * Failed or malformed batches must not masquerade as missing chats.
 * ==========================================================================*/
int talk_get_chats(const char *access_token,
                   const char *const *chat_mids, int count,
                   talk_chat_t **out, int *out_count) {
  talk_chat_t *arr = NULL;
  int cap = 0, n = 0, i;

  if (!access_token || !out || !out_count) return -1;
  *out = NULL; *out_count = 0;
  if (!chat_mids || count <= 0) return 0;

  for (i = 0; i < count; i += BATCH_SIZE) {
    int end = (i + BATCH_SIZE < count) ? i + BATCH_SIZE : count;
    cJSON *data = fetch_chats_batch(chat_mids, i, end, access_token);
    cJSON *chats_arr;
    int j, m;
    if (!cJSON_IsObject(data)) { cJSON_Delete(data); goto failed; }
    chats_arr = cJSON_GetObjectItem(data, "chats");
    if (!cJSON_IsArray(chats_arr)) { cJSON_Delete(data); goto failed; }
    m = cJSON_GetArraySize(chats_arr);
    for (j = 0; j < m; j++) {
      cJSON *entry = cJSON_GetArrayItem(chats_arr, j);
      if (n == cap) {
        void *tmp = grow_array(arr, &cap, n, sizeof(*arr), 16);
        if (!tmp) { cJSON_Delete(data); goto failed; }
        arr = tmp;
      }
      if (parse_chat_entry(entry, &arr[n]) != 0) {
        talk_chat_free(&arr[n]);
        cJSON_Delete(data);
        goto failed;
      }
      n++;
    }
    cJSON_Delete(data);
  }

  *out = arr; *out_count = n;
  ENIL_LOG("TalkService.getChats", "%d chats", n);
  return 0;
failed:
  for (i = 0; i < n; i++) talk_chat_free(&arr[i]);
  free(arr);
  return -1;
}

/* ============================================================================
 * Release malloc'd fields and raw cJSON inside a talk_chat_t.
 * ==========================================================================*/
void talk_chat_free(talk_chat_t *c) {
  if (!c) return;
  enil_field_free_all(c, talk_chat_fields, talk_chat_fields_count);
  if (c->raw) { cJSON_Delete(c->raw); c->raw = NULL; }
}

const enil_field_t talk_chat_fields[] = {
  { "chatMid",              offsetof(talk_chat_t, chatMid),              ENIL_F_STR  },
  { "createdTime",          offsetof(talk_chat_t, createdTime),          ENIL_F_I64  },
  { "notificationDisabled", offsetof(talk_chat_t, notificationDisabled), ENIL_F_BOOL },
  { "favoriteTimestamp",    offsetof(talk_chat_t, favoriteTimestamp),    ENIL_F_I64  },
  { "chatName",             offsetof(talk_chat_t, chatName),             ENIL_F_STR  },
  { "picturePath",          offsetof(talk_chat_t, picturePath),          ENIL_F_STR  },
  { "type",                 offsetof(talk_chat_t, type),                 ENIL_F_INT  },
  { "creatorMid",           offsetof(talk_chat_t, creatorMid),           ENIL_F_STR  },
  { "invitationTicket",     offsetof(talk_chat_t, invitationTicket),     ENIL_F_STR  },
  { "isInvited",            offsetof(talk_chat_t, isInvited),            ENIL_F_BOOL },
};
const size_t talk_chat_fields_count =
  sizeof(talk_chat_fields) / sizeof(talk_chat_fields[0]);

/* ============================================================================
 * TalkService.getMessageBoxes (typed, paginated)
 * ==========================================================================*/

static cJSON *build_message_boxes_body(const char *min_chat_id) {
  cJSON *params = cJSON_CreateArray();
  cJSON *opts   = cJSON_CreateObject();
  cJSON_AddBoolToObject  (opts, "activeOnly",                       1);
  cJSON_AddBoolToObject  (opts, "unreadOnly",                       0);
  cJSON_AddNumberToObject(opts, "messageBoxCountLimit",             BATCH_SIZE);
  cJSON_AddBoolToObject  (opts, "withUnreadCount",                  1);
  cJSON_AddNumberToObject(opts, "lastMessagesPerMessageBoxCount",   1);
  if (min_chat_id) cJSON_AddStringToObject(opts, "minChatId", min_chat_id);
  cJSON_AddItemToArray(params, opts);
  cJSON_AddItemToArray(params, cJSON_CreateNumber(SYNC_REASON_OPERATION));
  return params;
}

static int parse_message_box_entry(cJSON *entry, talk_message_box_t *out) {
  cJSON *ldi;
  memset(out, 0, sizeof(*out));
  if (enil_json_dup_string(entry, "id", &out->id) < 0) return -1;
  enil_json_get_int  (entry, "midType",           &out->midType);
  enil_json_dup_string_opt(entry, "lastSeenMessageId", &out->lastSeenMessageId);
  enil_json_get_int64(entry, "unreadCount",       &out->unreadCount);
  ldi = cJSON_GetObjectItemCaseSensitive(entry, "lastDeliveredMessageId");
  if (cJSON_IsObject(ldi)) {
    enil_json_dup_string_opt(ldi, "messageId",    &out->lastDeliveredMessageId);
    enil_json_get_int64     (ldi, "deliveredTime", &out->lastDeliveredTime);
  }
  out->raw = cJSON_Duplicate(entry, 1);
  return 0;
}

/* ============================================================================
 * TalkService.getMessageBoxes — paginates active message boxes ordered by id,
 * returning unread counts and last-seen markers.
 * ==========================================================================*/
int talk_get_message_boxes(const char *access_token,
                           talk_message_box_t **out, int *out_count) {
  talk_message_box_t *arr = NULL;
  char  *min_chat_id = NULL;
  int    cap = 0, n = 0, has_next = 1, failed = 0;

  if (!access_token || !out || !out_count) return -1;
  *out = NULL; *out_count = 0;

  while (has_next) {
    cJSON *params, *data, *boxes, *last, *last_id;
    char  *body;
    ENILLineResponse r;
    int    m, j;

    params = build_message_boxes_body(min_chat_id);
    body   = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (!body) { failed = 1; break; }

    r = enil_line_post(
      "/api/talk/thrift/Talk/TalkService/getMessageBoxes",
      body, access_token);
    free(body);
    data = parse_ok(&r, "getMessageBoxes");
    enil_line_response_free(&r);
    if (!data) { failed = 1; break; }

    boxes    = cJSON_GetObjectItem(data, "messageBoxes");
    has_next = cJSON_IsTrue(cJSON_GetObjectItem(data, "hasNext"));
    if (!cJSON_IsArray(boxes)) {
      cJSON_Delete(data);
      failed = 1;
      break;
    }
    m        = cJSON_IsArray(boxes) ? cJSON_GetArraySize(boxes) : 0;

    for (j = 0; j < m; j++) {
      cJSON *entry = cJSON_GetArrayItem(boxes, j);
      if (n == cap) {
        void *tmp = grow_array(arr, &cap, n, sizeof(*arr), 32);
        if (!tmp) { failed = 1; break; }
        arr = tmp;
      }
      if (parse_message_box_entry(entry, &arr[n]) != 0) { failed = 1; break; }
      n++;
    }

    if (failed) { cJSON_Delete(data); break; }
    /* A partial list must not masquerade as a complete current chat list. */
    if (has_next && (m == 0 ||
        (min_chat_id && !strcmp(min_chat_id, arr[n - 1].id)))) {
      cJSON_Delete(data);
      failed = 1;
      break;
    }
    free(min_chat_id);
    min_chat_id = NULL;
    if (has_next && m > 0) {
      last    = cJSON_GetArrayItem(boxes, m - 1);
      last_id = cJSON_GetObjectItem(last, "id");
      if (cJSON_IsString(last_id) && last_id->valuestring)
        min_chat_id = strdup(last_id->valuestring);
      if (!min_chat_id) failed = 1;
    }
    cJSON_Delete(data);
    if (failed) break;
  }
  free(min_chat_id);

  if (failed) {
    int i;
    for (i = 0; i < n; i++) talk_message_box_free(&arr[i]);
    free(arr);
    return -1;
  }

  *out = arr; *out_count = n;
  ENIL_LOG("TalkService.getMessageBoxes", "%d boxes", n);
  return 0;
}

/* ============================================================================
 * Release malloc'd fields inside a talk_message_box_t.
 * ==========================================================================*/
void talk_message_box_free(talk_message_box_t *mb) {
  if (!mb) return;
  enil_field_free_all(mb, talk_message_box_fields, talk_message_box_fields_count);
  if (mb->raw) { cJSON_Delete(mb->raw); mb->raw = NULL; }
}

const enil_field_t talk_message_box_fields[] = {
  { "id",                      offsetof(talk_message_box_t, id),                     ENIL_F_STR },
  { "midType",                 offsetof(talk_message_box_t, midType),                ENIL_F_INT },
  { "lastDeliveredMessageId",  offsetof(talk_message_box_t, lastDeliveredMessageId), ENIL_F_STR },
  { "lastDeliveredTime",       offsetof(talk_message_box_t, lastDeliveredTime),      ENIL_F_I64 },
  { "lastSeenMessageId",       offsetof(talk_message_box_t, lastSeenMessageId),      ENIL_F_STR },
  { "unreadCount",             offsetof(talk_message_box_t, unreadCount),            ENIL_F_I64 },
};
const size_t talk_message_box_fields_count =
  sizeof(talk_message_box_fields) / sizeof(talk_message_box_fields[0]);

/* ============================================================================
 * TalkService.getRecentMessagesV2 (typed)
 * ==========================================================================*/

/* ============================================================================
 * TalkService.getRecentMessagesV2 — fetch the last `count` messages for a
 * chat. Returns allocated talk_message_t array via *out.
 * ==========================================================================*/
int talk_get_recent_messages(const char *access_token, const char *chat_id, int count,
                             talk_message_t **out, int *out_count) {
  cJSON *params, *data;
  char *body;
  ENILLineResponse r;
  talk_message_t *arr = NULL;
  int n = 0, m, j;

  if (!access_token || !chat_id || !out || !out_count) return -1;
  *out = NULL; *out_count = 0;

  params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(chat_id));
  cJSON_AddItemToArray(params, cJSON_CreateNumber(count));
  body = cJSON_PrintUnformatted(params);
  cJSON_Delete(params);
  if (!body) return -1;

  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getRecentMessagesV2",
    body, access_token);
  free(body);

  data = parse_ok(&r, "getRecentMessages");
  enil_line_response_free(&r);
  if (!cJSON_IsArray(data)) {
    LOG("getRecentMessages", "expected array");
    cJSON_Delete(data);
    return -1;
  }

  m = cJSON_GetArraySize(data);
  if (m > 0) {
    arr = (talk_message_t *)calloc((size_t)m, sizeof(*arr));
    if (!arr) { cJSON_Delete(data); return -1; }
  }
  for (j = 0; j < m; j++) {
    cJSON *entry = cJSON_GetArrayItem(data, j);
    if (talk_message_parse(entry, chat_id, &arr[n]) == 0) n++;
  }
  cJSON_Delete(data);

  *out = arr; *out_count = n;
  ENIL_LOG("TalkService.getRecentMessages", "%s -> %d messages", chat_id, n);
  return 0;
}

/* ============================================================================
 * TalkService.getMessageReadRange — batched peer-read-state fetch.
 * Wire body is [[chat_mids...], 1]. The second argument is observed as
 * constant `1` in captured Chrome traffic; treated as opaque here. Returns the
 * raw "data" cJSON array so the sync layer can walk the reader_mid -> ranges
 * map directly without imposing a typed-struct shape on irregular data.
 * ==========================================================================*/
cJSON *talk_get_message_read_range(const char        *access_token,
                                   const char *const *chat_mids,
                                   int                count) {
  cJSON            *params, *ids, *data;
  char             *body;
  ENILLineResponse  r;
  int               i;

  if (!access_token || !chat_mids || count <= 0) return NULL;

  ids = cJSON_CreateArray();
  if (!ids) return NULL;
  for (i = 0; i < count; i++) {
    if (chat_mids[i] && chat_mids[i][0])
      cJSON_AddItemToArray(ids, cJSON_CreateString(chat_mids[i]));
  }
  if (cJSON_GetArraySize(ids) == 0) { cJSON_Delete(ids); return NULL; }

  params = cJSON_CreateArray();
  if (!params) { cJSON_Delete(ids); return NULL; }
  cJSON_AddItemToArray(params, ids);
  cJSON_AddItemToArray(params, cJSON_CreateNumber(1));
  body = cJSON_PrintUnformatted(params);
  cJSON_Delete(params);
  if (!body) return NULL;

  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getMessageReadRange",
    body, access_token);
  free(body);

  data = parse_ok(&r, "getMessageReadRange");
  enil_line_response_free(&r);
  if (!cJSON_IsArray(data)) {
    LOG("getMessageReadRange", "expected array");
    cJSON_Delete(data);
    return NULL;
  }
  ENIL_LOG("TalkService.getMessageReadRange",
           "%d chat(s) -> %d entries", count, cJSON_GetArraySize(data));
  return data;
}

/* ============================================================================
 * TalkService.sendMessage — body serializer + response parser
 * The request wire shape is [reqSeq, Message]; the response is a Message.
 * Both halves share talk_message_t.
 * ==========================================================================*/

/* ============================================================================
 * Serialise a TalkService.sendMessage request to the wire JSON body
 * `[reqSeq, Message]`. Returns malloc'd string.
 * ==========================================================================*/
char *talk_send_message_body_build(const talk_send_message_req_t *req) {
  const talk_message_t *m;
  cJSON  *arr, *msg, *meta_copy, *chunks_copy;
  char    created_str[32];
  char   *result;

  if (!req) return NULL;
  m = &req->message;

  msg = cJSON_CreateObject();
  if (!msg) return NULL;

  if (m->from_mid) cJSON_AddStringToObject(msg, "from", m->from_mid);
  if (m->to_mid)   cJSON_AddStringToObject(msg, "to",   m->to_mid);
  cJSON_AddNumberToObject(msg, "toType", m->toType);
  if (m->id)       cJSON_AddStringToObject(msg, "id",   m->id);

  /* LINE expects createdTime as a JSON string (json.Number numeric). */
  snprintf(created_str, sizeof(created_str), "%lld", m->createdTime);
  cJSON_AddStringToObject(msg, "createdTime", created_str);

  cJSON_AddNumberToObject(msg, "sessionId",   m->sessionId);
  cJSON_AddNumberToObject(msg, "contentType", m->contentType);
  if (m->text) cJSON_AddStringToObject(msg, "text", m->text);

  /* contentMetadata is always emitted (empty object when absent), matching
   * the legacy builders' behaviour. */
  meta_copy = m->contentMetadata
    ? cJSON_Duplicate(m->contentMetadata, 1)
    : cJSON_CreateObject();
  cJSON_AddItemToObject(msg, "contentMetadata", meta_copy);

  if (m->chunks) {
    chunks_copy = cJSON_Duplicate(m->chunks, 1);
    cJSON_AddItemToObject(msg, "chunks", chunks_copy);
  }

  cJSON_AddBoolToObject(msg, "hasContent", m->hasContent);

  if (m->relatedMessageId)
    cJSON_AddStringToObject(msg, "relatedMessageId", m->relatedMessageId);
  if (m->messageRelationType)
    cJSON_AddNumberToObject(msg, "messageRelationType", m->messageRelationType);
  if (m->relatedMessageServiceCode)
    cJSON_AddNumberToObject(msg, "relatedMessageServiceCode",
                            m->relatedMessageServiceCode);

  arr = cJSON_CreateArray();
  cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)req->reqSeq));
  cJSON_AddItemToArray(arr, msg);
  result = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  return result;
}

/* ============================================================================
 * Parse the talk_message_t returned by a successful sendMessage call.
 * ==========================================================================*/
int talk_send_message_response_parse(cJSON *data, talk_message_t *out) {
  if (!data || !out) return -1;
  return talk_message_parse(data, NULL, out);
}

/* ============================================================================
 * ShopService.getOwnedProductSummaries (typed, per-shop)
 *
 * The wire payload is shared between stickershop and sticonshop. Both
 * `sticker_package_fetch_all` and `sticon_package_fetch_all` call the same
 * RPC with their shop name, then parse-split the productList into typed
 * rows that match their target table.
 * ==========================================================================*/

static cJSON *fetch_owned_product_list(const char *access_token, const char *shop) {
  static const char *path =
    "/api/shop/thrift/ShopService/ShopService/getOwnedProductSummaries";
  char body[128];
  ENILLineResponse r;
  cJSON *root, *data, *list;

  if (!access_token || !shop) return NULL;
  snprintf(body, sizeof(body),
           "[\"%s\",0,1000,{\"language\":\"en\",\"country\":\"JP\"}]", shop);

  r = enil_line_post(path, body, access_token);
  if (r.status != 200) {
    ENIL_LOG("ShopService.getOwnedProductSummaries", "%s HTTP %ld", shop, r.status);
    enil_line_response_free(&r);
    return NULL;
  }
  root = cJSON_Parse(r.body);
  enil_line_response_free(&r);
  if (!root) {
    ENIL_LOG("ShopService.getOwnedProductSummaries", "%s: invalid JSON", shop);
    return NULL;
  }
  data = cJSON_GetObjectItem(root, "data");
  list = cJSON_GetObjectItem(data ? data : root, "productList");
  if (!cJSON_IsArray(list)) {
    ENIL_LOG("ShopService.getOwnedProductSummaries", "%s: no productList", shop);
    cJSON_Delete(root);
    return NULL;
  }
  cJSON_DetachItemFromObject(data ? data : root, "productList");
  cJSON_Delete(root);
  ENIL_LOG("ShopService.getOwnedProductSummaries", "%s: %d products", shop, cJSON_GetArraySize(list));
  return list;
}

static void parse_sticker_package_entry(cJSON *prod, sticker_package_t *out) {
  cJSON *type_sum, *stk_sum, *promo;
  memset(out, 0, sizeof(*out));
  enil_json_dup_string_opt(prod, "id",             &out->id);
  enil_json_dup_string_opt(prod, "name",           &out->name);
  enil_json_dup_string_opt(prod, "latestVersion",  &out->latestVersion);
  enil_json_get_bool      (prod, "grantedByDefault", &out->grantedByDefault);
  enil_json_dup_string_opt(prod, "authorId",       &out->authorId);
  out->installedTime =
    enil_json_coerce_int64(cJSON_GetObjectItem(prod, "installedTime"));
  enil_json_dup_string_opt(prod, "validUntil",     &out->validUntil);
  enil_json_get_int       (prod, "validFor",       &out->validFor);
  enil_json_get_int       (prod, "availability",   &out->availability);
  enil_json_get_bool      (prod, "canAutoDownload", &out->canAutoDownload);
  promo = cJSON_GetObjectItem(prod, "promotionInfo");
  if (cJSON_IsObject(promo)) enil_json_get_int(promo, "promotionType", &out->promotionType);

  type_sum = cJSON_GetObjectItem(prod, "productTypeSummary");
  stk_sum  = cJSON_IsObject(type_sum) ? cJSON_GetObjectItem(type_sum, "stickerSummary") : NULL;
  if (cJSON_IsObject(stk_sum)) {
    enil_json_get_int       (stk_sum, "stickerResourceType",      &out->stickerResourceType);
    enil_json_get_int       (stk_sum, "stickerSize",              &out->stickerSize);
    enil_json_dup_string_opt(stk_sum, "suggestVersion",           &out->suggestVersion);
    enil_json_get_bool      (stk_sum, "defaultDisplayOnKeyboard", &out->defaultDisplayOnKeyboard);
    enil_json_get_bool      (stk_sum, "availableForPhotoEdit",    &out->availableForPhotoEdit);
    {
      cJSON *ranges = cJSON_GetObjectItem(stk_sum, "stickerIdRanges");
      if (cJSON_IsArray(ranges)) out->stickerIdRanges = cJSON_Duplicate(ranges, 1);
    }
  }
  out->isPurchased = 1;
  out->raw = cJSON_Duplicate(prod, 1);
}

static void parse_sticon_package_entry(cJSON *prod, sticon_package_t *out) {
  cJSON *type_sum, *sti_sum, *avr, *promo;
  memset(out, 0, sizeof(*out));
  enil_json_dup_string_opt(prod, "id",             &out->id);
  enil_json_dup_string_opt(prod, "name",           &out->name);
  enil_json_dup_string_opt(prod, "latestVersion",  &out->latestVersion);
  enil_json_get_bool      (prod, "grantedByDefault", &out->grantedByDefault);
  enil_json_dup_string_opt(prod, "authorId",       &out->authorId);
  out->installedTime =
    enil_json_coerce_int64(cJSON_GetObjectItem(prod, "installedTime"));
  enil_json_dup_string_opt(prod, "validUntil",     &out->validUntil);
  enil_json_get_int       (prod, "validFor",       &out->validFor);
  enil_json_get_int       (prod, "availability",   &out->availability);
  enil_json_get_bool      (prod, "canAutoDownload", &out->canAutoDownload);
  promo = cJSON_GetObjectItem(prod, "promotionInfo");
  if (cJSON_IsObject(promo)) enil_json_get_int(promo, "promotionType", &out->promotionType);

  type_sum = cJSON_GetObjectItem(prod, "productTypeSummary");
  sti_sum  = cJSON_IsObject(type_sum) ? cJSON_GetObjectItem(type_sum, "sticonSummary") : NULL;
  if (cJSON_IsObject(sti_sum)) {
    enil_json_get_int       (sti_sum, "sticonResourceType",    &out->sticonResourceType);
    enil_json_dup_string_opt(sti_sum, "suggestVersion",        &out->suggestVersion);
    enil_json_get_bool      (sti_sum, "availableForPhotoEdit", &out->availableForPhotoEdit);
  }
  avr = cJSON_GetObjectItem(prod, "applicationVersionRange");
  if (cJSON_IsObject(avr)) out->applicationVersionRange = cJSON_Duplicate(avr, 1);
  out->isPurchased = 1;
  out->raw = cJSON_Duplicate(prod, 1);
}

/* ============================================================================
 * ShopService.getOwnedProductSummaries against stickershop. Returns the
 * caller's purchased sticker packages as a typed array.
 * ==========================================================================*/
int sticker_package_fetch_all(const char *access_token,
                              sticker_package_t **out, int *out_count) {
  cJSON *list;
  sticker_package_t *arr = NULL;
  int np, j, n = 0;

  if (!out || !out_count) return -1;
  *out = NULL; *out_count = 0;
  if (!access_token) return -1;

  list = fetch_owned_product_list(access_token, "stickershop");
  np   = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
  if (np > 0) {
    arr = (sticker_package_t *)calloc((size_t)np, sizeof(*arr));
    if (!arr) { cJSON_Delete(list); return -1; }
  }
  for (j = 0; j < np; j++) {
    cJSON *prod = cJSON_GetArrayItem(list, j);
    if (!cJSON_IsObject(prod)) continue;
    parse_sticker_package_entry(prod, &arr[n]);
    if (arr[n].id) n++;
    else           sticker_package_free(&arr[n]);
  }
  cJSON_Delete(list);
  *out = arr; *out_count = n;
  return 0;
}

/* ============================================================================
 * ShopService.getOwnedProductSummaries against sticonshop. Returns the
 * caller's purchased sticon (inline emoji) packages.
 * ==========================================================================*/
int sticon_package_fetch_all(const char *access_token,
                             sticon_package_t **out, int *out_count) {
  cJSON *list;
  sticon_package_t *arr = NULL;
  int np, j, n = 0;

  if (!out || !out_count) return -1;
  *out = NULL; *out_count = 0;
  if (!access_token) return -1;

  list = fetch_owned_product_list(access_token, "sticonshop");
  np   = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
  if (np > 0) {
    arr = (sticon_package_t *)calloc((size_t)np, sizeof(*arr));
    if (!arr) { cJSON_Delete(list); return -1; }
  }
  for (j = 0; j < np; j++) {
    cJSON *prod = cJSON_GetArrayItem(list, j);
    if (!cJSON_IsObject(prod)) continue;
    parse_sticon_package_entry(prod, &arr[n]);
    if (arr[n].id) n++;
    else           sticon_package_free(&arr[n]);
  }
  cJSON_Delete(list);
  *out = arr; *out_count = n;
  return 0;
}

/* ============================================================================
 * TalkService.getLastOpRevision — seeds the SSE localRev parameter.
 * Returns the revision number or -1 on failure.
 * ==========================================================================*/
long long TalkService_getLastOpRevision(const char *access_token) {
  ENILLineResponse r;
  cJSON *data;
  long long rev = -1;
  if (!access_token) { LOG("getLastOpRevision", "NULL access_token"); return -1; }
  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getLastOpRevision",
    "[]", access_token);
  data = parse_ok(&r, "getLastOpRevision");
  enil_line_response_free(&r);
  if (cJSON_IsString(data) || cJSON_IsNumber(data))
    rev = enil_json_coerce_int64(data);
  cJSON_Delete(data);
  if (rev < 0) LOG("getLastOpRevision", "expected revision");
  else ENIL_LOG("TalkService.getLastOpRevision", "localRev=%lld", rev);
  return rev;
}

/* ============================================================================
 * Tiny POST helper for fire-and-forget RPCs (read receipt, unsend, leave-chat).
 * Serialises `params` to JSON, POSTs to `path`, validates the OK envelope, and
 * throws the result away. Returns 0 on success.
 * ==========================================================================*/
static int post_void_rpc(const char *path, cJSON *params,
                         const char *access_token, const char *fn) {
  char *body;
  cJSON *root, *msg;
  ENILLineResponse r;
  int ok = -1;
  if (!path || !params || !access_token) {
    cJSON_Delete(params);
    return -1;
  }
  body = cJSON_PrintUnformatted(params);
  cJSON_Delete(params);
  if (!body) return -1;
  r = enil_line_post(path, body, access_token);
  free(body);
  if (r.status != 200) {
    char tag[96];
    snprintf(tag, sizeof(tag), "TalkService.%s", fn ? fn : "?");
    ENIL_LOG(tag, "HTTP %ld", r.status);
    enil_line_response_free(&r);
    return -1;
  }
  root = r.body ? cJSON_Parse(r.body) : NULL;
  msg  = root ? cJSON_GetObjectItem(root, "message") : NULL;
  if (msg && cJSON_IsString(msg) && strcmp(msg->valuestring, "OK") == 0) ok = 0;
  else {
    char tag[96];
    snprintf(tag, sizeof(tag), "TalkService.%s", fn ? fn : "?");
    ENIL_LOG(tag, "non-OK: %s", r.body ? r.body : "");
  }
  cJSON_Delete(root);
  enil_line_response_free(&r);
  return ok;
}

/* ============================================================================
 * TalkService.sendChatChecked — mark a message as the read marker for a chat.
 * Wire shape: [0, chatMid, messageId]. The leading 0 is the unused reqSeq
 * slot (matrix-line `methods.go:362`).
 * ==========================================================================*/
int talk_send_chat_checked(const char *access_token,
                           const char *chat_mid,
                           const char *message_id) {
  cJSON *params;
  if (!access_token || !chat_mid || !message_id) {
    LOG("sendChatChecked", "NULL argument");
    return -1;
  }
  params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber(0));
  cJSON_AddItemToArray(params, cJSON_CreateString(chat_mid));
  cJSON_AddItemToArray(params, cJSON_CreateString(message_id));
  return post_void_rpc(
    "/api/talk/thrift/Talk/TalkService/sendChatChecked",
    params, access_token, "sendChatChecked");
}

/* ============================================================================
 * TalkService.sendChatRemoved — leave/delete a chat. Wire shape:
 * [reqSeq, chatMid, lastReadMessageId, lastReadMessageTime].
 * matrix-line `methods.go:554`.
 * ==========================================================================*/
int talk_send_chat_removed(const char *access_token,
                           long long   req_seq,
                           const char *chat_mid,
                           const char *last_read_message_id,
                           long long   last_read_message_time) {
  cJSON *params;
  if (!access_token || !chat_mid) {
    LOG("sendChatRemoved", "NULL argument");
    return -1;
  }
  params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber((double)req_seq));
  cJSON_AddItemToArray(params, cJSON_CreateString(chat_mid));
  cJSON_AddItemToArray(params,
    cJSON_CreateString(last_read_message_id ? last_read_message_id : ""));
  cJSON_AddItemToArray(params, cJSON_CreateNumber((double)last_read_message_time));
  return post_void_rpc(
    "/api/talk/thrift/Talk/TalkService/sendChatRemoved",
    params, access_token, "sendChatRemoved");
}

/* ============================================================================
 * TalkService.negotiateE2EEPublicKey (typed)
 * ==========================================================================*/

static const enil_endpoint_t TALK_NEGOTIATE_E2EE_PUBLIC_KEY = {
  .name       = "TalkService.negotiateE2EEPublicKey",
  .method     = ENIL_HTTP_POST,
  .path       = "/api/talk/thrift/Talk/TalkService/negotiateE2EEPublicKey",
  .needs_hmac = 1,
  .needs_auth = 1
};

static cJSON *talk_negotiate_e2ee_public_key_build(const void *req_ptr) {
  const char *peer_mid = (const char *)req_ptr;
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(peer_mid ? peer_mid : ""));
  return params;
}

static int talk_negotiate_e2ee_public_key_parse(cJSON *data, void *out_ptr) {
  talk_e2ee_public_key_t *out = (talk_e2ee_public_key_t *)out_ptr;
  cJSON *pub_key;
  if (!cJSON_IsObject(data)) return -1;
  pub_key = cJSON_GetObjectItemCaseSensitive(data, "publicKey");
  if (!cJSON_IsObject(pub_key)) return -1;
  if (enil_json_dup_string(pub_key, "keyData", &out->publicKey) < 0) return -1;
  enil_json_get_int64(pub_key, "keyId", &out->keyId);
  enil_json_get_int  (data,    "e2eeVersion",  &out->e2eeVersion);
  enil_json_get_bool (data,    "expired",      &out->expired);
  enil_json_get_int64(data,    "createdTime",  &out->createdTime);
  enil_json_get_int  (data,    "renewalCount", &out->renewalCount);
  if (out->keyId == 0 || !out->publicKey) return -1;
  out->raw = cJSON_Duplicate(data, 1);
  return 0;
}

/* ============================================================================
 * TalkService.negotiateE2EEPublicKey — fetch the peer's E2EE public key for a
 * 1:1 chat. Returns 0 on success.
 * ==========================================================================*/
int talk_negotiate_e2ee_public_key(const char *access_token,
                                   const char *peer_mid,
                                   talk_e2ee_public_key_t *out) {
  if (!out || !peer_mid) return -1;
  memset(out, 0, sizeof(*out));
  return enil_api_call(&TALK_NEGOTIATE_E2EE_PUBLIC_KEY, access_token,
                       peer_mid,
                       talk_negotiate_e2ee_public_key_build,
                       out,
                       talk_negotiate_e2ee_public_key_parse);
}

/* ============================================================================
 * Release the public key string and raw cJSON inside the struct.
 * ==========================================================================*/
void talk_e2ee_public_key_free(talk_e2ee_public_key_t *p) {
  if (!p) return;
  enil_field_free_all(p, talk_e2ee_public_key_fields, talk_e2ee_public_key_fields_count);
  if (p->raw) { cJSON_Delete(p->raw); p->raw = NULL; }
}

const enil_field_t talk_e2ee_public_key_fields[] = {
  { "keyId",        offsetof(talk_e2ee_public_key_t, keyId),        ENIL_F_I64  },
  { "publicKey",    offsetof(talk_e2ee_public_key_t, publicKey),    ENIL_F_STR  },
  { "e2eeVersion",  offsetof(talk_e2ee_public_key_t, e2eeVersion),  ENIL_F_INT  },
  { "expired",      offsetof(talk_e2ee_public_key_t, expired),      ENIL_F_BOOL },
  { "createdTime",  offsetof(talk_e2ee_public_key_t, createdTime),  ENIL_F_I64  },
  { "renewalCount", offsetof(talk_e2ee_public_key_t, renewalCount), ENIL_F_INT  },
};
const size_t talk_e2ee_public_key_fields_count =
  sizeof(talk_e2ee_public_key_fields) / sizeof(talk_e2ee_public_key_fields[0]);

/* ============================================================================
 * TalkService.getE2EEPublicKey — returns malloc'd keyData base64 for a given
 * (mid, keyId), or NULL on failure. Caller must free.
 * ==========================================================================*/
char *TalkService_getE2EEPublicKey(const char *mid, long long key_id, const char *access_token) {
  cJSON *params, *data;
  char *body, *result = NULL;
  ENILLineResponse r;

  if (!mid || !access_token) { LOG("getE2EEPublicKey", "NULL argument"); return NULL; }

  params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(mid));
  cJSON_AddItemToArray(params, cJSON_CreateNumber(1));
  cJSON_AddItemToArray(params, cJSON_CreateNumber((double)key_id));
  body = cJSON_PrintUnformatted(params);
  cJSON_Delete(params);
  if (!body) return NULL;

  r = enil_line_post(
    "/api/talk/thrift/Talk/TalkService/getE2EEPublicKey",
    body, access_token);
  free(body);

  data = parse_ok(&r, "getE2EEPublicKey");
  enil_line_response_free(&r);
  if (data) {
    cJSON *kd = cJSON_GetObjectItem(data, "keyData");
    if (cJSON_IsString(kd) && kd->valuestring)
      result = strdup(kd->valuestring);
    cJSON_Delete(data);
  }
  return result;
}

/* ============================================================================
 * TalkService.getLastE2EEGroupSharedKey / getE2EEGroupSharedKey (typed)
 * ==========================================================================*/

/* ============================================================================
 * Parse a group shared-key payload into a talk_e2ee_group_shared_key_t.
 * Returns 0 on success; frees partial state on failure.
 * ==========================================================================*/
int talk_e2ee_group_shared_key_parse(cJSON *data, talk_e2ee_group_shared_key_t *out) {
  cJSON *allowed;
  if (!cJSON_IsObject(data) || !out) return -1;
  memset(out, 0, sizeof(*out));
  enil_json_get_int  (data, "keyVersion",         &out->keyVersion);
  enil_json_get_int64(data, "groupKeyId",         &out->groupKeyId);
  enil_json_dup_string_opt(data, "creator",       &out->creator);
  enil_json_get_int64(data, "creatorKeyId",       &out->creatorKeyId);
  enil_json_dup_string_opt(data, "receiver",      &out->receiver);
  enil_json_get_int64(data, "receiverKeyId",      &out->receiverKeyId);
  enil_json_dup_string_opt(data, "encryptedSharedKey", &out->encryptedSharedKey);
  enil_json_get_int  (data, "specVersion",        &out->specVersion);
  allowed = cJSON_GetObjectItemCaseSensitive(data, "allowedTypes");
  if (cJSON_IsArray(allowed)) out->allowedTypes = cJSON_Duplicate(allowed, 1);
  out->raw = cJSON_Duplicate(data, 1);
  if (!out->creator || out->groupKeyId == 0) {
    talk_e2ee_group_shared_key_free(out);
    return -1;
  }
  return 0;
}

static const enil_endpoint_t TALK_GET_LAST_E2EE_GROUP_SHARED_KEY = {
  .name       = "TalkService.getLastE2EEGroupSharedKey",
  .method     = ENIL_HTTP_POST,
  .path       = "/api/talk/thrift/Talk/TalkService/getLastE2EEGroupSharedKey",
  .needs_hmac = 1,
  .needs_auth = 1
};

static const enil_endpoint_t TALK_GET_E2EE_GROUP_SHARED_KEY = {
  .name       = "TalkService.getE2EEGroupSharedKey",
  .method     = ENIL_HTTP_POST,
  .path       = "/api/talk/thrift/Talk/TalkService/getE2EEGroupSharedKey",
  .needs_hmac = 1,
  .needs_auth = 1
};

typedef struct {
  const char *group_mid;
  long long   group_key_id;     /* unused when only group_mid is sent */
  int         with_key_id;      /* 1 → params is [1, mid, keyId], else [1, mid] */
} talk_group_shared_key_req_t;

static cJSON *talk_group_shared_key_build(const void *req_ptr) {
  const talk_group_shared_key_req_t *req = (const talk_group_shared_key_req_t *)req_ptr;
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber(1));
  cJSON_AddItemToArray(params, cJSON_CreateString(req->group_mid));
  if (req->with_key_id)
    cJSON_AddItemToArray(params, cJSON_CreateNumber((double)req->group_key_id));
  return params;
}

static int talk_group_shared_key_parse_fn(cJSON *data, void *out_ptr) {
  return talk_e2ee_group_shared_key_parse(data, (talk_e2ee_group_shared_key_t *)out_ptr);
}

/* ============================================================================
 * TalkService.getLastE2EEGroupSharedKey — fetches the active group key bundle.
 * ==========================================================================*/
int talk_get_last_e2ee_group_shared_key(const char *access_token,
                                        const char *group_mid,
                                        talk_e2ee_group_shared_key_t *out) {
  talk_group_shared_key_req_t req;
  if (!access_token || !group_mid || !out) return -1;
  memset(out, 0, sizeof(*out));
  req.group_mid    = group_mid;
  req.group_key_id = 0;
  req.with_key_id  = 0;
  return enil_api_call(&TALK_GET_LAST_E2EE_GROUP_SHARED_KEY, access_token,
                       &req, talk_group_shared_key_build,
                       out, talk_group_shared_key_parse_fn);
}

/* ============================================================================
 * TalkService.getE2EEGroupSharedKey — fetches a specific group key by id.
 * ==========================================================================*/
int talk_get_e2ee_group_shared_key(const char *access_token,
                                   const char *group_mid,
                                   long long   group_key_id,
                                   talk_e2ee_group_shared_key_t *out) {
  talk_group_shared_key_req_t req;
  if (!access_token || !group_mid || !out) return -1;
  memset(out, 0, sizeof(*out));
  req.group_mid    = group_mid;
  req.group_key_id = group_key_id;
  req.with_key_id  = 1;
  return enil_api_call(&TALK_GET_E2EE_GROUP_SHARED_KEY, access_token,
                       &req, talk_group_shared_key_build,
                       out, talk_group_shared_key_parse_fn);
}

static const enil_endpoint_t TALK_REGISTER_E2EE_GROUP_KEY = {
  .name       = "TalkService.registerE2EEGroupKey",
  .method     = ENIL_HTTP_POST,
  .path       = "/api/talk/thrift/Talk/TalkService/registerE2EEGroupKey",
  .needs_hmac = 1,
  .needs_auth = 1
};

typedef struct {
  const char *group_mid;
  cJSON      *enc_keys;     /* array of {mid, keyId, encryptedKey} */
} talk_register_group_key_req_t;

static cJSON *talk_register_e2ee_group_key_build(const void *req_ptr) {
  const talk_register_group_key_req_t *req = (const talk_register_group_key_req_t *)req_ptr;
  cJSON *params, *mids_arr, *key_ids_arr, *enc_arr, *item;
  int i, n;
  if (!req || !req->enc_keys) return NULL;
  n = cJSON_GetArraySize(req->enc_keys);
  mids_arr    = cJSON_CreateArray();
  key_ids_arr = cJSON_CreateArray();
  enc_arr     = cJSON_CreateArray();
  params      = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber(1));
  cJSON_AddItemToArray(params, cJSON_CreateString(req->group_mid));
  cJSON_AddItemToArray(params, mids_arr);
  cJSON_AddItemToArray(params, key_ids_arr);
  cJSON_AddItemToArray(params, enc_arr);
  for (i = 0; i < n; i++) {
    cJSON *mid_j, *kid_j, *enc_j;
    item = cJSON_GetArrayItem(req->enc_keys, i);
    mid_j = cJSON_GetObjectItem(item, "mid");
    kid_j = cJSON_GetObjectItem(item, "keyId");
    enc_j = cJSON_GetObjectItem(item, "encryptedKey");
    if (!cJSON_IsString(mid_j) || !cJSON_IsNumber(kid_j) || !cJSON_IsString(enc_j)) {
      cJSON_Delete(params);
      return NULL;
    }
    cJSON_AddItemToArray(mids_arr,    cJSON_CreateString(mid_j->valuestring));
    cJSON_AddItemToArray(key_ids_arr, cJSON_CreateNumber(kid_j->valuedouble));
    cJSON_AddItemToArray(enc_arr,     cJSON_CreateString(enc_j->valuestring));
  }
  return params;
}

/* ============================================================================
 * TalkService.registerE2EEGroupKey — push the newly minted per-member
 * encrypted key shares back to LINE.
 * ==========================================================================*/
int talk_register_e2ee_group_key(const char *access_token,
                                 const char *group_mid,
                                 cJSON      *enc_keys,
                                 talk_e2ee_group_shared_key_t *out) {
  talk_register_group_key_req_t req;
  if (!access_token || !group_mid || !enc_keys || !out) return -1;
  memset(out, 0, sizeof(*out));
  req.group_mid = group_mid;
  req.enc_keys  = enc_keys;
  return enil_api_call(&TALK_REGISTER_E2EE_GROUP_KEY, access_token,
                       &req, talk_register_e2ee_group_key_build,
                       out, talk_group_shared_key_parse_fn);
}

/* ============================================================================
 * Release malloc'd fields and cJSON children of a group shared-key struct.
 * ==========================================================================*/
void talk_e2ee_group_shared_key_free(talk_e2ee_group_shared_key_t *p) {
  if (!p) return;
  enil_field_free_all(p, talk_e2ee_group_shared_key_fields,
                      talk_e2ee_group_shared_key_fields_count);
  if (p->raw) { cJSON_Delete(p->raw); p->raw = NULL; }
}

const enil_field_t talk_e2ee_group_shared_key_fields[] = {
  { "keyVersion",         offsetof(talk_e2ee_group_shared_key_t, keyVersion),         ENIL_F_INT  },
  { "groupKeyId",         offsetof(talk_e2ee_group_shared_key_t, groupKeyId),         ENIL_F_I64  },
  { "creator",            offsetof(talk_e2ee_group_shared_key_t, creator),            ENIL_F_STR  },
  { "creatorKeyId",       offsetof(talk_e2ee_group_shared_key_t, creatorKeyId),       ENIL_F_I64  },
  { "receiver",           offsetof(talk_e2ee_group_shared_key_t, receiver),           ENIL_F_STR  },
  { "receiverKeyId",      offsetof(talk_e2ee_group_shared_key_t, receiverKeyId),      ENIL_F_I64  },
  { "encryptedSharedKey", offsetof(talk_e2ee_group_shared_key_t, encryptedSharedKey), ENIL_F_STR  },
  { "allowedTypes",       offsetof(talk_e2ee_group_shared_key_t, allowedTypes),       ENIL_F_JSON },
  { "specVersion",        offsetof(talk_e2ee_group_shared_key_t, specVersion),        ENIL_F_INT  },
};
const size_t talk_e2ee_group_shared_key_fields_count =
  sizeof(talk_e2ee_group_shared_key_fields) / sizeof(talk_e2ee_group_shared_key_fields[0]);

/* ============================================================================
 * TokenV3IssueResult
 * ==========================================================================*/

static int token_v3_get_double(cJSON *obj, const char *key, double *out) {
  cJSON *item;
  if (!obj || !key || !out) return -1;
  item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(item)) { *out = item->valuedouble; return 0; }
  if (cJSON_IsString(item) && item->valuestring) {
    *out = strtod(item->valuestring, NULL);
    return 0;
  }
  return -1;
}

/* ============================================================================
 * Parse the tokenV3IssueResult payload (access/refresh tokens, durations).
 * Returns 0 on success.
 * ==========================================================================*/
int talk_token_v3_issue_result_parse(cJSON *data, talk_token_v3_issue_result_t *out) {
  cJSON *policy;
  if (!cJSON_IsObject(data) || !out) return -1;
  memset(out, 0, sizeof(*out));
  if (enil_json_dup_string(data, "accessToken", &out->accessToken) < 0) return -1;
  enil_json_dup_string_opt(data, "refreshToken",   &out->refreshToken);
  enil_json_dup_string_opt(data, "loginSessionId", &out->loginSessionId);
  enil_json_get_int64(data, "durationUntilRefreshInSec", &out->durationUntilRefreshInSec);
  enil_json_get_int64(data, "tokenIssueTimeEpochSec",    &out->tokenIssueTimeEpochSec);
  policy = cJSON_GetObjectItemCaseSensitive(data, "refreshApiRetryPolicy");
  if (cJSON_IsObject(policy)) {
    enil_json_get_int64(policy, "initialDelayInMillis",
                        &out->refreshApiRetryPolicy.initialDelayInMillis);
    enil_json_get_int64(policy, "maxDelayInMillis",
                        &out->refreshApiRetryPolicy.maxDelayInMillis);
    token_v3_get_double(policy, "multiplier", &out->refreshApiRetryPolicy.multiplier);
    token_v3_get_double(policy, "jitterRate", &out->refreshApiRetryPolicy.jitterRate);
  }
  out->raw = cJSON_Duplicate(data, 1);
  return 0;
}

/* ============================================================================
 * Release malloc'd strings inside a talk_token_v3_issue_result_t.
 * ==========================================================================*/
void talk_token_v3_issue_result_free(talk_token_v3_issue_result_t *p) {
  if (!p) return;
  enil_field_free_all(p, talk_token_v3_issue_result_fields,
                      talk_token_v3_issue_result_fields_count);
  if (p->raw) { cJSON_Delete(p->raw); p->raw = NULL; }
}

const enil_field_t talk_token_v3_issue_result_fields[] = {
  { "accessToken",               offsetof(talk_token_v3_issue_result_t, accessToken),               ENIL_F_STR },
  { "refreshToken",              offsetof(talk_token_v3_issue_result_t, refreshToken),              ENIL_F_STR },
  { "durationUntilRefreshInSec", offsetof(talk_token_v3_issue_result_t, durationUntilRefreshInSec), ENIL_F_I64 },
  { "tokenIssueTimeEpochSec",    offsetof(talk_token_v3_issue_result_t, tokenIssueTimeEpochSec),    ENIL_F_I64 },
  { "loginSessionId",            offsetof(talk_token_v3_issue_result_t, loginSessionId),            ENIL_F_STR },
};
const size_t talk_token_v3_issue_result_fields_count =
  sizeof(talk_token_v3_issue_result_fields) / sizeof(talk_token_v3_issue_result_fields[0]);

/* ============================================================================
 * TalkService.determineMediaMessageFlow (typed)
 * Wire: body is [ {"chatMid": "<mid>"} ] — callRPC wraps the single
 * map-valued arg in a JSON array.
 * ==========================================================================*/

static const enil_endpoint_t TALK_DETERMINE_MEDIA_MESSAGE_FLOW = {
  .name       = "TalkService.determineMediaMessageFlow",
  .method     = ENIL_HTTP_POST,
  .path       = "/api/talk/thrift/Talk/TalkService/determineMediaMessageFlow",
  .needs_hmac = 1,
  .needs_auth = 1
};

static cJSON *talk_determine_media_message_flow_build(const void *req_ptr) {
  const talk_media_flow_req_t *req = (const talk_media_flow_req_t *)req_ptr;
  cJSON *params = cJSON_CreateArray();
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddStringToObject(obj, "chatMid", req && req->chatMid ? req->chatMid : "");
  cJSON_AddItemToArray(params, obj);
  return params;
}

static int talk_determine_media_message_flow_parse(cJSON *data, void *out_ptr) {
  talk_media_flow_t *out = (talk_media_flow_t *)out_ptr;
  cJSON *flow_map;
  if (!cJSON_IsObject(data)) return -1;
  flow_map = cJSON_GetObjectItemCaseSensitive(data, "flowMap");
  if (!cJSON_IsObject(flow_map)) return -1;
  out->flowMap = cJSON_Duplicate(flow_map, 1);
  enil_json_get_int64(data, "cacheTtlMillis", &out->cacheTtlMillis);
  out->raw = cJSON_Duplicate(data, 1);
  return 0;
}

int talk_determine_media_message_flow(const char                  *access_token,
                                      const talk_media_flow_req_t *req,
                                      talk_media_flow_t           *out) {
  if (!access_token || !req || !req->chatMid || !out) {
    LOG("determineMediaMessageFlow", "NULL argument");
    return -1;
  }
  memset(out, 0, sizeof(*out));
  return enil_api_call(&TALK_DETERMINE_MEDIA_MESSAGE_FLOW, access_token,
                       req,
                       talk_determine_media_message_flow_build,
                       out,
                       talk_determine_media_message_flow_parse);
}

void talk_media_flow_free(talk_media_flow_t *p) {
  if (!p) return;
  enil_field_free_all(p, talk_media_flow_fields, talk_media_flow_fields_count);
  if (p->raw) { cJSON_Delete(p->raw); p->raw = NULL; }
}

const enil_field_t talk_media_flow_fields[] = {
  { "flowMap",        offsetof(talk_media_flow_t, flowMap),        ENIL_F_JSON },
  { "cacheTtlMillis", offsetof(talk_media_flow_t, cacheTtlMillis), ENIL_F_I64  },
};
const size_t talk_media_flow_fields_count =
  sizeof(talk_media_flow_fields) / sizeof(talk_media_flow_fields[0]);
