#ifndef ENIL_API_TYPES_H
#define ENIL_API_TYPES_H

#include <stddef.h>
#include "cJSON.h"

/* ============================================================================
 * Generic field descriptor — lets ObjC wrap a C struct as an NSDictionary
 * ==========================================================================*/

typedef enum {
  ENIL_F_STR,
  ENIL_F_INT,
  ENIL_F_BOOL,
  ENIL_F_I64,
  ENIL_F_JSON
} enil_field_type_t;

typedef struct {
  const char        *key;     /* LINE JSON key */
  size_t             offset;  /* offsetof(struct, field) */
  enil_field_type_t  type;
} enil_field_t;

/* Free every ENIL_F_STR (free) and ENIL_F_JSON (cJSON_Delete) field described
 * by `fields`, zeroing each slot; scalar fields (INT/BOOL/I64) are left alone.
 * A trailing `raw` cJSON is intentionally NOT part of the field tables, so
 * callers free it explicitly. Safe on a NULL base. */
void enil_field_free_all(void *base, const enil_field_t *fields, size_t count);

/* ============================================================================
 * Transport descriptor
 * ==========================================================================*/

typedef enum {
  ENIL_HTTP_GET,
  ENIL_HTTP_POST
} enil_http_method_t;

typedef struct {
  const char         *name;         /* "TalkService.getProfile" — used in logs */
  enil_http_method_t  method;
  const char         *path;         /* "/api/talk/thrift/Talk/TalkService/getProfile" */
  int                 needs_hmac;   /* sign body via worker /sign */
  int                 needs_auth;   /* attach X-Line-Access */
} enil_endpoint_t;

/* ============================================================================
 * TalkService.getProfile
 * ==========================================================================*/

typedef struct {
  int sync_reason;                  /* SYNC_REASON_OPERATION = 3 */
} talk_get_profile_req_t;

typedef struct {
  char      *mid;                                 /* "mid" */
  char      *user_id;                             /* "userid" */
  char      *region_code;                         /* "regionCode" */
  char      *display_name;                        /* "displayName" */
  char      *status_message;                      /* "statusMessage" */
  int        allow_search_by_user_id;             /* "allowSearchByUserid" */
  int        allow_search_by_email;               /* "allowSearchByEmail" */
  char      *picture_path;                        /* "picturePath" */
  char      *status_message_content_metadata;     /* "statusMessageContentMetadata"; compact JSON text */
  int        nft_profile;                         /* "nftProfile" */
  char      *profile_id;                          /* "profileId" */
  int        profile_type;                        /* "profileType" */
  long long  created_time_millis;                 /* "createdTimeMillis" — string in JSON */
  cJSON     *raw;                                 /* full server data; for raw_json column */
} talk_profile_t;

void talk_profile_free(talk_profile_t *p);

extern const enil_field_t talk_profile_fields[];
extern const size_t       talk_profile_fields_count;

/* ============================================================================
 * TalkService.getContactsV2 — single contact entry
 * ==========================================================================*/

typedef struct {
  char  *mid;
  char  *display_name;
  char  *display_name_overridden;
  char  *status_message;
  char  *picture_path;
  int    registered;
  cJSON *raw;
} talk_contact_t;

void talk_contact_free(talk_contact_t *c);

extern const enil_field_t talk_contact_fields[];
extern const size_t       talk_contact_fields_count;

/* ============================================================================
 * TalkService.getChats — single Chat entry
 * ==========================================================================*/

typedef struct {
  char      *chatMid;
  long long  createdTime;
  int        notificationDisabled;
  long long  favoriteTimestamp;
  char      *chatName;
  char      *picturePath;
  int        type;                /* 0=GROUP, 1=ROOM */
  char      *creatorMid;          /* lifted from extra.groupExtra.creatorMid */
  char      *invitationTicket;    /* lifted from extra.groupExtra.invitationTicket */
  int        isInvited;           /* Application-derived: 1 if mid came from
                                     getAllChatMids.invitedChatMids, else 0. */
  cJSON     *raw;
} talk_chat_t;

void talk_chat_free(talk_chat_t *c);

extern const enil_field_t talk_chat_fields[];
extern const size_t       talk_chat_fields_count;

/* ============================================================================
 * TalkService.getAllChatMids — request opts + response (memberChatMids /
 * invitedChatMids). Transient: lists feed getChats; the member/invited split
 * is then projected onto talk_chat_t.isInvited before chats_v2 upsert.
 * ==========================================================================*/

typedef struct {
  int withMemberChats;
  int withInvitedChats;
} talk_get_all_chat_mids_req_t;

typedef struct {
  char **memberChatMids;
  int    memberCount;
  char **invitedChatMids;
  int    invitedCount;
  cJSON *raw;
} talk_get_all_chat_mids_t;

void talk_get_all_chat_mids_free(talk_get_all_chat_mids_t *p);

/* ============================================================================
 * TalkService.getMessageBoxes — single MessageBox entry
 * ==========================================================================*/

typedef struct {
  char      *id;                       /* chat mid */
  int        midType;
  char      *lastDeliveredMessageId;   /* from lastDeliveredMessageId.messageId */
  long long  lastDeliveredTime;        /* from lastDeliveredMessageId.deliveredTime */
  char      *lastSeenMessageId;
  long long  unreadCount;              /* json.Number — may be string */
  cJSON     *raw;
} talk_message_box_t;

void talk_message_box_free(talk_message_box_t *mb);

extern const enil_field_t talk_message_box_fields[];
extern const size_t       talk_message_box_fields_count;

/* ============================================================================
 * TalkService.getRecentMessagesV2 — single Message entry
 * ==========================================================================*/

typedef struct {
  /* LINE API fields (mirror of pkg/line/structs.go Message). */
  char      *id;
  char      *from_mid;                /* LINE "from" */
  char      *to_mid;                  /* LINE "to" */
  int        toType;
  int        sessionId;
  long long  createdTime;
  int        contentType;
  int        hasContent;
  char      *text;
  char      *relatedMessageId;
  int        messageRelationType;
  int        relatedMessageServiceCode;
  cJSON     *contentMetadata;         /* string map; REPLACE re-injected after decrypt */
  cJSON     *chunks;                  /* E2EE ciphertext array (5 strings) */

  /* Application-derived (set by caller / upsert; not from LINE response). */
  char      *chat_id;                 /* required for upsert */
  cJSON     *raw;                     /* full server data; for raw_json column */
} talk_message_t;

void talk_message_free(talk_message_t *m);

/* Parses one LINE message JSON object into a talk_message_t. The chat_id is
 * copied in (may be NULL — caller can fill later before upsert). Returns 0
 * on success, -1 on failure. */
int  talk_message_parse(cJSON *entry, const char *chat_id, talk_message_t *out);

extern const enil_field_t talk_message_fields[];
extern const size_t       talk_message_fields_count;

/* ============================================================================
 * TalkService.sendMessage — request + response
 * Wire format: [reqSeq, Message]. Response is a Message (uses talk_message_t).
 * The request's message uses borrowed string refs by convention — call
 * talk_send_message_body_build, then free your borrowed strings normally.
 * Do NOT call talk_message_free on a request whose fields point at borrowed
 * stack/literal storage.
 * ==========================================================================*/

typedef struct {
  long long      reqSeq;
  talk_message_t message;
} talk_send_message_req_t;

/* Serialize a sendMessage request body. Returns a malloc'd JSON string the
 * caller must free(), or NULL on failure. */
char *talk_send_message_body_build(const talk_send_message_req_t *req);

/* Parse a sendMessage server response (the unwrapped "data" object — a
 * Message) into a talk_message_t. Caller owns the result; free with
 * talk_message_free. Returns 0 on success. */
int   talk_send_message_response_parse(cJSON *data, talk_message_t *out);

/* ============================================================================
 * LINE SSE operation types — AUTHORITATIVE, complete list.
 *
 * Source of truth: LINE Chrome extension v3.7.2 OPType enum, transcribed
 * verbatim from the deobfuscated bundle
 * (chrome/formatted/main/deobfuscated.js, `let vU = function (e) {...}`).
 * This supersedes matrix-line's partial consts.go. Do NOT hand-edit values;
 * re-transcribe from the extension if LINE bumps the protocol.
 *
 * The X-macro is the single definition: both the enum and
 * enil_op_type_name() are generated from it so they can never drift.
 * ==========================================================================*/

#define ENIL_OP_LIST(X) \
  X(END_OF_OPERATION,                    0)   \
  X(UPDATE_PROFILE,                      1)   \
  X(NOTIFIED_UPDATE_PROFILE,             2)   \
  X(REGISTER_USERID,                     3)   \
  X(ADD_CONTACT,                         4)   \
  X(NOTIFIED_ADD_CONTACT,                5)   \
  X(BLOCK_CONTACT,                       6)   \
  X(UNBLOCK_CONTACT,                     7)   \
  X(NOTIFIED_RECOMMEND_CONTACT,          8)   \
  X(CREATE_GROUP,                        9)   \
  X(UPDATE_GROUP,                        10)  \
  X(NOTIFIED_UPDATE_GROUP,               11)  \
  X(INVITE_INTO_GROUP,                   12)  \
  X(NOTIFIED_INVITE_INTO_GROUP,          13)  \
  X(LEAVE_GROUP,                         14)  \
  X(NOTIFIED_LEAVE_GROUP,                15)  \
  X(ACCEPT_GROUP_INVITATION,             16)  \
  X(NOTIFIED_ACCEPT_GROUP_INVITATION,    17)  \
  X(KICKOUT_FROM_GROUP,                  18)  \
  X(NOTIFIED_KICKOUT_FROM_GROUP,         19)  \
  X(CREATE_ROOM,                         20)  \
  X(INVITE_INTO_ROOM,                    21)  \
  X(NOTIFIED_INVITE_INTO_ROOM,           22)  \
  X(LEAVE_ROOM,                          23)  \
  X(NOTIFIED_LEAVE_ROOM,                 24)  \
  X(SEND_MESSAGE,                        25)  \
  X(RECEIVE_MESSAGE,                     26)  \
  X(SEND_MESSAGE_RECEIPT,                27)  \
  X(RECEIVE_MESSAGE_RECEIPT,             28)  \
  X(SEND_CONTENT_RECEIPT,                29)  \
  X(RECEIVE_ANNOUNCEMENT,                30)  \
  X(CANCEL_INVITATION_GROUP,             31)  \
  X(NOTIFIED_CANCEL_INVITATION_GROUP,    32)  \
  X(NOTIFIED_UNREGISTER_USER,            33)  \
  X(REJECT_GROUP_INVITATION,             34)  \
  X(NOTIFIED_REJECT_GROUP_INVITATION,    35)  \
  X(UPDATE_SETTINGS,                     36)  \
  X(NOTIFIED_REGISTER_USER,              37)  \
  X(INVITE_VIA_EMAIL,                    38)  \
  X(NOTIFIED_REQUEST_RECOVERY,           39)  \
  X(SEND_CHAT_CHECKED,                   40)  \
  X(SEND_CHAT_REMOVED,                   41)  \
  X(NOTIFIED_FORCE_SYNC,                 42)  \
  X(SEND_CONTENT,                        43)  \
  X(SEND_MESSAGE_MYHOME,                 44)  \
  X(NOTIFIED_UPDATE_CONTENT_PREVIEW,     45)  \
  X(REMOVE_ALL_MESSAGES,                 46)  \
  X(NOTIFIED_UPDATE_PURCHASES,           47)  \
  X(DUMMY,                               48)  \
  X(UPDATE_CONTACT,                      49)  \
  X(NOTIFIED_RECEIVED_CALL,              50)  \
  X(CANCEL_CALL,                         51)  \
  X(NOTIFIED_REDIRECT,                   52)  \
  X(NOTIFIED_CHANNEL_SYNC,               53)  \
  X(FAILED_SEND_MESSAGE,                 54)  \
  X(NOTIFIED_READ_MESSAGE,               55)  \
  X(FAILED_EMAIL_CONFIRMATION,           56)  \
  X(NOTIFIED_CHAT_CONTENT,               58)  \
  X(NOTIFIED_PUSH_NOTICENTER_ITEM,       59)  \
  X(NOTIFIED_JOIN_CHAT,                  60)  \
  X(NOTIFIED_LEAVE_CHAT,                 61)  \
  X(NOTIFIED_TYPING,                     62)  \
  X(FRIEND_REQUEST_ACCEPTED,             63)  \
  X(DESTROY_MESSAGE,                     64)  \
  X(NOTIFIED_DESTROY_MESSAGE,            65)  \
  X(UPDATE_PUBLICKEYCHAIN,               66)  \
  X(NOTIFIED_UPDATE_PUBLICKEYCHAIN,      67)  \
  X(NOTIFIED_BLOCK_CONTACT,              68)  \
  X(NOTIFIED_UNBLOCK_CONTACT,            69)  \
  X(UPDATE_GROUPPREFERENCE,              70)  \
  X(NOTIFIED_PAYMENT_EVENT,              71)  \
  X(REGISTER_E2EE_PUBLICKEY,             72)  \
  X(NOTIFIED_E2EE_KEY_EXCHANGE_REQ,      73)  \
  X(NOTIFIED_E2EE_KEY_EXCHANGE_RESP,     74)  \
  X(NOTIFIED_E2EE_MESSAGE_RESEND_REQ,    75)  \
  X(NOTIFIED_E2EE_MESSAGE_RESEND_RESP,   76)  \
  X(NOTIFIED_E2EE_KEY_UPDATE,            77)  \
  X(NOTIFIED_BUDDY_UPDATE_PROFILE,       78)  \
  X(NOTIFIED_UPDATE_LINEAT_TABS,         79)  \
  X(UPDATE_ROOM,                         80)  \
  X(NOTIFIED_BEACON_DETECTED,            81)  \
  X(UPDATE_EXTENDED_PROFILE,             82)  \
  X(ADD_FOLLOW,                          83)  \
  X(NOTIFIED_ADD_FOLLOW,                 84)  \
  X(DELETE_FOLLOW,                       85)  \
  X(NOTIFIED_DELETE_FOLLOW,              86)  \
  X(UPDATE_TIMELINE_SETTINGS,            87)  \
  X(NOTIFIED_FRIEND_REQUEST,             88)  \
  X(UPDATE_RINGBACK_TONE,                89)  \
  X(NOTIFIED_POSTBACK,                   90)  \
  X(RECEIVE_READ_WATERMARK,              91)  \
  X(NOTIFIED_MESSAGE_DELIVERED,          92)  \
  X(NOTIFIED_UPDATE_CHAT_BAR,            93)  \
  X(NOTIFIED_CHATAPP_INSTALLED,          94)  \
  X(NOTIFIED_CHATAPP_UPDATED,            95)  \
  X(NOTIFIED_CHATAPP_NEW_MARK,           96)  \
  X(NOTIFIED_CHATAPP_DELETED,            97)  \
  X(NOTIFIED_CHATAPP_SYNC,               98)  \
  X(NOTIFIED_UPDATE_MESSAGE,             99)  \
  X(UPDATE_CHATROOMBGM,                  100) \
  X(NOTIFIED_UPDATE_CHATROOMBGM,         101) \
  X(UPDATE_RINGTONE,                     102) \
  X(UPDATE_USER_SETTINGS,                118) \
  X(NOTIFIED_UPDATE_STATUS_BAR,          119) \
  X(CREATE_CHAT,                         120) \
  X(UPDATE_CHAT,                         121) \
  X(NOTIFIED_UPDATE_CHAT,                122) \
  X(INVITE_INTO_CHAT,                    123) \
  X(NOTIFIED_INVITE_INTO_CHAT,           124) \
  X(CANCEL_CHAT_INVITATION,              125) \
  X(NOTIFIED_CANCEL_CHAT_INVITATION,     126) \
  X(DELETE_SELF_FROM_CHAT,               127) \
  X(NOTIFIED_DELETE_SELF_FROM_CHAT,      128) \
  X(ACCEPT_CHAT_INVITATION,              129) \
  X(NOTIFIED_ACCEPT_CHAT_INVITATION,     130) \
  X(REJECT_CHAT_INVITATION,              131) \
  X(DELETE_OTHER_FROM_CHAT,              132) \
  X(NOTIFIED_DELETE_OTHER_FROM_CHAT,     133) \
  X(NOTIFIED_CONTACT_CALENDAR_EVENT,     134) \
  X(NOTIFIED_CONTACT_CALENDAR_EVENT_ALL, 135) \
  X(UPDATE_THINGS_OPERATIONS,            136) \
  X(SEND_CHAT_HIDDEN,                    137) \
  X(CHAT_META_SYNC_ALL,                  138) \
  X(SEND_REACTION,                       139) \
  X(NOTIFIED_SEND_REACTION,              140) \
  X(NOTIFIED_UPDATE_PROFILE_CONTENT,     141) \
  X(FAILED_DELIVERY_MESSAGE,             142) \
  X(SEND_ENCRYPTED_E2EE_KEY_REQUESTED,   143) \
  X(CHANNEL_PAAK_AUTHENTICATION_REQUESTED, 144) \
  X(UPDATE_PIN_STATE,                    145) \
  X(NOTIFIED_PREMIUMBACKUP_STATE_CHANGED, 146) \
  X(CREATE_MULTI_PROFILE,                147) \
  X(MULTI_PROFILE_STATUS_CHANGED,        148) \
  X(DELETE_MULTI_PROFILE,                149) \
  X(UPDATE_PROFILE_MAPPING,              150) \
  X(DELETE_PROFILE_MAPPING,              151) \
  X(NOTIFIED_DESTROY_NOTICENTER_PUSH,    152) \
  X(FORCE_KEY_BACKUP_HEADER_VALIDATION,  153)

typedef enum {
#define ENIL_OP_ENUM_ENTRY(name, value) ENIL_OP_##name = value,
  ENIL_OP_LIST(ENIL_OP_ENUM_ENTRY)
#undef ENIL_OP_ENUM_ENTRY
  ENIL_OP_UNKNOWN = -1
} enil_op_type_t;

const char *enil_op_type_name(int type);

/* ============================================================================
 * SSE /api/operation/receive — single Operation entry (push payload)
 * Transient: not stored in sqlite; consumed by the dispatch table.
 * ==========================================================================*/

typedef struct {
  long long       revision;
  int             type;            /* see LINE operation type enum */
  int             reqSeq;
  long long       createdTime;
  char           *param1;
  char           *param2;
  char           *param3;
  talk_message_t *message;         /* NULL if op has no embedded message */
  cJSON          *raw;             /* full server data */
} talk_operation_t;

void talk_operation_free(talk_operation_t *op);

/* Parse one SSE `message`-event payload into a typed operation.
 * `my_mid` is used to derive the embedded message's chat_id when present.
 * Returns 0 on success, -1 on failure. */
int  talk_operation_parse(cJSON *root, const char *my_mid, talk_operation_t *out);

extern const enil_field_t talk_operation_fields[];
extern const size_t       talk_operation_fields_count;

/* ============================================================================
 * TalkService.negotiateE2EEPublicKey — peer public key for 1:1 E2EE send
 * Transient: not stored in sqlite; consumed by /e2ee/encrypt-user-v2.
 * ==========================================================================*/

typedef struct {
  long long  keyId;          /* json.Number → I64 */
  char      *publicKey;      /* base64 key material (nested publicKey.keyData) */
  int        e2eeVersion;
  int        expired;
  long long  createdTime;    /* json.Number → I64 */
  int        renewalCount;
  cJSON     *raw;
} talk_e2ee_public_key_t;

void talk_e2ee_public_key_free(talk_e2ee_public_key_t *p);

int  talk_negotiate_e2ee_public_key(const char *access_token,
                                    const char *peer_mid,
                                    talk_e2ee_public_key_t *out);

extern const enil_field_t talk_e2ee_public_key_fields[];
extern const size_t       talk_e2ee_public_key_fields_count;

/* ============================================================================
 * TalkService.getLastE2EEGroupSharedKey / getE2EEGroupSharedKey
 * Transient: not stored in sqlite; consumed by /e2ee/encrypt-group-v2 and
 * /e2ee/decrypt-group-v2 (the worker receives the full `raw` cJSON).
 * ==========================================================================*/

typedef struct {
  int        keyVersion;
  long long  groupKeyId;
  char      *creator;
  long long  creatorKeyId;
  char      *receiver;
  long long  receiverKeyId;
  char      *encryptedSharedKey;
  cJSON     *allowedTypes;        /* []int */
  int        specVersion;
  cJSON     *raw;                 /* full server data — passed to worker */
} talk_e2ee_group_shared_key_t;

void talk_e2ee_group_shared_key_free(talk_e2ee_group_shared_key_t *p);

int  talk_get_last_e2ee_group_shared_key(const char *access_token,
                                         const char *group_mid,
                                         talk_e2ee_group_shared_key_t *out);
int  talk_get_e2ee_group_shared_key(const char *access_token,
                                    const char *group_mid,
                                    long long   group_key_id,
                                    talk_e2ee_group_shared_key_t *out);
int  talk_register_e2ee_group_key(const char *access_token,
                                  const char *group_mid,
                                  cJSON      *enc_keys,
                                  talk_e2ee_group_shared_key_t *out);

/* Parse a raw group-key JSON object (response shape shared by all three
 * group-key endpoints, including registerE2EEGroupKey). Returns 0 on success. */
int  talk_e2ee_group_shared_key_parse(cJSON *data, talk_e2ee_group_shared_key_t *out);

extern const enil_field_t talk_e2ee_group_shared_key_fields[];
extern const size_t       talk_e2ee_group_shared_key_fields_count;

/* ============================================================================
 * TalkService.determineMediaMessageFlow — server picks `emi/emv/ema/emf` vs `m`
 * Transient: not stored in sqlite. Caller should cache flowMap in memory keyed
 * by chat_mid for cacheTtlMillis. matrix-line `methods.go:559`.
 *
 * Response shape: { "flowMap": { "<contentType>": <flow> }, "cacheTtlMillis": "<int>" }
 * flow values: 1 = plaintext PUT to `r/talk/m/<msgId>`,
 *              2 = E2EE upload via OBS (emi/emv/ema/emf SIDs by contentType).
 * ==========================================================================*/

typedef struct {
  const char *chatMid;          /* borrowed; not freed */
} talk_media_flow_req_t;

typedef struct {
  cJSON     *flowMap;          /* {"<contentType>": <flow>} */
  long long  cacheTtlMillis;   /* string in JSON; normalized to int64 */
  cJSON     *raw;
} talk_media_flow_t;

void talk_media_flow_free(talk_media_flow_t *p);

int  talk_determine_media_message_flow(const char                  *access_token,
                                       const talk_media_flow_req_t *req,
                                       talk_media_flow_t           *out);

extern const enil_field_t talk_media_flow_fields[];
extern const size_t       talk_media_flow_fields_count;

/* ============================================================================
 * AuthService.issueTokenV3 / refreshToken — TokenV3IssueResult
 * Drives proactive token refresh. Server delivers most numeric fields as
 * json.Number (strings); parser normalizes them to native types. The string
 * representations are also kept verbatim in `raw` for round-trip into
 * session.json.
 * ==========================================================================*/

typedef struct {
  long long  initialDelayInMillis;
  long long  maxDelayInMillis;
  double     multiplier;
  double     jitterRate;
} talk_refresh_api_retry_policy_t;

typedef struct {
  char                            *accessToken;
  char                            *refreshToken;
  long long                        durationUntilRefreshInSec;
  long long                        tokenIssueTimeEpochSec;
  char                            *loginSessionId;
  talk_refresh_api_retry_policy_t  refreshApiRetryPolicy;
  cJSON                           *raw;          /* full server data */
} talk_token_v3_issue_result_t;

void talk_token_v3_issue_result_free(talk_token_v3_issue_result_t *p);

/* Parse a tokenV3IssueResult JSON object (returned by /api/auth/tokenRefresh
 * and login flows). Returns 0 on success, -1 if accessToken is missing. */
int  talk_token_v3_issue_result_parse(cJSON *data, talk_token_v3_issue_result_t *out);

extern const enil_field_t talk_token_v3_issue_result_fields[];
extern const size_t       talk_token_v3_issue_result_fields_count;

/* ============================================================================
 * ShopService.getOwnedProductSummaries — stickershop product (one row)
 * Mirrors the LINE JSON product entry where shop == "stickershop".
 * ==========================================================================*/

typedef struct {
  char      *id;
  char      *name;
  char      *latestVersion;
  int        grantedByDefault;
  char      *authorId;
  long long  installedTime;            /* JSON string, numeric epoch-ms */
  char      *validUntil;               /* "-1" sentinel — keep as string */
  int        validFor;
  int        availability;
  int        canAutoDownload;
  int        promotionType;            /* flattened from promotionInfo.promotionType */
  /* productTypeSummary.stickerSummary */
  int        stickerResourceType;
  int        stickerSize;
  char      *suggestVersion;
  int        defaultDisplayOnKeyboard;
  int        availableForPhotoEdit;
  cJSON     *stickerIdRanges;          /* [{start,size}, …] */
  /* local */
  int        isPurchased;
  cJSON     *raw;
} sticker_package_t;

void sticker_package_free(sticker_package_t *p);

extern const enil_field_t sticker_package_fields[];
extern const size_t       sticker_package_fields_count;

/* ============================================================================
 * ShopService.getOwnedProductSummaries — sticonshop product (one row)
 * Mirrors the LINE JSON product entry where shop == "sticonshop".
 * ==========================================================================*/

typedef struct {
  char      *id;
  char      *name;
  char      *latestVersion;
  int        grantedByDefault;
  char      *authorId;
  long long  installedTime;
  char      *validUntil;
  int        validFor;
  int        availability;
  int        canAutoDownload;
  int        promotionType;
  /* productTypeSummary.sticonSummary */
  int        sticonResourceType;       /* int form from product summary */
  char      *suggestVersion;
  int        availableForPhotoEdit;
  /* applicationVersionRange */
  cJSON     *applicationVersionRange;
  /* local */
  int        isPurchased;
  cJSON     *raw;
} sticon_package_t;

void sticon_package_free(sticon_package_t *p);

extern const enil_field_t sticon_package_fields[];
extern const size_t       sticon_package_fields_count;

/* ============================================================================
 * Chat summary — joined view of chats_v2, message_boxes_v2, contacts_v2
 * Used by the UI chat list. Not a direct LINE API type.
 * ==========================================================================*/

typedef struct {
  char      *chatMid;
  int        type;                  /* 0=group/room, 1=1:1 */
  char      *displayName;           /* chatName for groups; contact name for 1:1 */
  long long  unreadCount;
  long long  lastDeliveredTime;
  int        isInvited;             /* projected from chats_v2.isInvited */
} chat_summary_t;

void chat_summary_free(chat_summary_t *s);

extern const enil_field_t chat_summary_fields[];
extern const size_t       chat_summary_fields_count;

/* ============================================================================
 * UI row views — projected from sqlite for the WebView renderer & view controllers.
 * ==========================================================================*/

typedef struct {
  char     *message_id;
  char     *sender_name;
  char     *text;
  char     *text_html;       /* enriched: enil_message_text_html */
  char     *media_path;
  char     *thumb_path;
  char     *from_mid;
  char     *sticker_id;
  char     *sticker_pkg_id;
  char     *sticker_path;    /* enriched: enil_message_sticker_image_info */
  char     *avatar_path;     /* enriched: "avatars/<from_mid>.jpg" if on disk */
  char     *reactions_json;  /* messages_v2.reactions_json (NULL = none) */
  long long created_at;
  int       content_type;
  int       orig_width;
  int       orig_height;
  int       thumb_width;
  int       thumb_height;
  int       sticker_width;   /* enriched */
  int       sticker_height;  /* enriched */
  int       is_outgoing;     /* enriched: from_mid == my_mid */
  int       is_deleted;      /* messages_v2.is_deleted (unsend op 64/65) */
  int       decrypt_status;  /* messages_v2.decrypt_status; ENIL_DECRYPT_FAILED
                              * triggers the placeholder render path. */
} message_row_t;

void message_row_free(message_row_t *m);
extern const enil_field_t message_row_fields[];
extern const size_t       message_row_fields_count;

typedef struct {
  char *sticker_id;
  char *package_id;
  char *package_name;
  char *shop;
  char *image_path;
  char *thumb_path;
  char *alt_text;
  int   image_width;
  int   image_height;
  int   thumb_width;
  int   thumb_height;
} sticker_row_t;

void sticker_row_free(sticker_row_t *s);
extern const enil_field_t sticker_row_fields[];
extern const size_t       sticker_row_fields_count;

typedef struct {
  char *sticker_id;
  char *package_id;
  char *package_name;
  char *image_path;
  char *alt_text;
  int   image_width;
  int   image_height;
} sticon_row_t;

void sticon_row_free(sticon_row_t *s);
extern const enil_field_t sticon_row_fields[];
extern const size_t       sticon_row_fields_count;

/* ============================================================================
 * Local session state — typed view of Application Support/ENIL/<acct>/session.json
 * ==========================================================================*/

/* Scalars are owned (malloc'd / inline). Nested blobs that the rest of the
 * codebase passes through opaquely (worker state, key list, encrypted token
 * map, login meta) stay as cJSON — owned by the session_t and freed in
 * enil_session_free. */
typedef struct {
  char      *accessToken;
  char      *refreshToken;
  char      *mid;
  char      *displayName;
  char      *regionCode;
  char      *certificate;
  char      *e2eeLoginPublicKey;
  char      *e2eeVersion;
  char      *e2eeHashKeyChain;
  long long  durationUntilRefreshInSec;
  long long  tokenIssueTimeEpochSec;
  long long  e2eeLatestKeyId;
  long long  e2eeSequenceNumber;
  long long  reqSeq;
  int        needsReauth;          /* set on SSE talkException 117; cleared
                                      implicitly when reauth replaces this
                                      file wholesale */
  cJSON     *e2eeKeys;             /* array */
  cJSON     *e2eeLoginMetaData;    /* object */
  cJSON     *encryptedAccessTokens;/* object */
  cJSON     *workerRestoreState;   /* object */
  cJSON     *refreshApiRetryPolicy;/* object */
  cJSON     *lastPartialFullSyncs; /* object: {"<categoryId>": "<ts>"} */
} session_t;

#endif
