/* ============================================================================
 * sqlite schema and row-level CRUD for accounts, friends, chats, messages,
 * sticker/sticon packs, and other synced LINE state.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <assert.h>
#include "enil_db.h"
#include "enil_api_json.h"
#include "enil_message_format.h"
#include "enil_cocoa_collate.h"  /* enil_localized_collate (plain-C sqlite collation hook) */
#include "enil_cocoa_log.h"

#define LOG(fn, msg) enil_log("Db." fn, "%s", msg)

/* Defined with the other statement helpers below; forward-declared here because
 * the message-sticon helpers in the "Helpers" section call it before that. */
static int step_done(sqlite3 *db, sqlite3_stmt *stmt, const char *tag);

/* --- SQL --- */

static const char * const kSQL_CreateAccounts =
  "CREATE TABLE IF NOT EXISTS accounts_v2 ("
  "  mid                          TEXT PRIMARY KEY,"
  "  userid                       TEXT,"
  "  regionCode                   TEXT,"
  "  displayName                  TEXT,"
  "  statusMessage                TEXT,"
  "  allowSearchByUserid          INTEGER,"
  "  allowSearchByEmail           INTEGER,"
  "  picturePath                  TEXT,"
  "  statusMessageContentMetadata TEXT,"
  "  nftProfile                   INTEGER,"
  "  profileId                    TEXT,"
  "  profileType                  INTEGER,"
  "  createdTimeMillis            INTEGER,"
  "  raw_json                     TEXT"
  ")";

static const char * const kSQL_UpsertAccount =
  "INSERT OR REPLACE INTO accounts_v2 ("
  "  mid, userid, regionCode, displayName, statusMessage,"
  "  allowSearchByUserid, allowSearchByEmail, picturePath,"
  "  statusMessageContentMetadata, nftProfile, profileId, profileType,"
  "  createdTimeMillis, raw_json"
  ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

static const char * const kSQL_GetAccount =
  "SELECT mid, userid, regionCode, displayName, statusMessage,"
  "       allowSearchByUserid, allowSearchByEmail, picturePath,"
  "       statusMessageContentMetadata, nftProfile, profileId, profileType,"
  "       createdTimeMillis FROM accounts_v2 LIMIT 1";

static const char * const kSQL_CreateContacts =
  "CREATE TABLE IF NOT EXISTS contacts_v2 ("
  "  mid                    TEXT PRIMARY KEY,"
  "  displayName            TEXT,"
  "  displayNameOverridden  TEXT,"
  "  statusMessage          TEXT,"
  "  picturePath            TEXT,"
  "  registered             INTEGER NOT NULL DEFAULT 1,"
  "  raw_json               TEXT"
  ")";

static const char * const kSQL_UpsertContact =
  "INSERT OR REPLACE INTO contacts_v2"
  " (mid, displayName, displayNameOverridden, statusMessage, picturePath, registered, raw_json)"
  " VALUES (?, ?, ?, ?, ?, ?, ?)";

static const char * const kSQL_GetContacts =
  "SELECT mid, displayName, displayNameOverridden, statusMessage, picturePath"
  " FROM contacts_v2 WHERE registered = 1"
  " ORDER BY COALESCE(displayNameOverridden, displayName) COLLATE LOCALIZED";

static const char * const kSQL_CreateChatsV2 =
  "CREATE TABLE IF NOT EXISTS chats_v2 ("
  "  chatMid              TEXT PRIMARY KEY,"
  "  createdTime          INTEGER,"
  "  notificationDisabled INTEGER,"
  "  favoriteTimestamp    INTEGER,"
  "  chatName             TEXT,"
  "  picturePath          TEXT,"
  "  type                 INTEGER,"
  "  creatorMid           TEXT,"
  "  invitationTicket     TEXT,"
  "  isInvited            INTEGER NOT NULL DEFAULT 0,"
  "  raw_json             TEXT"
  ")";

static const char * const kSQL_UpsertChatV2 =
  "INSERT OR REPLACE INTO chats_v2 ("
  "  chatMid, createdTime, notificationDisabled, favoriteTimestamp,"
  "  chatName, picturePath, type, creatorMid, invitationTicket,"
  "  isInvited, raw_json"
  ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

static const char * const kSQL_CreateMessageBoxesV2 =
  "CREATE TABLE IF NOT EXISTS message_boxes_v2 ("
  "  id                     TEXT PRIMARY KEY,"
  "  midType                INTEGER,"
  "  lastDeliveredMessageId TEXT,"
  "  lastDeliveredTime      INTEGER,"
  "  lastSeenMessageId      TEXT,"
  "  unreadCount            INTEGER,"
  "  peerLastReadTime       INTEGER,"
  "  peerLastReaderMid      TEXT,"
  "  raw_json               TEXT"
  ")";

/* Two-step upsert: ensure-row + update only sync-owned columns. INSERT OR
 * REPLACE would delete-and-reinsert the row, wiping peerLastReadTime /
 * peerLastReaderMid (written exclusively by the SSE op-55 handler) on every
 * getMessageBoxes refresh. Tiger's bundled sqlite (~3.1.3) predates the
 * ON CONFLICT UPSERT syntax, so we split it manually. */
static const char * const kSQL_InsertMessageBoxV2Row =
  "INSERT OR IGNORE INTO message_boxes_v2 (id) VALUES (?)";

/* lastSeenMessageId is COALESCE-guarded: a NULL bind preserves the stored
 * value rather than clobbering it. getMessageBoxes (full sync) passes the
 * real server read-position; the message-arrival upsert (upsert_chat_for_message)
 * passes NULL because it has no opinion on the seen watermark, and must not
 * wipe it. Same protection peerLastReadTime gets by being excluded entirely —
 * but lastSeenMessageId IS written here by sync, so COALESCE, not exclusion. */
static const char * const kSQL_UpdateMessageBoxV2 =
  "UPDATE message_boxes_v2 SET"
  "  midType = ?,"
  "  lastDeliveredMessageId = ?,"
  "  lastDeliveredTime = ?,"
  "  lastSeenMessageId = COALESCE(?, lastSeenMessageId),"
  "  unreadCount = ?,"
  "  raw_json = ?"
  " WHERE id = ?";

/* SSE SEND/RECEIVE events carry one message, not the server-authoritative
 * MessageBox object. Preserve sync-owned raw_json/lastSeenMessageId; only move
 * the delivered high-water mark and update unreadCount from the live event. */
static const char * const kSQL_UpdateMessageBoxV2LiveMessage =
  "UPDATE message_boxes_v2 SET"
  "  midType = ?,"
  "  lastDeliveredMessageId = ?,"
  "  lastDeliveredTime = ?,"
  "  unreadCount = CASE WHEN ? != 0 THEN"
  "    COALESCE(unreadCount, 0) + CASE"
  "      WHEN ? IS NOT NULL AND ? != ''"
  "       AND NOT EXISTS (SELECT 1 FROM messages_v2 WHERE id = ?)"
  "      THEN 1 ELSE 0 END"
  "    ELSE 0 END"
  " WHERE id = ?";

static const char * const kSQL_GetMessageBoxesV2 =
  "SELECT id, midType, lastDeliveredMessageId, lastDeliveredTime,"
  "       lastSeenMessageId, unreadCount"
  " FROM message_boxes_v2";

/* Joined chat summary for the UI list. 1:1 chats only appear in
 * message_boxes_v2; group/room chats appear in chats_v2 too. The SELECT
 * projection is shared verbatim between the full-list query (ORDER BY recency)
 * and the single-row lookup (WHERE mb.id = ?) so the two can never drift in
 * column order/shape — both feed the same positional chat_summary_fields[]. */
#define SQL_CHAT_SUMMARY_SELECT \
  "SELECT mb.id AS chatMid," \
  "  CASE WHEN substr(mb.id,1,1) IN ('c','r') THEN 0 ELSE 1 END AS type," \
  "  COALESCE(" \
  "    NULLIF(c.chatName, '')," \
  "    NULLIF(f.displayNameOverridden, '')," \
  "    NULLIF(f.displayName, '')," \
  "    mb.id" \
  "  ) AS displayName," \
  "  mb.unreadCount," \
  "  mb.lastDeliveredTime," \
  "  COALESCE(c.isInvited, 0) AS isInvited" \
  " FROM message_boxes_v2 mb" \
  " LEFT JOIN chats_v2    c ON c.chatMid = mb.id" \
  " LEFT JOIN contacts_v2 f ON f.mid     = mb.id"

static const char * const kSQL_GetChatSummaries =
  SQL_CHAT_SUMMARY_SELECT " ORDER BY mb.lastDeliveredTime DESC";

/* Single-row sibling: same projection, one chat by message-box id. Drives the
 * surgical chat-list update on SSE message events (no full-roster rebuild). */
static const char * const kSQL_GetChatSummaryOne =
  SQL_CHAT_SUMMARY_SELECT " WHERE mb.id = ? LIMIT 1";

/* Per-member group read state. One row per (chatMid, readerMid); the row is
 * overwritten in place as a member's high-water mark advances. The 1:1 path
 * keeps using message_boxes_v2.peerLastRead* — this table only carries
 * c/C/r/R-prefixed chats so the 1:1 fast path is undisturbed. */
static const char * const kSQL_CreateMessageBoxReadersV2 =
  "CREATE TABLE IF NOT EXISTS message_box_readers_v2 ("
  "  chatMid    TEXT NOT NULL,"
  "  readerMid  TEXT NOT NULL,"
  "  readTime   INTEGER NOT NULL,"
  "  PRIMARY KEY (chatMid, readerMid)"
  ")";

static const char * const kSQL_CreateMessagesV2 =
  "CREATE TABLE IF NOT EXISTS messages_v2 ("
  /* LINE API fields (column names match LINE JSON keys; SQL keywords quoted). */
  "  id                        TEXT PRIMARY KEY,"
  "  \"from\"                  TEXT,"
  "  \"to\"                    TEXT,"
  "  toType                    INTEGER NOT NULL DEFAULT 0,"
  "  sessionId                 INTEGER NOT NULL DEFAULT 0,"
  "  createdTime               INTEGER NOT NULL DEFAULT 0,"
  "  contentType               INTEGER NOT NULL DEFAULT 0,"
  "  hasContent                INTEGER NOT NULL DEFAULT 0,"
  "  text                      TEXT,"
  "  contentMetadata           TEXT,"
  "  chunks                    TEXT,"
  "  relatedMessageId          TEXT,"
  "  messageRelationType       INTEGER NOT NULL DEFAULT 0,"
  "  relatedMessageServiceCode INTEGER NOT NULL DEFAULT 0,"
  "  raw_json                  TEXT,"
  /* Application-derived state (not from LINE). */
  "  chat_id                   TEXT NOT NULL,"
  "  decrypt_status            INTEGER NOT NULL DEFAULT 0,"
  "  replace_json              TEXT,"
  "  sticker_id                TEXT,"
  "  sticker_pkg_id            TEXT,"
  "  sticon_ownership          TEXT,"
  "  enc_km                    TEXT,"
  "  media_path                TEXT,"
  "  media_sid                 TEXT,"
  "  media_oid                 TEXT,"
  "  media_obs_pop             TEXT,"
  "  media_e2ee_version        TEXT,"
  "  media_plain_size          INTEGER NOT NULL DEFAULT 0,"
  "  orig_width                INTEGER NOT NULL DEFAULT 0,"
  "  orig_height               INTEGER NOT NULL DEFAULT 0,"
  "  thumb_path                TEXT,"
  "  thumb_width               INTEGER NOT NULL DEFAULT 0,"
  "  thumb_height              INTEGER NOT NULL DEFAULT 0,"
  "  is_deleted                INTEGER NOT NULL DEFAULT 0,"
  "  reactions_json            TEXT"
  ")";

static const char * const kSQL_CreateMessageSticons =
  "CREATE TABLE IF NOT EXISTS message_sticons ("
  "  message_id TEXT NOT NULL,"
  "  ordinal    INTEGER NOT NULL,"
  "  package_id TEXT NOT NULL,"
  "  sticon_id  TEXT NOT NULL,"
  "  PRIMARY KEY (message_id, ordinal)"
  ")";

static const char * const kSQL_CreateMessageSticonPackages =
  "CREATE TABLE IF NOT EXISTS message_sticon_packages ("
  "  message_id TEXT NOT NULL,"
  "  package_id TEXT NOT NULL,"
  "  PRIMARY KEY (message_id, package_id)"
  ")";

/* ----------------------------------------------------------------------------
 * Stickers / sticons — v2 typed schema (per `temp/CLAUDE.md` migration)
 *
 * Layout (symmetric across the two shops):
 *   sticker_packages_v2  — stickershop product (typed columns + raw_json)
 *   stickers_v2          — locally-derived per-sticker rows (stickershop)
 *   sticon_packages_v2   — sticonshop product (typed columns + raw_json)
 *   sticons_v2           — locally-derived per-sticon rows (sticonshop);
 *                          alt_text comes from meta.altTexts[sticon_id]
 *
 * The legacy `sticker_packages` and `stickers` tables are intentionally left
 * on disk and unused; the `_v2` schema is created fresh on first run.
 * ----------------------------------------------------------------------------*/

static const char * const kSQL_CreateStickerPackagesV2 =
  "CREATE TABLE IF NOT EXISTS sticker_packages_v2 ("
  "  id                       TEXT PRIMARY KEY,"
  "  name                     TEXT,"
  "  latestVersion            TEXT,"
  "  grantedByDefault         INTEGER,"
  "  authorId                 TEXT,"
  "  installedTime            INTEGER,"
  "  validUntil               TEXT,"
  "  validFor                 INTEGER,"
  "  availability             INTEGER,"
  "  canAutoDownload          INTEGER,"
  "  promotionType            INTEGER,"
  "  stickerResourceType      INTEGER,"
  "  stickerSize              INTEGER,"
  "  suggestVersion           TEXT,"
  "  defaultDisplayOnKeyboard INTEGER,"
  "  availableForPhotoEdit    INTEGER,"
  "  stickerIdRanges          TEXT,"
  "  isPurchased              INTEGER NOT NULL DEFAULT 0,"
  "  raw_json                 TEXT"
  ")";

static const char * const kSQL_UpsertStickerPackageV2 =
  "INSERT OR REPLACE INTO sticker_packages_v2 ("
  "  id, name, latestVersion, grantedByDefault, authorId, installedTime,"
  "  validUntil, validFor, availability, canAutoDownload, promotionType,"
  "  stickerResourceType, stickerSize, suggestVersion, defaultDisplayOnKeyboard,"
  "  availableForPhotoEdit, stickerIdRanges, isPurchased, raw_json"
  ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

static const char * const kSQL_CreateSticonPackagesV2 =
  "CREATE TABLE IF NOT EXISTS sticon_packages_v2 ("
  "  id                       TEXT PRIMARY KEY,"
  "  name                     TEXT,"
  "  latestVersion            TEXT,"
  "  grantedByDefault         INTEGER,"
  "  authorId                 TEXT,"
  "  installedTime            INTEGER,"
  "  validUntil               TEXT,"
  "  validFor                 INTEGER,"
  "  availability             INTEGER,"
  "  canAutoDownload          INTEGER,"
  "  promotionType            INTEGER,"
  "  sticonResourceType       INTEGER,"
  "  suggestVersion           TEXT,"
  "  availableForPhotoEdit    INTEGER,"
  "  applicationVersionRange  TEXT,"
  "  isPurchased              INTEGER NOT NULL DEFAULT 0,"
  "  raw_json                 TEXT"
  ")";

static const char * const kSQL_UpsertSticonPackageV2 =
  "INSERT OR REPLACE INTO sticon_packages_v2 ("
  "  id, name, latestVersion, grantedByDefault, authorId, installedTime,"
  "  validUntil, validFor, availability, canAutoDownload, promotionType,"
  "  sticonResourceType, suggestVersion, availableForPhotoEdit,"
  "  applicationVersionRange, isPurchased, raw_json"
  ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

/* Discovery upsert — only inserts a stub row when the pack isn't already
 * present, leaving any typed columns NULL. Used by message scans to record
 * unpurchased packs seen in the wild. */
static const char * const kSQL_DiscoverSticonPackageV2 =
  "INSERT OR IGNORE INTO sticon_packages_v2 (id, isPurchased) VALUES (?, 0)";

static const char * const kSQL_DiscoverStickerPackageV2 =
  "INSERT OR IGNORE INTO sticker_packages_v2 (id, isPurchased) VALUES (?, 0)";

static const char * const kSQL_CreateStickersV2 =
  "CREATE TABLE IF NOT EXISTS stickers_v2 ("
  "  sticker_id   TEXT NOT NULL,"
  "  package_id   TEXT NOT NULL,"
  "  option       TEXT,"
  "  hash         TEXT,"
  "  image_path   TEXT,"
  "  thumb_path   TEXT,"
  "  image_width  INTEGER,"
  "  image_height INTEGER,"
  "  thumb_width  INTEGER,"
  "  thumb_height INTEGER,"
  "  alt_text     TEXT,"
  "  PRIMARY KEY (sticker_id, package_id)"
  ")";

static const char * const kSQL_UpsertStickerV2 =
  "INSERT INTO stickers_v2 (sticker_id, package_id, option, hash)"
  " VALUES (?, ?, ?, ?)"
  " ON CONFLICT(sticker_id, package_id) DO UPDATE SET"
  "  option = COALESCE(excluded.option, option),"
  "  hash   = COALESCE(excluded.hash,   hash)";

static const char * const kSQL_CreateTalkExceptions =
  "CREATE TABLE IF NOT EXISTS talk_exceptions ("
  "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  received_at INTEGER NOT NULL,"
  "  code        INTEGER,"
  "  reason      TEXT,"
  "  raw_json    TEXT"
  ")";

static const char * const kSQL_InsertTalkException =
  "INSERT INTO talk_exceptions (received_at, code, reason, raw_json)"
  " VALUES (?, ?, ?, ?)";

/* Audit log of every SSE event we receive (except keep-alive pings).
 * `handled` is 'YES' if the dispatcher acted on it, 'NO' if dropped. */
static const char * const kSQL_CreateSSEEventsV2 =
  "CREATE TABLE IF NOT EXISTS sse_events_v2 ("
  "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  received_at INTEGER NOT NULL,"
  "  event_type  TEXT,"
  "  op_type     INTEGER,"
  "  op_name     TEXT,"
  "  revision    INTEGER,"
  "  chat_id     TEXT,"
  "  handled     TEXT NOT NULL,"
  "  raw_json    TEXT"
  ")";

static const char * const kSQL_InsertSSEEvent =
  "INSERT INTO sse_events_v2"
  " (received_at, event_type, op_type, op_name, revision, chat_id,"
  "  handled, raw_json)"
  " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

/* Generic local-state key/value store; see enil_db_*_local_rev. Not LINE data. */
static const char * const kSQL_CreateDbMeta =
  "CREATE TABLE IF NOT EXISTS db_meta ("
  "  key   TEXT PRIMARY KEY,"
  "  value TEXT"
  ")";

static const char * const kSQL_CreateSticonsV2 =
  "CREATE TABLE IF NOT EXISTS sticons_v2 ("
  "  sticon_id    TEXT NOT NULL,"
  "  package_id   TEXT NOT NULL,"
  "  alt_text     TEXT,"
  "  image_path   TEXT,"
  "  thumb_path   TEXT,"
  "  image_width  INTEGER,"
  "  image_height INTEGER,"
  "  thumb_width  INTEGER,"
  "  thumb_height INTEGER,"
  "  PRIMARY KEY (sticon_id, package_id)"
  ")";

static const char * const kSQL_UpsertSticonV2 =
  "INSERT INTO sticons_v2 (sticon_id, package_id, alt_text)"
  " VALUES (?, ?, ?)"
  " ON CONFLICT(sticon_id, package_id) DO UPDATE SET"
  "  alt_text = COALESCE(excluded.alt_text, alt_text)";

/* Purchased sticonshop packs that haven't been expanded into sticons_v2 yet
 * (i.e. their meta.json hasn't been ingested). Used by the catch-up loop to
 * full-expand owned packs that somehow missed the main PHASE 5 pass. */
static const char * const kSQL_GetSticonPackagesNeedingMetaV2 =
  "SELECT sp.id FROM sticon_packages_v2 sp"
  " LEFT JOIN sticons_v2 s ON s.package_id = sp.id"
  " WHERE sp.isPurchased = 1 AND s.sticon_id IS NULL";

/* Unpurchased sticonshop packs that have at least one sticons_v2 row missing
 * its alt_text. These are packs we've seen sticons from in messages but haven't
 * fetched meta.json for. Used by the enrichment loop — meta.json is fetched
 * once per pack to fill alt_text on the rows we already have, without
 * pre-expanding the rest of the pack. */
static const char * const kSQL_GetSticonPackagesNeedingAlt =
  "SELECT DISTINCT s.package_id FROM sticons_v2 s"
  " JOIN sticon_packages_v2 sp ON sp.id = s.package_id"
  " WHERE sp.isPurchased = 0 AND s.alt_text IS NULL";

/* Per-pack equivalent of the two needing-meta/needing-alt sweeps, for the SSE
 * path. Returns isPurchased plus total sticon rows and rows missing alt_text so
 * the caller can decide: owned-with-no-rows -> expand, unowned-with-null-alt ->
 * enrich. */
static const char * const kSQL_SticonPackageMetaState =
  "SELECT COALESCE(sp.isPurchased, 0),"
  " (SELECT COUNT(*) FROM sticons_v2 s WHERE s.package_id = sp.id),"
  " (SELECT COUNT(*) FROM sticons_v2 s WHERE s.package_id = sp.id"
  "    AND s.alt_text IS NULL)"
  " FROM sticon_packages_v2 sp WHERE sp.id = ?";

static const char * const kSQL_SetSticonAltText =
  "UPDATE sticons_v2 SET alt_text = ?"
  " WHERE sticon_id = ? AND package_id = ? AND alt_text IS NULL";

/* Back-fill sticonResourceType for a pack discovered via messages. We never
 * want to clobber a value that already came from the owned-product summary,
 * so this only fires when the column is still NULL. */
static const char * const kSQL_SetSticonPackageResourceType =
  "UPDATE sticon_packages_v2 SET sticonResourceType = ?"
  " WHERE id = ? AND sticonResourceType IS NULL";

static const char * const kSQL_GetStickersNeedingDownload =
  "SELECT s.sticker_id, s.package_id, 'stickershop', s.option, s.hash, ''"
  " FROM stickers_v2 s"
  " JOIN sticker_packages_v2 sp ON sp.id = s.package_id"
  " WHERE s.image_path IS NULL AND sp.isPurchased = ?"
  " ORDER BY s.package_id, s.sticker_id";

/* The literal 2 below is ENIL_RESOURCE_TYPE_ANIMATION (enil_db.h) — kept in
 * sync by hand because a SQL string can't reference the C macro. */
static const char * const kSQL_GetSticonsNeedingDownload =
  "SELECT s.sticon_id, s.package_id, 'sticonshop', '', '',"
  " CASE WHEN cp.sticonResourceType = 2 THEN 'ANIMATION' ELSE '' END"
  " FROM sticons_v2 s"
  " JOIN sticon_packages_v2 cp ON cp.id = s.package_id"
  " WHERE s.image_path IS NULL AND cp.isPurchased = ?"
  " ORDER BY s.package_id, s.sticon_id";

static const char * const kSQL_SetStickerImagePath =
  "UPDATE stickers_v2 SET image_path = ?, image_width = ?, image_height = ?"
  " WHERE sticker_id = ? AND package_id = ?";

static const char * const kSQL_SetStickerThumbPath =
  "UPDATE stickers_v2 SET thumb_path = ?, thumb_width = ?, thumb_height = ?"
  " WHERE sticker_id = ? AND package_id = ?";

static const char * const kSQL_SetSticonImagePath =
  "UPDATE sticons_v2 SET image_path = ?, image_width = ?, image_height = ?"
  " WHERE sticon_id = ? AND package_id = ?";

static const char * const kSQL_SetSticonThumbPath =
  "UPDATE sticons_v2 SET thumb_path = ?, thumb_width = ?, thumb_height = ?"
  " WHERE sticon_id = ? AND package_id = ?";

/* The next three queries are shop-agnostic: callers (e.g. message rendering)
 * have an (id, package_id) tuple and want the local image — could be either a
 * sticker or a sticon. UNION ALL hits whichever table has the row. */
static const char * const kSQL_GetStickerImageInfo =
  "SELECT image_path, COALESCE(image_width, 0), COALESCE(image_height, 0)"
  " FROM stickers_v2 WHERE sticker_id = ?"
  " UNION ALL"
  " SELECT image_path, COALESCE(image_width, 0), COALESCE(image_height, 0)"
  " FROM sticons_v2 WHERE sticon_id = ?"
  " LIMIT 1";

static const char * const kSQL_GetStickerImageInfoForPackage =
  "SELECT image_path, COALESCE(image_width, 0), COALESCE(image_height, 0)"
  " FROM stickers_v2"
  " WHERE package_id = ? AND sticker_id = ? AND image_path IS NOT NULL"
  " UNION ALL"
  " SELECT image_path, COALESCE(image_width, 0), COALESCE(image_height, 0)"
  " FROM sticons_v2"
  " WHERE package_id = ? AND sticon_id = ? AND image_path IS NOT NULL"
  " LIMIT 1";

static const char * const kSQL_GetPurchasedSticons =
  "SELECT s.sticon_id, s.package_id, COALESCE(cp.name, ''),"
  " COALESCE(s.image_path, ''), COALESCE(s.alt_text, ''),"
  " COALESCE(s.image_width, 0), COALESCE(s.image_height, 0)"
  " FROM sticons_v2 s"
  " JOIN sticon_packages_v2 cp ON cp.id = s.package_id"
  " WHERE cp.isPurchased = 1 AND s.image_path IS NOT NULL"
  " ORDER BY COALESCE(cp.name, '') COLLATE LOCALIZED, s.package_id COLLATE BINARY,"
  " CAST(s.sticon_id AS INTEGER), s.sticon_id";

static const char * const kSQL_GetPurchasedStickers =
  "SELECT s.sticker_id, s.package_id, COALESCE(sp.name, ''),"
  " 'stickershop', COALESCE(s.image_path, ''),"
  " COALESCE(s.thumb_path, ''), COALESCE(s.alt_text, ''),"
  " COALESCE(s.image_width, 0), COALESCE(s.image_height, 0),"
  " COALESCE(s.thumb_width, 0), COALESCE(s.thumb_height, 0)"
  " FROM stickers_v2 s"
  " JOIN sticker_packages_v2 sp ON sp.id = s.package_id"
  " WHERE sp.isPurchased = 1 AND s.image_path IS NOT NULL"
  " ORDER BY COALESCE(sp.name, '') COLLATE LOCALIZED, s.package_id COLLATE BINARY, CAST(s.sticker_id AS INTEGER), s.sticker_id";

static const char * const kSQL_UpsertMessageV2 =
  "INSERT OR IGNORE INTO messages_v2"
  " (id, \"from\", \"to\", toType, sessionId, createdTime, contentType, hasContent,"
  "  text, contentMetadata, chunks, relatedMessageId, messageRelationType,"
  "  relatedMessageServiceCode, raw_json,"
  "  chat_id, decrypt_status, replace_json, sticker_id, sticker_pkg_id,"
  "  sticon_ownership, media_sid, media_oid, media_obs_pop, media_e2ee_version,"
  "  media_plain_size, reactions_json)"
  " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

static const char * const kSQL_FillMessageReplaceJson =
  "UPDATE messages_v2 SET replace_json = ?"
  " WHERE id = ? AND (replace_json IS NULL OR replace_json = '')";


static const char * const kSQL_GetMessageById =
  "SELECT m.id,"
  "  COALESCE(c.displayNameOverridden, c.displayName, m.\"from\", '') AS sender_name,"
  "  m.chat_id,"
  "  m.createdTime,"
  "  COALESCE(m.text, '') AS text,"
  "  m.contentType,"
  "  COALESCE(m.media_path, '') AS media_path,"
  "  COALESCE(m.\"from\", '') AS from_mid,"
  "  m.orig_width,"
  "  m.orig_height,"
  "  COALESCE(m.thumb_path, '') AS thumb_path,"
  "  m.thumb_width,"
  "  m.thumb_height,"
  "  COALESCE(m.sticker_id, '') AS sticker_id,"
  "  COALESCE(m.sticker_pkg_id, '') AS sticker_pkg_id,"
  "  m.reactions_json,"
  "  m.is_deleted,"
  "  m.decrypt_status"
  " FROM messages_v2 m"
  " LEFT JOIN contacts_v2 c ON m.\"from\" = c.mid"
  " WHERE m.id = ?";

/* 0 = ENIL_DECRYPT_PENDING (never attempted),
 * 3 = ENIL_DECRYPT_FAILED  (a previous attempt failed — usually because the
 *     worker was misconfigured at the time). Per CLAUDE.md re-decryption is
 *     idempotent (pure ECDH, no ratchet), so retrying FAILED rows on every
 *     full sync is safe; transient failures (bad worker secret since fixed)
 *     will now recover the next time the user hits Sync. */
static const char * const kSQL_GetMessagesNeedingDecrypt =
  "SELECT id, raw_json FROM messages_v2"
  " WHERE decrypt_status IN (0, 3) AND raw_json IS NOT NULL";

static const char * const kSQL_GetMessagesNeedingReplace =
  "SELECT id, raw_json FROM messages_v2"
  " WHERE decrypt_status = 2"
  "   AND raw_json LIKE '%STICON_OWNERSHIP%'"
  "   AND raw_json NOT LIKE '%\"REPLACE\"%'";

static const char * const kSQL_SetMessageRawJson =
  "UPDATE messages_v2 SET raw_json = ?, replace_json = ? WHERE id = ?";

static const char * const kSQL_SetMessageText =
  "UPDATE messages_v2 SET text = ?, decrypt_status = ? WHERE id = ?";

static const char * const kSQL_SetMessagePlaintext =
  "UPDATE messages_v2 SET text = ?, raw_json = ?, decrypt_status = ?, replace_json = ?"
  " WHERE id = ?";

static const char * const kSQL_GetMessagesNeedingMedia =
  "SELECT id, chat_id, media_path, enc_km,"
  " COALESCE(media_sid, ''), COALESCE(media_oid, ''),"
  " COALESCE(media_obs_pop, ''), COALESCE(media_e2ee_version, ''),"
  " media_plain_size"
  " FROM messages_v2 WHERE contentType = 1";

/* Same columns as above, for one message — the SSE path resolves only the
 * incoming message's media instead of rescanning every image row. */
static const char * const kSQL_GetMessageMediaInfo =
  "SELECT id, chat_id, media_path, enc_km,"
  " COALESCE(media_sid, ''), COALESCE(media_oid, ''),"
  " COALESCE(media_obs_pop, ''), COALESCE(media_e2ee_version, ''),"
  " media_plain_size"
  " FROM messages_v2 WHERE id = ?";

static const char * const kSQL_SetMediaInfo =
  "UPDATE messages_v2 SET media_path = ?, orig_width = ?, orig_height = ?,"
  " thumb_path = ?, thumb_width = ?, thumb_height = ?"
  " WHERE id = ?";

static const char * const kSQL_SetMessageEncKm =
  "UPDATE messages_v2 SET enc_km = ?, decrypt_status = 2 WHERE id = ?";

static const char * const kSQL_ClearMessageSticons =
  "DELETE FROM message_sticons WHERE message_id = ?";

static const char * const kSQL_InsertMessageSticon =
  "INSERT OR REPLACE INTO message_sticons"
  " (message_id, ordinal, package_id, sticon_id) VALUES (?, ?, ?, ?)";

static const char * const kSQL_ClearMessageSticonPackages =
  "DELETE FROM message_sticon_packages WHERE message_id = ?";

static const char * const kSQL_InsertMessageSticonPackage =
  "INSERT OR IGNORE INTO message_sticon_packages"
  " (message_id, package_id) VALUES (?, ?)";

static const char * const kSQL_GetMessageSticons =
  "SELECT ms.package_id, ms.sticon_id, COALESCE(s.image_path, ''),"
  " COALESCE(s.image_width, 0), COALESCE(s.image_height, 0)"
  " FROM message_sticons ms"
  " LEFT JOIN sticons_v2 s ON s.package_id = ms.package_id"
  "  AND s.sticon_id = ms.sticon_id"
  " WHERE ms.message_id = ? ORDER BY ms.ordinal ASC";

/* --- Helpers --- */

static char *extract_replace_json_from_meta(cJSON *meta) {
  cJSON *replace_item;
  if (!cJSON_IsObject(meta)) return NULL;
  replace_item = cJSON_GetObjectItem(meta, "REPLACE");
  if (cJSON_IsString(replace_item) && replace_item->valuestring)
    return strdup(replace_item->valuestring);
  if (cJSON_IsObject(replace_item))
    return cJSON_PrintUnformatted(replace_item);
  return NULL;
}

static void parse_media_meta(cJSON *meta, const char **sid, const char **oid,
                             const char **obs_pop, const char **e2ee_version,
                             sqlite3_int64 *plain_size)
{
  const char *info_text;
  cJSON *info, *file_size;

  if (sid) *sid = NULL;
  if (oid) *oid = NULL;
  if (obs_pop) *obs_pop = NULL;
  if (e2ee_version) *e2ee_version = NULL;
  if (plain_size) *plain_size = 0;
  if (!cJSON_IsObject(meta)) return;

  if (sid) *sid = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "SID"));
  if (oid) *oid = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "OID"));
  if (obs_pop) *obs_pop = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "OBS_POP"));
  if (e2ee_version)
    *e2ee_version = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "e2eeVersion"));

  info_text = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "MEDIA_CONTENT_INFO"));
  info = info_text ? cJSON_Parse(info_text) : NULL;
  file_size = info ? cJSON_GetObjectItem(info, "fileSize") : NULL;
  if (plain_size && file_size && cJSON_IsNumber(file_size) && file_size->valuedouble > 0)
    *plain_size = (sqlite3_int64)file_size->valuedouble;
  cJSON_Delete(info);
}

static int fill_message_replace_json(sqlite3 *db, const char *message_id,
                                     const char *replace_json) {
  sqlite3_stmt *stmt;
  int rc;
  if (!db || !message_id || !replace_json || !replace_json[0]) return SQLITE_OK;
  rc = sqlite3_prepare_v2(db, kSQL_FillMessageReplaceJson, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(stmt, 1, replace_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, message_id,   -1, SQLITE_TRANSIENT);
  return step_done(db, stmt, "Db.fill_message_replace_json");
}

static int clear_bound_rows(sqlite3 *db, const char *sql, const char *message_id)
{
  sqlite3_stmt *stmt;
  int rc;
  if (!db || !sql || !message_id) return SQLITE_ERROR;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(stmt, 1, message_id, -1, SQLITE_TRANSIENT);
  return step_done(db, stmt, "Db.clear_bound_rows");
}

static int insert_message_sticon(sqlite3 *db, const char *message_id,
                                 int ordinal, const char *package_id,
                                 const char *sticon_id)
{
  sqlite3_stmt *stmt;
  int rc;
  if (!db || !message_id || !package_id || !sticon_id ||
      !package_id[0] || !sticon_id[0])
    return SQLITE_OK;
  rc = sqlite3_prepare_v2(db, kSQL_InsertMessageSticon, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.insert_sticon", "prepare failed: %s", sqlite3_errmsg(db));
    return rc;
  }
  sqlite3_bind_text(stmt, 1, message_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, ordinal);
  sqlite3_bind_text(stmt, 3, package_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, sticon_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE)
    ENIL_LOG("Db.insert_sticon", "step failed (%d): %s", rc, sqlite3_errmsg(db));
  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

static int insert_message_sticon_package(sqlite3 *db, const char *message_id,
                                         const char *package_id)
{
  sqlite3_stmt *stmt;
  int rc;
  if (!db || !message_id || !package_id || !package_id[0]) return SQLITE_OK;
  rc = sqlite3_prepare_v2(db, kSQL_InsertMessageSticonPackage, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(stmt, 1, message_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, package_id, -1, SQLITE_TRANSIENT);
  return step_done(db, stmt, "Db.insert_message_sticon_package");
}

static void store_message_sticon_packages(sqlite3 *db, const char *message_id,
                                          const char *ownership_json)
{
  cJSON *arr;
  int i, n;
  if (!db || !message_id || !ownership_json || !ownership_json[0]) return;
  arr = cJSON_Parse(ownership_json);
  n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
  for (i = 0; i < n; i++) {
    cJSON *item = cJSON_GetArrayItem(arr, i);
    const char *pkg = cJSON_IsString(item) ? item->valuestring : NULL;
    insert_message_sticon_package(db, message_id, pkg);
  }
  cJSON_Delete(arr);
}

static void store_message_sticons(sqlite3 *db, const char *message_id,
                                  const char *replace_json)
{
  cJSON *root, *sticon, *resources;
  int i, n;
  if (!db || !message_id || !replace_json || !replace_json[0]) return;
  root = cJSON_Parse(replace_json);
  sticon = root ? cJSON_GetObjectItem(root, "sticon") : NULL;
  resources = sticon ? cJSON_GetObjectItem(sticon, "resources") : NULL;
  n = cJSON_IsArray(resources) ? cJSON_GetArraySize(resources) : 0;
  for (i = 0; i < n; i++) {
    cJSON *res = cJSON_GetArrayItem(resources, i);
    const char *pkg = cJSON_GetStringValue(cJSON_GetObjectItem(res, "productId"));
    const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(res, "sticonId"));
    insert_message_sticon(db, message_id, i, pkg, sid);
    insert_message_sticon_package(db, message_id, pkg);
    /* Mirror discovery into the catalog so the renderer's join to sticons_v2
     * finds a row, and the download phase pulls the per-sticon image. */
    if (pkg && pkg[0] && sid && sid[0]) {
      enil_db_sticon_package_discover(db, pkg);
      enil_db_sticon_upsert(db, sid, pkg, NULL);
    }
  }
  cJSON_Delete(root);
}

static void replace_message_sticon_rows(sqlite3 *db, const char *message_id,
                                        const char *replace_json,
                                        const char *ownership_json)
{
  if (!db || !message_id) return;
  clear_bound_rows(db, kSQL_ClearMessageSticons, message_id);
  clear_bound_rows(db, kSQL_ClearMessageSticonPackages, message_id);
  store_message_sticon_packages(db, message_id, ownership_json);
  store_message_sticons(db, message_id, replace_json);
}

/* --- Connection --- */

/* ============================================================================
 * Open an sqlite3 connection to path. WAL is enabled and persists in the DB
 * header, so later opens just re-affirm it. Returns NULL on failure.
 * ==========================================================================*/
sqlite3 *enil_db_open(const char *path) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(path, &db);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.open", "%s", sqlite3_errmsg(db));
    sqlite3_close(db);
    return NULL;
  }
  if (sqlite3_create_collation(db, "LOCALIZED", SQLITE_UTF8,
                               NULL, enil_localized_collate) != SQLITE_OK) {
    ENIL_LOG("Db.open", "LOCALIZED collation register failed: %s", sqlite3_errmsg(db));
  }
  if (sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL) != SQLITE_OK) {
    ENIL_LOG("Db.open", "WAL enable failed: %s", sqlite3_errmsg(db));
  }
  return db;
}

/* ============================================================================
 * Close an sqlite3 connection. Safe on NULL.
 * ==========================================================================*/
void enil_db_close(sqlite3 *db) {
  if (db) sqlite3_close(db);
}

/* Secondary indexes for the hot lookups. IF NOT EXISTS needs sqlite >= 3.3.0;
 * the app statically links 3.43.2 on every target (incl Tiger PPC), so it is
 * safe regardless of the host OS sqlite.
 *  - sticons_v2(package_id, alt_text) covers the live SSE path's per-pack
 *    meta-state COUNT(*) subqueries (enil_db_sticon_package_meta_state): both
 *    the total and the alt_text-IS-NULL count read from the index, no scan.
 *  - stickers_v2(image_path) seeks the not-yet-downloaded rows in the full
 *    sync sweep instead of scanning every sticker.
 *  - messages_v2(contentType) seeks image rows in the full sync media sweep. */
static const char * const kSQL_CreateIndexes =
  "CREATE INDEX IF NOT EXISTS idx_sticons_package"
  " ON sticons_v2 (package_id, alt_text);"
  "CREATE INDEX IF NOT EXISTS idx_stickers_image_path"
  " ON stickers_v2 (image_path);"
  "CREATE INDEX IF NOT EXISTS idx_messages_contenttype"
  " ON messages_v2 (contentType);";

/* ============================================================================
 * Apply every CREATE TABLE statement to the database. Idempotent.
 * Returns 0 on success.
 * ==========================================================================*/
int enil_db_create_tables(sqlite3 *db) {
  char *err = NULL;
  int rc;

  rc = sqlite3_exec(db, kSQL_CreateAccounts, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "accounts: %s", err);
    sqlite3_free(err);
    return rc;
  }

  /* migrate legacy table name — silently ignored if already done */
  sqlite3_exec(db, "ALTER TABLE friends RENAME TO contacts", NULL, NULL, NULL);

  rc = sqlite3_exec(db, kSQL_CreateContacts, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "contacts: %s", err);
    sqlite3_free(err);
    return rc;
  }

  rc = sqlite3_exec(db, kSQL_CreateChatsV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "chats_v2: %s", err);
    sqlite3_free(err);
    return rc;
  }
  /* migrate existing DBs created before isInvited landed; ignore duplicate-column error */
  sqlite3_exec(db,
    "ALTER TABLE chats_v2 ADD COLUMN isInvited INTEGER NOT NULL DEFAULT 0",
    NULL, NULL, NULL);
  rc = sqlite3_exec(db, kSQL_CreateMessageBoxesV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "message_boxes_v2: %s", err);
    sqlite3_free(err);
    return rc;
  }
  /* migrate DBs created before peer read columns landed; ignore dup-col error */
  sqlite3_exec(db,
    "ALTER TABLE message_boxes_v2 ADD COLUMN peerLastReadTime INTEGER",
    NULL, NULL, NULL);
  sqlite3_exec(db,
    "ALTER TABLE message_boxes_v2 ADD COLUMN peerLastReaderMid TEXT",
    NULL, NULL, NULL);

  rc = sqlite3_exec(db, kSQL_CreateMessageBoxReadersV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "message_box_readers_v2: %s", err);
    sqlite3_free(err);
    return rc;
  }

  rc = sqlite3_exec(db, kSQL_CreateMessagesV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "messages_v2: %s", err);
    sqlite3_free(err);
    return rc;
  }
  /* migrate existing DBs created before is_deleted landed; ignore dup-col error */
  sqlite3_exec(db,
    "ALTER TABLE messages_v2 ADD COLUMN is_deleted INTEGER NOT NULL DEFAULT 0",
    NULL, NULL, NULL);
  sqlite3_exec(db,
    "ALTER TABLE messages_v2 ADD COLUMN reactions_json TEXT",
    NULL, NULL, NULL);

  rc = sqlite3_exec(db, kSQL_CreateMessageSticons, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "message_sticons: %s", err);
    sqlite3_free(err);
    return rc;
  }
  rc = sqlite3_exec(db, kSQL_CreateMessageSticonPackages, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "message_sticon_packages: %s", err);
    sqlite3_free(err);
    return rc;
  }

  rc = sqlite3_exec(db, kSQL_CreateStickerPackagesV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "sticker_packages_v2: %s", err);
    sqlite3_free(err); return rc;
  }
  rc = sqlite3_exec(db, kSQL_CreateSticonPackagesV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "sticon_packages_v2: %s", err);
    sqlite3_free(err); return rc;
  }
  rc = sqlite3_exec(db, kSQL_CreateStickersV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "stickers_v2: %s", err);
    sqlite3_free(err); return rc;
  }
  rc = sqlite3_exec(db, kSQL_CreateSticonsV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "sticons_v2: %s", err);
    sqlite3_free(err); return rc;
  }
  rc = sqlite3_exec(db, kSQL_CreateTalkExceptions, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "talk_exceptions: %s", err);
    sqlite3_free(err); return rc;
  }
  rc = sqlite3_exec(db, kSQL_CreateSSEEventsV2, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "sse_events_v2: %s", err);
    sqlite3_free(err); return rc;
  }
  rc = sqlite3_exec(db, kSQL_CreateDbMeta, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "db_meta: %s", err);
    sqlite3_free(err); return rc;
  }

  rc = sqlite3_exec(db, kSQL_CreateIndexes, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.create_tables", "indexes: %s", err);
    sqlite3_free(err); return rc;
  }

  return SQLITE_OK;
}


/* --- Accounts --- */

static void bind_text_or_null(sqlite3_stmt *stmt, int idx, const char *s) {
  if (s) sqlite3_bind_text(stmt, idx, s, -1, SQLITE_TRANSIENT);
  else   sqlite3_bind_null(stmt, idx);
}

static char *dup_or_null(const char *s) {
  return (s && s[0]) ? strdup(s) : NULL;
}

static char *dup_or_empty(const unsigned char *s) {
  return s ? strdup((const char *)s) : strdup("");
}

/* Step a prepared write statement to completion, finalize it, and map the
 * result to SQLITE_OK / the failing rc. Logs the sqlite error under `tag` on
 * failure. Folds the prepare→bind→[step→finalize→check] tail that every
 * mutating helper in this file shares; callers keep only their distinctive
 * binds. The statement is always finalized, including on error. */
static int step_done(sqlite3 *db, sqlite3_stmt *stmt, const char *tag) {
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    ENIL_LOG(tag, "%s", sqlite3_errmsg(db));
    return rc;
  }
  return SQLITE_OK;
}

/* Positional bind descriptor for enil_db_exec(): folds the
 * prepare → bind 1..n → step_done tail that every simple mutating helper in
 * this file repeats. STR binds SQL NULL for a NULL pointer (via
 * bind_text_or_null); INT/I64 carry their value in `i`. Build entries with the
 * ENIL_S / ENIL_I / ENIL_L macros so the call site stays type-checked. */
typedef enum { ENIL_BIND_STR, ENIL_BIND_INT, ENIL_BIND_I64 } enil_bind_kind_t;

typedef struct {
  enil_bind_kind_t kind;
  const char      *s;
  long long        i;
} enil_bind_t;

#define ENIL_S(v) { ENIL_BIND_STR, (v),  0 }
#define ENIL_I(v) { ENIL_BIND_INT, NULL, (long long)(v) }
#define ENIL_L(v) { ENIL_BIND_I64, NULL, (long long)(v) }

/* Prepare `sql`, bind `binds[0..n-1]` to parameters 1..n in order, then run it
 * to completion via step_done. Returns SQLITE_OK on a clean write, else the
 * failing rc (logged under `tag`). The only thing a caller loses versus the
 * hand-written form is post-step work — functions that touch sticon rows or
 * log on success after the step keep the explicit shape. */
static int enil_db_exec(sqlite3 *db, const char *sql, const char *tag,
                        const enil_bind_t *binds, int n) {
  sqlite3_stmt *stmt;
  int rc, i;
  if (!db || !sql) return SQLITE_ERROR;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG(tag, "prepare: %s", sqlite3_errmsg(db));
    return rc;
  }
  for (i = 0; i < n; i++) {
    switch (binds[i].kind) {
      case ENIL_BIND_STR: bind_text_or_null(stmt, i + 1, binds[i].s);       break;
      case ENIL_BIND_INT: sqlite3_bind_int  (stmt, i + 1, (int)binds[i].i); break;
      case ENIL_BIND_I64: sqlite3_bind_int64(stmt, i + 1, binds[i].i);      break;
    }
  }
  return step_done(db, stmt, tag);
}

/* Single-text-bound scalar reads. Prepare `sql` (one '?' bound to `key`), read
 * column 0 of the first row, and finalize. The text variant returns a malloc'd
 * copy (NULL when the row/column is missing or empty — caller free()s); the
 * i64 variant returns `dflt` when the row/column is missing or NULL. Both are
 * the read-side siblings of row_exists. */
static char *db_scalar_text(sqlite3 *db, const char *sql,
                            const char *key, const char *tag) {
  sqlite3_stmt *stmt;
  char *result = NULL;
  if (!db || !key) return NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    ENIL_LOG(tag, "prepare: %s", sqlite3_errmsg(db));
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *v = sqlite3_column_text(stmt, 0);
    if (v && v[0]) result = strdup((const char *)v);
  }
  sqlite3_finalize(stmt);
  return result;
}

static long long db_scalar_i64(sqlite3 *db, const char *sql, const char *key,
                               long long dflt, const char *tag) {
  sqlite3_stmt *stmt;
  long long result = dflt;
  if (!db || !key) return dflt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    ENIL_LOG(tag, "prepare: %s", sqlite3_errmsg(db));
    return dflt;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW &&
      sqlite3_column_type(stmt, 0) != SQLITE_NULL)
    result = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return result;
}

/* ---- db_meta: local database state. See declarations in enil_db.h. ---- */

int enil_db_set_local_rev(sqlite3 *db, long long local_rev) {
  enil_bind_t b[] = { ENIL_S("localRev"), ENIL_L(local_rev) };
  if (!db || local_rev < 0) { LOG("set_local_rev", "invalid argument"); return SQLITE_ERROR; }
  return enil_db_exec(db, "INSERT OR REPLACE INTO db_meta (key, value) VALUES (?, ?)",
                      "Db.set_local_rev", b, 2);
}

long long enil_db_get_local_rev(sqlite3 *db) {
  long long rev = db_scalar_i64(db, "SELECT value FROM db_meta WHERE key = ?",
                                "localRev", 0, "Db.get_local_rev");
  return rev > 0 ? rev : 0;
}

int enil_db_has_local_rev(sqlite3 *db) {
  /* Present iff a value was ever persisted. db_scalar_text returns NULL when
     the row is missing; a localRev value is never empty, so non-NULL ⟺ set. */
  char *v = db_scalar_text(db, "SELECT value FROM db_meta WHERE key = ?",
                           "localRev", "Db.has_local_rev");
  int has = (v != NULL);
  free(v);
  return has;
}

/* Shared body for the four (sticker|sticon) × (image|thumb) path setters:
 * UPDATE … SET path = ?, width = ?, height = ? WHERE <id> = ? AND package_id = ?.
 * The only per-setter differences are the SQL statement and the log tag. */
static int set_path_dims(sqlite3 *db, const char *sql, const char *tag,
                         const char *path, int width, int height,
                         const char *id, const char *package_id) {
  enil_bind_t b[] = {
    ENIL_S(path), ENIL_I(width), ENIL_I(height), ENIL_S(id), ENIL_S(package_id)
  };
  if (!id || !package_id) return SQLITE_ERROR;
  return enil_db_exec(db, sql, tag, b, 5);
}

/* ---- Metadata-driven bind / read --------------------------------------------
 * Walk a struct's enil_field_t[] table — the same descriptor that drives
 * ENILStructDict and the JSON parse — so a typed row's SQL bind and column
 * read come from one source of truth instead of being hand-written per struct.
 * Neither touches a trailing `raw` cJSON (deliberately absent from the tables);
 * callers bind/read/free that explicitly. Both assume the SQL column order
 * matches the field-table order.
 * ----------------------------------------------------------------------------*/

/* Bind the `count` fields to parameters 1..count in order. JSON fields are
 * printed to a temporary string and bound with SQLITE_TRANSIENT (sqlite copies
 * immediately, so the temporary is freed right away). Returns the next free
 * 1-based parameter index (count+1) for any trailing columns the caller binds
 * by hand (e.g. raw_json, a hardcoded flag). */
static int bind_fields(sqlite3_stmt *stmt, const void *base,
                       const enil_field_t *fields, size_t count) {
  size_t i;
  /* Drift guard: the statement must have at least `count` parameters, or the
   * SQL and the field table have fallen out of sync (debug builds only). */
  assert(sqlite3_bind_parameter_count(stmt) >= (int)count);
  for (i = 0; i < count; i++) {
    const void *p   = (const char *)base + fields[i].offset;
    int         idx = (int)i + 1;
    switch (fields[i].type) {
      case ENIL_F_STR:
        bind_text_or_null(stmt, idx, *(const char *const *)p);
        break;
      case ENIL_F_INT:
      case ENIL_F_BOOL:
        sqlite3_bind_int(stmt, idx, *(const int *)p);
        break;
      case ENIL_F_I64:
        sqlite3_bind_int64(stmt, idx, *(const long long *)p);
        break;
      case ENIL_F_JSON: {
        cJSON *j = *(cJSON *const *)p;
        char  *s = j ? cJSON_PrintUnformatted(j) : NULL;
        bind_text_or_null(stmt, idx, s);
        free(s);
        break;
      }
    }
  }
  return (int)count + 1;
}

typedef int (*upsert_extra_bind_fn)(sqlite3_stmt *stmt, int idx,
                                    const void *base);

/* Shared body for typed INSERT OR REPLACE rows whose SQL is:
 * descriptor-driven fields, optional one-off extra fields, trailing raw_json.
 * The caller keeps validation local so error messages stay specific. */
static int upsert_fields_with_raw(sqlite3 *db, const char *sql,
                                  const char *tag, const void *base,
                                  cJSON *raw,
                                  const enil_field_t *fields,
                                  size_t field_count,
                                  upsert_extra_bind_fn extra_bind) {
  sqlite3_stmt *stmt;
  char *raw_str = NULL;
  int raw_idx, rc;

  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG(tag, "prepare: %s", sqlite3_errmsg(db));
    return rc;
  }

  if (raw) raw_str = cJSON_PrintUnformatted(raw);
  raw_idx = bind_fields(stmt, base, fields, field_count);
  if (extra_bind) raw_idx = extra_bind(stmt, raw_idx, base);
  bind_text_or_null(stmt, raw_idx, raw_str);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(raw_str);

  if (rc != SQLITE_DONE) {
    ENIL_LOG(tag, "%s", sqlite3_errmsg(db));
    return rc;
  }
  return SQLITE_OK;
}

/* Read columns 0..count-1 of the current row into the typed fields. STR fields
 * become NULL on an empty/NULL column when empty_as_null, otherwise "". JSON
 * columns are parsed (NULL/empty yields a NULL pointer). */
static void read_fields(sqlite3_stmt *stmt, void *base,
                        const enil_field_t *fields, size_t count,
                        int empty_as_null) {
  size_t i;
  /* Drift guard: the result set must have at least `count` columns, or the SQL
   * and the field table have fallen out of sync (debug builds only). */
  assert(sqlite3_column_count(stmt) >= (int)count);
  for (i = 0; i < count; i++) {
    void *p   = (char *)base + fields[i].offset;
    int   idx = (int)i;
    switch (fields[i].type) {
      case ENIL_F_STR: {
        const char *s = (const char *)sqlite3_column_text(stmt, idx);
        *(char **)p = empty_as_null ? dup_or_null(s)
                                    : dup_or_empty((const unsigned char *)s);
        break;
      }
      case ENIL_F_INT:
      case ENIL_F_BOOL:
        *(int *)p = sqlite3_column_int(stmt, idx);
        break;
      case ENIL_F_I64:
        *(long long *)p = sqlite3_column_int64(stmt, idx);
        break;
      case ENIL_F_JSON: {
        const char *s = (const char *)sqlite3_column_text(stmt, idx);
        *(cJSON **)p = (s && s[0]) ? cJSON_Parse(s) : NULL;
        break;
      }
    }
  }
}

/* Step every row of an already-prepared statement into a freshly malloc'd
 * array of `elem_size`-byte elements, reading each via read_fields. The caller
 * prepares (and binds) the statement; this finalizes it. The array grows
 * geometrically from 16; each element is zeroed before read_fields fills it.
 * On a realloc failure the partial array is returned (caller still owns and
 * must free the elements read so far). Returns SQLITE_OK on a clean finish or
 * SQLITE_NOMEM if growth failed. */
static int enil_db_collect(sqlite3_stmt *stmt, size_t elem_size,
                           const enil_field_t *fields, size_t count,
                           int empty_as_null, void **out, int *out_count) {
  char *arr = NULL;
  int   n = 0, cap = 0, rc = SQLITE_OK;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    void *elem;
    if (n == cap) {
      int   new_cap = cap ? cap * 2 : 16;
      char *tmp     = realloc(arr, (size_t)new_cap * elem_size);
      if (!tmp) { rc = SQLITE_NOMEM; break; }
      arr = tmp; cap = new_cap;
    }
    elem = arr + (size_t)n * elem_size;
    memset(elem, 0, elem_size);
    read_fields(stmt, elem, fields, count, empty_as_null);
    n++;
  }
  sqlite3_finalize(stmt);
  *out = arr; *out_count = n;
  return rc;
}

/* Column projection descriptor for the ad-hoc cJSON row-builders below. Unlike
 * enil_field_t these don't map to a C struct (no offset) — the array position
 * IS the result-set column index. Only STR/INT/I64 occur in these projections. */
typedef struct {
  const char        *key;
  enil_field_type_t  type;
} enil_col_t;

/* Build a cJSON object from the current statement row: column i is read by
 * cols[i].type and stored under cols[i].key. STR columns become "" when NULL;
 * INT/BOOL/I64 become JSON numbers. Returns a new object the caller owns. */
static cJSON *row_to_cjson(sqlite3_stmt *stmt, const enil_col_t *cols, size_t count) {
  cJSON *row = cJSON_CreateObject();
  size_t i;
  for (i = 0; i < count; i++) {
    int idx = (int)i;
    if (cols[i].type == ENIL_F_I64) {
      cJSON_AddNumberToObject(row, cols[i].key,
                              (double)sqlite3_column_int64(stmt, idx));
    } else if (cols[i].type == ENIL_F_STR || cols[i].type == ENIL_F_JSON) {
      const char *s = (const char *)sqlite3_column_text(stmt, idx);
      cJSON_AddStringToObject(row, cols[i].key, s ? s : "");
    } else { /* ENIL_F_INT / ENIL_F_BOOL */
      cJSON_AddNumberToObject(row, cols[i].key,
                              (double)sqlite3_column_int(stmt, idx));
    }
  }
  return row;
}

/* Drain every row of an already-prepared (and already-bound) statement into a
 * fresh cJSON array, building one object per row via row_to_cjson. Finalizes
 * the statement. The JSON-projection sibling of enil_db_collect: it folds the
 * while(step==ROW){ AddItemToArray(row_to_cjson(...)) } finalize loop that the
 * ad-hoc readers below all share, leaving each caller with just its prepare
 * and (optional) bind. */
static cJSON *rows_to_cjson_array(sqlite3_stmt *stmt,
                                  const enil_col_t *cols, size_t count) {
  cJSON *result = cJSON_CreateArray();
  while (sqlite3_step(stmt) == SQLITE_ROW)
    cJSON_AddItemToArray(result, row_to_cjson(stmt, cols, count));
  sqlite3_finalize(stmt);
  return result;
}

/* ============================================================================
 * Upsert the current user's profile into the accounts table.
 * ==========================================================================*/
int enil_db_talk_profile_upsert(sqlite3 *db, const talk_profile_t *p) {
  if (!db || !p || !p->mid) { LOG("talk_profile_upsert", "missing mid"); return SQLITE_ERROR; }
  return upsert_fields_with_raw(db, kSQL_UpsertAccount,
                                "Db.talk_profile_upsert", p, p->raw,
                                talk_profile_fields,
                                talk_profile_fields_count, NULL);
}

/* ============================================================================
 * Read the single accounts row back into a talk_profile_t. Caller must call
 * talk_profile_free on the result.
 * ==========================================================================*/
int enil_db_talk_profile_get(sqlite3 *db, talk_profile_t *out) {
  sqlite3_stmt *stmt;
  int rc;
  if (!db || !out) return -1;
  memset(out, 0, sizeof(*out));

  rc = sqlite3_prepare_v2(db, kSQL_GetAccount, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.talk_profile_get", "%s", sqlite3_errmsg(db));
    return -1;
  }
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }

  read_fields(stmt, out, talk_profile_fields, talk_profile_fields_count, 1);

  sqlite3_finalize(stmt);
  return 0;
}

/* --- Contacts --- */

/* ============================================================================
 * Upsert a single friend/contact row.
 * ==========================================================================*/
static int bind_contact_registered(sqlite3_stmt *stmt, int idx,
                                   const void *base) {
  const talk_contact_t *c = (const talk_contact_t *)base;
  sqlite3_bind_int(stmt, idx, c->registered);
  return idx + 1;
}

int enil_db_talk_contact_upsert(sqlite3 *db, const talk_contact_t *c) {
  if (!db || !c || !c->mid) { LOG("talk_contact_upsert", "missing mid"); return SQLITE_ERROR; }
  return upsert_fields_with_raw(db, kSQL_UpsertContact,
                                "Db.talk_contact_upsert", c, c->raw,
                                talk_contact_fields,
                                talk_contact_fields_count,
                                bind_contact_registered);
}

/* ============================================================================
 * Load every contact row. Caller frees each entry with talk_contact_free.
 * ==========================================================================*/
int enil_db_talk_contact_get_all(sqlite3 *db, talk_contact_t **out, int *out_count) {
  sqlite3_stmt *stmt;
  int rc, i;

  if (!db || !out || !out_count) return -1;
  *out = NULL; *out_count = 0;

  rc = sqlite3_prepare_v2(db, kSQL_GetContacts, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.talk_contact_get_all", "%s", sqlite3_errmsg(db));
    return -1;
  }

  enil_db_collect(stmt, sizeof(talk_contact_t),
                  talk_contact_fields, talk_contact_fields_count, 1,
                  (void **)out, out_count);
  /* GetContacts filters to registered = 1 already. */
  for (i = 0; i < *out_count; i++) (*out)[i].registered = 1;
  return 0;
}

/* Generic single-key existence probe. Returns 1 if a row matches, 0 if not.
 * On any error returns 1 (treat as "present" so callers don't trigger a
 * redundant network backfill against a broken/locked db). */
static int row_exists(sqlite3 *db, const char *sql, const char *key) {
  sqlite3_stmt *stmt;
  int found;
  if (!db || !sql || !key || !key[0]) return 1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    ENIL_LOG("Db.row_exists", "prepare: %s", sqlite3_errmsg(db));
    return 1;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  found = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
  sqlite3_finalize(stmt);
  return found;
}

int enil_db_chat_exists(sqlite3 *db, const char *chat_mid) {
  return row_exists(db,
    "SELECT 1 FROM chats_v2 WHERE chatMid = ? LIMIT 1", chat_mid);
}

/* A 1:1 chat that has been messaged lives in message_boxes_v2 (keyed by the
 * peer mid as `id`); group/room chats also get a chats_v2 row. */
int enil_db_message_box_exists(sqlite3 *db, const char *chat_mid) {
  return row_exists(db,
    "SELECT 1 FROM message_boxes_v2 WHERE id = ? LIMIT 1", chat_mid);
}

int enil_db_contact_exists(sqlite3 *db, const char *mid) {
  return row_exists(db,
    "SELECT 1 FROM contacts_v2 WHERE mid = ? LIMIT 1", mid);
}

/* ============================================================================
 * Release malloc'd fields and raw cJSON inside a talk_contact_t.
 * ==========================================================================*/
void talk_contact_free(talk_contact_t *c) {
  if (!c) return;
  enil_field_free_all(c, talk_contact_fields, talk_contact_fields_count);
  if (c->raw) { cJSON_Delete(c->raw); c->raw = NULL; }
}

const enil_field_t talk_contact_fields[] = {
  { "mid",                   offsetof(talk_contact_t, mid),                    ENIL_F_STR },
  { "displayName",           offsetof(talk_contact_t, display_name),           ENIL_F_STR },
  { "displayNameOverridden", offsetof(talk_contact_t, display_name_overridden), ENIL_F_STR },
  { "statusMessage",         offsetof(talk_contact_t, status_message),         ENIL_F_STR },
  { "picturePath",           offsetof(talk_contact_t, picture_path),           ENIL_F_STR },
};
const size_t talk_contact_fields_count =
  sizeof(talk_contact_fields) / sizeof(talk_contact_fields[0]);

/* --- Chats (typed) --- */

/* ============================================================================
 * Upsert a chat row in the chats_v2 table.
 * ==========================================================================*/
int enil_db_chat_upsert(sqlite3 *db, const talk_chat_t *c) {
  if (!db || !c || !c->chatMid) { LOG("chat_upsert", "missing chatMid"); return SQLITE_ERROR; }
  return upsert_fields_with_raw(db, kSQL_UpsertChatV2,
                                "Db.chat_upsert", c, c->raw,
                                talk_chat_fields, talk_chat_fields_count,
                                NULL);
}

/* --- MessageBoxes (typed) --- */

/* ============================================================================
 * Upsert a message-box row (per-chat read state and unread counters).
 * ==========================================================================*/
int enil_db_message_box_upsert(sqlite3 *db, const talk_message_box_t *mb) {
  sqlite3_stmt *stmt;
  char *raw_str = NULL;
  int rc;

  if (!db || !mb || !mb->id) { LOG("message_box_upsert", "missing id"); return SQLITE_ERROR; }

  rc = sqlite3_prepare_v2(db, kSQL_InsertMessageBoxV2Row, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_box_upsert", "prepare insert: %s", sqlite3_errmsg(db));
    return rc;
  }
  bind_text_or_null(stmt, 1, mb->id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    ENIL_LOG("Db.message_box_upsert", "insert: %s", sqlite3_errmsg(db));
    return rc;
  }

  rc = sqlite3_prepare_v2(db, kSQL_UpdateMessageBoxV2, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_box_upsert", "prepare update: %s", sqlite3_errmsg(db));
    return rc;
  }
  if (mb->raw) raw_str = cJSON_PrintUnformatted(mb->raw);

  sqlite3_bind_int  (stmt, 1, mb->midType);
  bind_text_or_null (stmt, 2, mb->lastDeliveredMessageId);
  sqlite3_bind_int64(stmt, 3, mb->lastDeliveredTime);
  bind_text_or_null (stmt, 4, mb->lastSeenMessageId);
  sqlite3_bind_int64(stmt, 5, mb->unreadCount);
  bind_text_or_null (stmt, 6, raw_str);
  bind_text_or_null (stmt, 7, mb->id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(raw_str);
  if (rc != SQLITE_DONE) {
    ENIL_LOG("Db.message_box_upsert", "update: %s", sqlite3_errmsg(db));
    return rc;
  }
  return SQLITE_OK;
}

int enil_db_message_box_upsert_live_message(sqlite3 *db,
                                            const char *chat_mid,
                                            int mid_type,
                                            const char *last_message_id,
                                            long long last_message_time,
                                            int incoming,
                                            const char *message_id) {
  sqlite3_stmt *stmt;
  int rc;

  if (!db || !chat_mid) {
    LOG("message_box_upsert_live_message", "missing chat id");
    return SQLITE_ERROR;
  }

  rc = sqlite3_prepare_v2(db, kSQL_InsertMessageBoxV2Row, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_box_upsert_live_message",
             "prepare insert: %s", sqlite3_errmsg(db));
    return rc;
  }
  bind_text_or_null(stmt, 1, chat_mid);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    ENIL_LOG("Db.message_box_upsert_live_message",
             "insert: %s", sqlite3_errmsg(db));
    return rc;
  }

  rc = sqlite3_prepare_v2(db, kSQL_UpdateMessageBoxV2LiveMessage, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_box_upsert_live_message",
             "prepare update: %s", sqlite3_errmsg(db));
    return rc;
  }

  sqlite3_bind_int  (stmt, 1, mid_type);
  bind_text_or_null (stmt, 2, last_message_id);
  sqlite3_bind_int64(stmt, 3, last_message_time);
  sqlite3_bind_int  (stmt, 4, incoming ? 1 : 0);
  bind_text_or_null (stmt, 5, message_id);
  bind_text_or_null (stmt, 6, message_id);
  bind_text_or_null (stmt, 7, message_id);
  bind_text_or_null (stmt, 8, chat_mid);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    ENIL_LOG("Db.message_box_upsert_live_message",
             "update: %s", sqlite3_errmsg(db));
    return rc;
  }
  return SQLITE_OK;
}

/* ============================================================================
 * Load every message-box row. Caller frees with talk_message_box_free.
 * ==========================================================================*/
int enil_db_message_box_get_all(sqlite3 *db, talk_message_box_t **out, int *out_count) {
  sqlite3_stmt *stmt;
  int rc;

  if (!db || !out || !out_count) return -1;
  *out = NULL; *out_count = 0;

  rc = sqlite3_prepare_v2(db, kSQL_GetMessageBoxesV2, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_box_get_all", "%s", sqlite3_errmsg(db));
    return -1;
  }
  enil_db_collect(stmt, sizeof(talk_message_box_t),
                  talk_message_box_fields, talk_message_box_fields_count, 1,
                  (void **)out, out_count);
  return 0;
}

/* --- Chat summary (joined) --- */

/* ============================================================================
 * Build the chat-list sidebar projection (chat + last message + unread).
 * ==========================================================================*/
int enil_db_chat_summary_get_all(sqlite3 *db, chat_summary_t **out, int *out_count) {
  sqlite3_stmt *stmt;
  int rc;

  if (!db || !out || !out_count) return -1;
  *out = NULL; *out_count = 0;

  rc = sqlite3_prepare_v2(db, kSQL_GetChatSummaries, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.chat_summary_get_all", "%s", sqlite3_errmsg(db));
    return -1;
  }
  enil_db_collect(stmt, sizeof(chat_summary_t),
                  chat_summary_fields, chat_summary_fields_count, 1,
                  (void **)out, out_count);
  return 0;
}

/* ============================================================================
 * Single chat-list row by message-box id. Returns 0 on a hit (out populated;
 * caller owns its strings via chat_summary_free), -1 when the chat has no
 * message_boxes_v2 row (never messaged, or just locally deleted) so the caller
 * can drop it from the list. Mirrors get_all's column reading for one row.
 * ==========================================================================*/
int enil_db_chat_summary_get_one(sqlite3 *db, const char *chatMid,
                                 chat_summary_t *out) {
  sqlite3_stmt *stmt;
  int rc;

  if (!db || !chatMid || !out) return -1;
  memset(out, 0, sizeof(*out));

  rc = sqlite3_prepare_v2(db, kSQL_GetChatSummaryOne, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.chat_summary_get_one", "%s", sqlite3_errmsg(db));
    return -1;
  }
  sqlite3_bind_text(stmt, 1, chatMid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }
  read_fields(stmt, out, chat_summary_fields, chat_summary_fields_count, 1);
  sqlite3_finalize(stmt);
  return 0;
}

/* ============================================================================
 * Release malloc'd fields inside a chat_summary_t.
 * ==========================================================================*/
void chat_summary_free(chat_summary_t *s) {
  if (!s) return;
  enil_field_free_all(s, chat_summary_fields, chat_summary_fields_count);
}

const enil_field_t chat_summary_fields[] = {
  { "chatMid",           offsetof(chat_summary_t, chatMid),           ENIL_F_STR },
  { "type",              offsetof(chat_summary_t, type),              ENIL_F_INT },
  { "displayName",       offsetof(chat_summary_t, displayName),       ENIL_F_STR },
  { "unreadCount",       offsetof(chat_summary_t, unreadCount),       ENIL_F_I64 },
  { "lastDeliveredTime", offsetof(chat_summary_t, lastDeliveredTime), ENIL_F_I64 },
  { "isInvited",         offsetof(chat_summary_t, isInvited),         ENIL_F_BOOL },
};
const size_t chat_summary_fields_count =
  sizeof(chat_summary_fields) / sizeof(chat_summary_fields[0]);

/* --- Messages --- */

/* ============================================================================
 * Parse a single LINE message JSON entry into a talk_message_t.
 * Returns 0 on success.
 * ==========================================================================*/
int talk_message_parse(cJSON *entry, const char *chat_id, talk_message_t *out) {
  cJSON *chunks_item, *meta_item;
  if (!entry || !out) return -1;
  memset(out, 0, sizeof(*out));
  if (enil_json_dup_string(entry, "id", &out->id) < 0) return -1;
  enil_json_dup_string_opt(entry, "from",                      &out->from_mid);
  enil_json_dup_string_opt(entry, "to",                        &out->to_mid);
  enil_json_get_int   (entry,     "toType",                    &out->toType);
  enil_json_get_int   (entry,     "sessionId",                 &out->sessionId);
  enil_json_get_int64 (entry,     "createdTime",               &out->createdTime);
  enil_json_get_int   (entry,     "contentType",               &out->contentType);
  enil_json_get_bool  (entry,     "hasContent",                &out->hasContent);
  enil_json_dup_string_opt(entry, "text",                      &out->text);
  enil_json_dup_string_opt(entry, "relatedMessageId",          &out->relatedMessageId);
  enil_json_get_int   (entry,     "messageRelationType",       &out->messageRelationType);
  enil_json_get_int   (entry,     "relatedMessageServiceCode", &out->relatedMessageServiceCode);

  meta_item = cJSON_GetObjectItemCaseSensitive(entry, "contentMetadata");
  if (cJSON_IsObject(meta_item)) out->contentMetadata = cJSON_Duplicate(meta_item, 1);
  chunks_item = cJSON_GetObjectItemCaseSensitive(entry, "chunks");
  if (cJSON_IsArray(chunks_item)) out->chunks = cJSON_Duplicate(chunks_item, 1);

  if (chat_id) out->chat_id = strdup(chat_id);
  out->raw = cJSON_Duplicate(entry, 1);
  return 0;
}

/* ============================================================================
 * Release every malloc'd field and embedded cJSON inside a talk_message_t.
 * ==========================================================================*/
void talk_message_free(talk_message_t *m) {
  if (!m) return;
  enil_field_free_all(m, talk_message_fields, talk_message_fields_count);
  if (m->raw) { cJSON_Delete(m->raw); m->raw = NULL; }
}

const enil_field_t talk_message_fields[] = {
  { "id",                        offsetof(talk_message_t, id),                        ENIL_F_STR  },
  { "from",                      offsetof(talk_message_t, from_mid),                  ENIL_F_STR  },
  { "to",                        offsetof(talk_message_t, to_mid),                    ENIL_F_STR  },
  { "toType",                    offsetof(talk_message_t, toType),                    ENIL_F_INT  },
  { "sessionId",                 offsetof(talk_message_t, sessionId),                 ENIL_F_INT  },
  { "createdTime",               offsetof(talk_message_t, createdTime),               ENIL_F_I64  },
  { "contentType",               offsetof(talk_message_t, contentType),               ENIL_F_INT  },
  { "hasContent",                offsetof(talk_message_t, hasContent),                ENIL_F_BOOL },
  { "text",                      offsetof(talk_message_t, text),                      ENIL_F_STR  },
  { "contentMetadata",           offsetof(talk_message_t, contentMetadata),           ENIL_F_JSON },
  { "chunks",                    offsetof(talk_message_t, chunks),                    ENIL_F_JSON },
  { "relatedMessageId",          offsetof(talk_message_t, relatedMessageId),          ENIL_F_STR  },
  { "messageRelationType",       offsetof(talk_message_t, messageRelationType),       ENIL_F_INT  },
  { "relatedMessageServiceCode", offsetof(talk_message_t, relatedMessageServiceCode), ENIL_F_INT  },
  { "chat_id",                   offsetof(talk_message_t, chat_id),                   ENIL_F_STR  },
};
const size_t talk_message_fields_count =
  sizeof(talk_message_fields) / sizeof(talk_message_fields[0]);

/* --- Operations (SSE push payload — transient, no DB table) --- */

/* ============================================================================
 * Map a LINE operation type integer to a human-readable constant name.
 * ==========================================================================*/
const char *enil_op_type_name(int type) {
  switch (type) {
#define ENIL_OP_NAME_CASE(name, value) case value: return #name;
    ENIL_OP_LIST(ENIL_OP_NAME_CASE)
#undef ENIL_OP_NAME_CASE
    default: return "UNKNOWN";
  }
}


static const char *operation_chat_id(cJSON *msg, const char *my_mid) {
  cJSON *to_type_item = cJSON_GetObjectItem(msg, "toType");
  const char *from_mid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "from"));
  const char *to_mid   = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "to"));
  int to_type = cJSON_IsNumber(to_type_item) ? (int)to_type_item->valuedouble : 0;
  if (to_type == 0) {
    if (my_mid && to_mid && strcmp(to_mid, my_mid) == 0) return from_mid;
    return to_mid;
  }
  return to_mid;
}

/* ============================================================================
 * Parse an SSE operation event into a typed talk_operation_t including the
 * embedded message and resolved chat id. Returns 0 on success.
 * ==========================================================================*/
int talk_operation_parse(cJSON *root, const char *my_mid, talk_operation_t *out) {
  cJSON *msg_item;
  if (!root || !out) return -1;
  memset(out, 0, sizeof(*out));
  enil_json_get_int64(root,     "revision",    &out->revision);
  enil_json_get_int  (root,     "type",        &out->type);
  enil_json_get_int  (root,     "reqSeq",      &out->reqSeq);
  enil_json_get_int64(root,     "createdTime", &out->createdTime);
  enil_json_dup_string_opt(root, "param1",     &out->param1);
  enil_json_dup_string_opt(root, "param2",     &out->param2);
  enil_json_dup_string_opt(root, "param3",     &out->param3);

  msg_item = cJSON_GetObjectItemCaseSensitive(root, "message");
  if (cJSON_IsObject(msg_item)) {
    const char *chat_id = operation_chat_id(msg_item, my_mid);
    out->message = (talk_message_t *)calloc(1, sizeof(*out->message));
    if (!out->message) { talk_operation_free(out); return -1; }
    if (talk_message_parse(msg_item, chat_id, out->message) != 0) {
      free(out->message); out->message = NULL;
    }
  }

  out->raw = cJSON_Duplicate(root, 1);
  return 0;
}

/* ============================================================================
 * Release malloc'd fields and embedded message inside a talk_operation_t.
 * ==========================================================================*/
void talk_operation_free(talk_operation_t *op) {
  if (!op) return;
  enil_field_free_all(op, talk_operation_fields, talk_operation_fields_count);
  if (op->message) {
    talk_message_free(op->message);
    free(op->message);
    op->message = NULL;
  }
  if (op->raw) { cJSON_Delete(op->raw); op->raw = NULL; }
}

const enil_field_t talk_operation_fields[] = {
  { "revision",    offsetof(talk_operation_t, revision),    ENIL_F_I64 },
  { "type",        offsetof(talk_operation_t, type),        ENIL_F_INT },
  { "reqSeq",      offsetof(talk_operation_t, reqSeq),      ENIL_F_INT },
  { "createdTime", offsetof(talk_operation_t, createdTime), ENIL_F_I64 },
  { "param1",      offsetof(talk_operation_t, param1),      ENIL_F_STR },
  { "param2",      offsetof(talk_operation_t, param2),      ENIL_F_STR },
  { "param3",      offsetof(talk_operation_t, param3),      ENIL_F_STR },
};
const size_t talk_operation_fields_count =
  sizeof(talk_operation_fields) / sizeof(talk_operation_fields[0]);

/* ============================================================================
 * Upsert a fully-populated talk_message_t row, writing both raw_json and any
 * already-decrypted plaintext into the messages_v2 table.
 * ==========================================================================*/
/* Build the storage-shape JSON for messages_v2.reactions_json by
 * transforming the wire array (m->raw->reactions). Returns:
 *   - field present and non-empty → malloc'd JSON string, *had_field=1.
 *   - field present but empty      → NULL, *had_field=1 (caller writes NULL).
 *   - field absent (e.g. SSE-only) → NULL, *had_field=0 (caller leaves alone). */
static char *reactions_extract_for_storage(const talk_message_t *m,
                                           int *had_field) {
  cJSON *raw_arr, *child, *xformed, *src_rt, *paid, *predef;
  cJSON *src_mid, *src_at;
  char *out;
  int produced;

  if (had_field) *had_field = 0;
  if (!m || !cJSON_IsObject(m->raw)) return NULL;
  raw_arr = cJSON_GetObjectItem(m->raw, "reactions");
  if (!cJSON_IsArray(raw_arr)) return NULL;
  if (had_field) *had_field = 1;
  if (cJSON_GetArraySize(raw_arr) == 0) return NULL;

  xformed = cJSON_CreateArray();
  if (!xformed) return NULL;
  produced = 0;
  for (child = raw_arr->child; child; child = child->next) {
    cJSON *entry;
    src_mid = cJSON_GetObjectItem(child, "fromUserMid");
    src_at  = cJSON_GetObjectItem(child, "atMillis");
    src_rt  = cJSON_GetObjectItem(child, "reactionType");
    if (!cJSON_IsString(src_mid) || !cJSON_IsObject(src_rt)) continue;
    entry = cJSON_CreateObject();
    if (!entry) continue;
    cJSON_AddStringToObject(entry, "reactorMid", src_mid->valuestring);
    predef = cJSON_GetObjectItem(src_rt, "predefinedReactionType");
    paid   = cJSON_GetObjectItem(src_rt, "paidReactionType");
    if (cJSON_IsNumber(predef))
      cJSON_AddNumberToObject(entry, "predefinedReactionType",
                              predef->valuedouble);
    if (cJSON_IsObject(paid)) {
      cJSON *pid = cJSON_GetObjectItem(paid, "productId");
      cJSON *eid = cJSON_GetObjectItem(paid, "emojiId");
      if (cJSON_IsString(pid))
        cJSON_AddStringToObject(entry, "productId", pid->valuestring);
      if (cJSON_IsString(eid))
        cJSON_AddStringToObject(entry, "emojiId",   eid->valuestring);
    }
    /* atMillis is a string on the wire — convert to numeric createdTime. */
    if (cJSON_IsString(src_at) && src_at->valuestring && src_at->valuestring[0])
      cJSON_AddNumberToObject(entry, "createdTime",
                              (double)atoll(src_at->valuestring));
    else if (cJSON_IsNumber(src_at))
      cJSON_AddNumberToObject(entry, "createdTime", src_at->valuedouble);
    cJSON_AddItemToArray(xformed, entry);
    produced = 1;
  }
  if (!produced) { cJSON_Delete(xformed); return NULL; }
  out = cJSON_PrintUnformatted(xformed);
  cJSON_Delete(xformed);
  return out;
}

int enil_db_message_upsert(sqlite3 *db, const talk_message_t *m) {
  const char   *sticker_id     = NULL;
  const char   *sticker_pkg_id = NULL;
  const char   *sticon_own     = NULL;
  const char   *media_sid      = NULL;
  const char   *media_oid      = NULL;
  const char   *media_obs_pop  = NULL;
  const char   *media_e2ee_ver = NULL;
  sqlite3_int64 media_size     = 0;
  char         *replace_json   = NULL;
  char         *raw_json       = NULL;
  char         *meta_json      = NULL;
  char         *chunks_json    = NULL;
  char         *reactions_json = NULL;
  int           had_reactions  = 0;
  int           decrypt_status;
  sqlite3_stmt *stmt;
  int           rc;

  if (!db || !m || !m->id || !m->id[0] || !m->chat_id || !m->chat_id[0])
    return SQLITE_ERROR;

  decrypt_status = (m->text && m->text[0]) ? ENIL_DECRYPT_PLAINTEXT
                                            : ENIL_DECRYPT_PENDING;

  if (cJSON_IsObject(m->contentMetadata)) {
    cJSON *meta = m->contentMetadata;
    if (m->contentType == 7) {
      sticker_id     = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKID"));
      sticker_pkg_id = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKPKGID"));
    }
    if (m->contentType == 1)
      parse_media_meta(meta, &media_sid, &media_oid, &media_obs_pop,
                       &media_e2ee_ver, &media_size);
    sticon_own   = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STICON_OWNERSHIP"));
    replace_json = extract_replace_json_from_meta(meta);
    meta_json    = cJSON_PrintUnformatted(meta);
  }
  if (cJSON_IsArray(m->chunks))
    chunks_json = cJSON_PrintUnformatted(m->chunks);
  if (m->raw) raw_json = cJSON_PrintUnformatted(m->raw);
  reactions_json = reactions_extract_for_storage(m, &had_reactions);

  rc = sqlite3_prepare_v2(db, kSQL_UpsertMessageV2, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_upsert", "prepare: %s", sqlite3_errmsg(db));
    free(replace_json); free(raw_json); free(meta_json); free(chunks_json);
    free(reactions_json);
    return rc;
  }

  bind_text_or_null(stmt,  1, m->id);
  bind_text_or_null(stmt,  2, m->from_mid);
  bind_text_or_null(stmt,  3, m->to_mid);
  sqlite3_bind_int (stmt,  4, m->toType);
  sqlite3_bind_int (stmt,  5, m->sessionId);
  sqlite3_bind_int64(stmt, 6, (sqlite3_int64)m->createdTime);
  sqlite3_bind_int (stmt,  7, m->contentType);
  sqlite3_bind_int (stmt,  8, m->hasContent);
  bind_text_or_null(stmt,  9, m->text);
  bind_text_or_null(stmt, 10, meta_json);
  bind_text_or_null(stmt, 11, chunks_json);
  bind_text_or_null(stmt, 12, m->relatedMessageId);
  sqlite3_bind_int (stmt, 13, m->messageRelationType);
  sqlite3_bind_int (stmt, 14, m->relatedMessageServiceCode);
  bind_text_or_null(stmt, 15, raw_json);
  bind_text_or_null(stmt, 16, m->chat_id);
  sqlite3_bind_int (stmt, 17, decrypt_status);
  bind_text_or_null(stmt, 18, replace_json);
  bind_text_or_null(stmt, 19, sticker_id);
  bind_text_or_null(stmt, 20, sticker_pkg_id);
  bind_text_or_null(stmt, 21, sticon_own);
  bind_text_or_null(stmt, 22, media_sid);
  bind_text_or_null(stmt, 23, media_oid);
  bind_text_or_null(stmt, 24, media_obs_pop);
  bind_text_or_null(stmt, 25, media_e2ee_ver);
  sqlite3_bind_int64(stmt, 26, media_size);
  bind_text_or_null(stmt, 27, reactions_json);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  /* When the wire response included a reactions field (full sync via
     getRecentMessagesV2 always does; SSE op 25/26 message echoes may not),
     it's the source of truth — overwrite whatever the SSE handler had been
     mutating up to this point. */
  if (rc == SQLITE_DONE && had_reactions && sqlite3_changes(db) == 0) {
    sqlite3_stmt *rstmt;
    if (sqlite3_prepare_v2(db,
          "UPDATE messages_v2 SET reactions_json = ? WHERE id = ?",
          -1, &rstmt, NULL) == SQLITE_OK) {
      bind_text_or_null(rstmt, 1, reactions_json);
      sqlite3_bind_text (rstmt, 2, m->id, -1, SQLITE_TRANSIENT);
      (void)sqlite3_step(rstmt);
      sqlite3_finalize(rstmt);
    }
  }
  /* INSERT OR IGNORE: if a row already existed (SSE op echo raced ahead
     of the local send, or vice versa) and we now have plaintext that the
     stored row lacks, fill the gap. Conservative: only writes when current
     row has no text AND incoming does. */
  if (rc == SQLITE_DONE && sqlite3_changes(db) == 0
      && m->text && m->text[0]) {
    sqlite3_stmt *fillstmt;
    int frc = sqlite3_prepare_v2(db,
      "UPDATE messages_v2"
      "   SET text = ?,"
      "       contentMetadata = COALESCE(NULLIF(contentMetadata, ''), ?),"
      "       chunks          = COALESCE(NULLIF(chunks, ''), ?),"
      "       raw_json        = COALESCE(NULLIF(raw_json, ''), ?),"
      "       decrypt_status  = ?"
      " WHERE id = ? AND (text IS NULL OR text = '')",
      -1, &fillstmt, NULL);
    if (frc == SQLITE_OK) {
      bind_text_or_null(fillstmt, 1, m->text);
      bind_text_or_null(fillstmt, 2, meta_json);
      bind_text_or_null(fillstmt, 3, chunks_json);
      bind_text_or_null(fillstmt, 4, raw_json);
      sqlite3_bind_int  (fillstmt, 5, decrypt_status);
      sqlite3_bind_text (fillstmt, 6, m->id, -1, SQLITE_TRANSIENT);
      (void)sqlite3_step(fillstmt);
      sqlite3_finalize(fillstmt);
    }
  }
  if (rc == SQLITE_DONE && replace_json) {
    int fill_rc = fill_message_replace_json(db, m->id, replace_json);
    if (fill_rc != SQLITE_OK) rc = fill_rc;
  }
  if (rc == SQLITE_DONE && replace_json)
    replace_message_sticon_rows(db, m->id, replace_json, sticon_own);
  free(replace_json);
  free(raw_json);
  free(meta_json);
  free(chunks_json);
  free(reactions_json);

  if (rc != SQLITE_DONE) {
    ENIL_LOG("Db.message_upsert", "%s", sqlite3_errmsg(db));
    return rc;
  }
  return SQLITE_OK;
}

/* ============================================================================
 * Release the per-row strings inside a message_row_t projection.
 * ==========================================================================*/
void message_row_free(message_row_t *m) {
  if (!m) return;
  enil_field_free_all(m, message_row_fields, message_row_fields_count);
}

const enil_field_t message_row_fields[] = {
  { "message_id",     offsetof(message_row_t, message_id),     ENIL_F_STR  },
  { "sender_name",    offsetof(message_row_t, sender_name),    ENIL_F_STR  },
  { "text",           offsetof(message_row_t, text),           ENIL_F_STR  },
  { "text_html",      offsetof(message_row_t, text_html),      ENIL_F_STR  },
  { "media_path",     offsetof(message_row_t, media_path),     ENIL_F_STR  },
  { "thumb_path",     offsetof(message_row_t, thumb_path),     ENIL_F_STR  },
  { "from_mid",       offsetof(message_row_t, from_mid),       ENIL_F_STR  },
  { "sticker_id",     offsetof(message_row_t, sticker_id),     ENIL_F_STR  },
  { "sticker_pkg_id", offsetof(message_row_t, sticker_pkg_id), ENIL_F_STR  },
  { "sticker_path",   offsetof(message_row_t, sticker_path),   ENIL_F_STR  },
  { "avatar_path",    offsetof(message_row_t, avatar_path),    ENIL_F_STR  },
  { "reactions_json", offsetof(message_row_t, reactions_json), ENIL_F_STR  },
  { "created_at",     offsetof(message_row_t, created_at),     ENIL_F_I64  },
  { "content_type",   offsetof(message_row_t, content_type),   ENIL_F_INT  },
  { "orig_width",     offsetof(message_row_t, orig_width),     ENIL_F_INT  },
  { "orig_height",    offsetof(message_row_t, orig_height),    ENIL_F_INT  },
  { "thumb_width",    offsetof(message_row_t, thumb_width),    ENIL_F_INT  },
  { "thumb_height",   offsetof(message_row_t, thumb_height),   ENIL_F_INT  },
  { "sticker_width",  offsetof(message_row_t, sticker_width),  ENIL_F_INT  },
  { "sticker_height", offsetof(message_row_t, sticker_height), ENIL_F_INT  },
  { "is_outgoing",    offsetof(message_row_t, is_outgoing),    ENIL_F_INT  },
  { "is_deleted",     offsetof(message_row_t, is_deleted),     ENIL_F_INT  },
  { "decrypt_status", offsetof(message_row_t, decrypt_status), ENIL_F_INT  }
};
const size_t message_row_fields_count =
  sizeof(message_row_fields) / sizeof(message_row_fields[0]);

/* Newest-first page of a chat's messages older than `before` (0 = newest).
 * Bind: 1=chat_id, 2=before, 3=limit. Reversed to ascending by the caller. */
static const char * const kSQL_GetMessagesPage =
  "SELECT m.id,"
  "  COALESCE(c.displayNameOverridden, c.displayName, m.\"from\", '') AS sender_name,"
  "  m.createdTime,"
  "  COALESCE(m.text, '') AS text,"
  "  m.contentType,"
  "  COALESCE(m.media_path, '') AS media_path,"
  "  COALESCE(m.\"from\", '') AS from_mid,"
  "  m.orig_width,"
  "  m.orig_height,"
  "  COALESCE(m.thumb_path, '') AS thumb_path,"
  "  m.thumb_width,"
  "  m.thumb_height,"
  "  COALESCE(m.sticker_id, '') AS sticker_id,"
  "  COALESCE(m.sticker_pkg_id, '') AS sticker_pkg_id,"
  "  m.reactions_json,"
  "  m.is_deleted,"
  "  m.decrypt_status"
  " FROM messages_v2 m"
  " LEFT JOIN contacts_v2 c ON m.\"from\" = c.mid"
  " WHERE m.chat_id = ? AND (?2 = 0 OR m.createdTime < ?2)"
  " ORDER BY m.createdTime DESC"
  " LIMIT ?3";

/* Populate one message_row_t from the current stmt row (kSQL_GetMessagesPage
 * column order). */
static void fill_message_row(sqlite3 *db, sqlite3_stmt *stmt,
                             const char *my_mid, message_row_t *m) {
  char stk_path_buf[512];
  int stk_w = 0, stk_h = 0;
  memset(m, 0, sizeof(*m));
  m->message_id     = dup_or_empty(sqlite3_column_text(stmt, 0));
  m->sender_name    = dup_or_empty(sqlite3_column_text(stmt, 1));
  m->created_at     = sqlite3_column_int64(stmt, 2);
  m->text           = dup_or_empty(sqlite3_column_text(stmt, 3));
  m->content_type   = sqlite3_column_int(stmt, 4);
  m->media_path     = dup_or_empty(sqlite3_column_text(stmt, 5));
  m->from_mid       = dup_or_empty(sqlite3_column_text(stmt, 6));
  m->orig_width     = sqlite3_column_int(stmt, 7);
  m->orig_height    = sqlite3_column_int(stmt, 8);
  m->thumb_path     = dup_or_empty(sqlite3_column_text(stmt, 9));
  m->thumb_width    = sqlite3_column_int(stmt, 10);
  m->thumb_height   = sqlite3_column_int(stmt, 11);
  m->sticker_id     = dup_or_empty(sqlite3_column_text(stmt, 12));
  m->sticker_pkg_id = dup_or_empty(sqlite3_column_text(stmt, 13));
  m->reactions_json = dup_or_null((const char *)sqlite3_column_text(stmt, 14));
  m->is_deleted     = sqlite3_column_int(stmt, 15);
  m->decrypt_status = sqlite3_column_int(stmt, 16);
  m->is_outgoing    = (my_mid && m->from_mid && strcmp(m->from_mid, my_mid) == 0) ? 1 : 0;
  m->text_html      = enil_message_text_html(db, m->message_id, m->text);
  if (!m->text_html) m->text_html = strdup("");
  if (m->content_type == 7 &&
      enil_message_sticker_image_info(db, m->sticker_id, m->sticker_pkg_id,
                                       stk_path_buf, sizeof(stk_path_buf),
                                       &stk_w, &stk_h)) {
    m->sticker_path   = strdup(stk_path_buf);
    m->sticker_width  = stk_w;
    m->sticker_height = stk_h;
  } else {
    m->sticker_path = strdup("");
  }
  /* Avatar: relative path the WebView resolves against enilDir/. Only set
   * for incoming messages — outgoing bubbles don't render the avatar
   * column at all. */
  m->avatar_path = enil_db_avatar_relpath(db,
                                          m->is_outgoing ? NULL : m->from_mid);
}

/* Helper for fill_message_row + the SSE incremental-append path
 * (enil_sync_js_for_sse_message). Keeps the "where does the avatar file
 * live on disk and does it exist" logic in exactly one place so both
 * render paths produce the same `avatar_path` for a given mid. */
char *enil_db_avatar_relpath(sqlite3 *db, const char *from_mid) {
  const char *dbfile, *slash;
  size_t dlen;
  char dir[1024], abs[1700], rel[600];
  struct stat stt;
  if (!db || !from_mid || !from_mid[0]) return strdup("");
  dbfile = sqlite3_db_filename(db, "main");
  if (!dbfile) return strdup("");
  slash = strrchr(dbfile, '/');
  if (!slash) return strdup("");
  dlen = (size_t)(slash - dbfile);
  if (!dlen || dlen >= sizeof(dir)) return strdup("");
  memcpy(dir, dbfile, dlen);
  dir[dlen] = '\0';
  snprintf(rel, sizeof(rel), "avatars/%s.jpg", from_mid);
  snprintf(abs, sizeof(abs), "%s/%s", dir, rel);
  if (stat(abs, &stt) != 0 || !S_ISREG(stt.st_mode)) return strdup("");
  return strdup(rel);
}

/* ============================================================================
 * Load one page of a chat's messages older than `before` (0 = newest),
 * ascending for display. *out_has_more is set to 1 if at least one older
 * message exists beyond this page. Caller frees each entry with
 * message_row_free.
 * ==========================================================================*/
int enil_db_message_rows_get_page(sqlite3 *db, const char *chat_id,
                                   const char *my_mid, long long before,
                                   int limit, message_row_t **out,
                                   int *out_count, int *out_has_more) {
  sqlite3_stmt *stmt;
  message_row_t *rows = NULL;
  int n = 0, rc, i, j;

  if (out)          *out = NULL;
  if (out_count)    *out_count = 0;
  if (out_has_more) *out_has_more = 0;
  if (!db || !chat_id || limit <= 0 || !out || !out_count) return SQLITE_MISUSE;

  rc = sqlite3_prepare_v2(db, kSQL_GetMessagesPage, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_rows_get_page", "%s", sqlite3_errmsg(db));
    return rc;
  }
  sqlite3_bind_text(stmt, 1, chat_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, before);
  /* Fetch one extra row to detect whether more history exists. */
  sqlite3_bind_int(stmt, 3, limit + 1);

  /* Newest-first; allocate limit+1, keep at most `limit`. */
  rows = (message_row_t *)malloc((size_t)(limit + 1) * sizeof(*rows));
  if (!rows) { sqlite3_finalize(stmt); return SQLITE_NOMEM; }
  while (sqlite3_step(stmt) == SQLITE_ROW && n <= limit)
    fill_message_row(db, stmt, my_mid, &rows[n++]);
  sqlite3_finalize(stmt);

  if (n > limit) {
    /* The extra row proves older history exists; drop it. */
    if (out_has_more) *out_has_more = 1;
    message_row_free(&rows[limit]);
    n = limit;
  }

  /* Reverse newest-first → ascending for display. */
  for (i = 0, j = n - 1; i < j; i++, j--) {
    message_row_t tmp = rows[i];
    rows[i] = rows[j];
    rows[j] = tmp;
  }

  if (out)       *out = rows;
  if (out_count) *out_count = n;
  return SQLITE_OK;
}

/* ============================================================================
 * Fetch a single message row by id and return it as a cJSON object.
 * Caller must cJSON_Delete the result.
 * ==========================================================================*/
/* Column projection for kSQL_GetMessageById (see that SELECT for column order).
 * This is the message-pane row shape consumed by the incremental-append path;
 * it overlaps message_row_fields but is a distinct SELECT/order, so it keeps
 * its own table. */
static const enil_col_t message_by_id_cols[] = {
  { "message_id",     ENIL_F_STR },
  { "sender_name",    ENIL_F_STR },
  { "chat_id",        ENIL_F_STR },
  { "created_at",     ENIL_F_I64 },
  { "text",           ENIL_F_STR },
  { "content_type",   ENIL_F_INT },
  { "media_path",     ENIL_F_STR },
  { "from_mid",       ENIL_F_STR },
  { "orig_width",     ENIL_F_INT },
  { "orig_height",    ENIL_F_INT },
  { "thumb_path",     ENIL_F_STR },
  { "thumb_width",    ENIL_F_INT },
  { "thumb_height",   ENIL_F_INT },
  { "sticker_id",     ENIL_F_STR },
  { "sticker_pkg_id", ENIL_F_STR },
  { "reactions_json", ENIL_F_STR },
  { "is_deleted",     ENIL_F_INT },
  { "decrypt_status", ENIL_F_INT }
};

cJSON *enil_db_get_message_by_id(sqlite3 *db, const char *message_id) {
  sqlite3_stmt *stmt;
  cJSON        *row = NULL;
  int           rc;

  if (!db || !message_id) return NULL;
  rc = sqlite3_prepare_v2(db, kSQL_GetMessageById, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.get_message_by_id", "%s", sqlite3_errmsg(db));
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, message_id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    row = row_to_cjson(stmt, message_by_id_cols,
                       sizeof(message_by_id_cols) / sizeof(message_by_id_cols[0]));
  }
  sqlite3_finalize(stmt);
  return row;
}

/* {message_id, raw_json} — shared by the decrypt and replace backfill passes. */
static const enil_col_t msgid_raw_cols[] = {
  { "message_id", ENIL_F_STR },
  { "raw_json",   ENIL_F_STR }
};

/* Run a {message_id, raw_json} projection (no bind params) and return the rows
 * as a cJSON array. Caller must cJSON_Delete the result. */
static cJSON *msgid_raw_rows(sqlite3 *db, const char *sql, const char *log_tag) {
  sqlite3_stmt *stmt;
  if (!db) return NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    ENIL_LOG(log_tag, "%s", sqlite3_errmsg(db));
    return NULL;
  }
  return rows_to_cjson_array(stmt, msgid_raw_cols, 2);
}

/* ============================================================================
 * Return rows whose ciphertext has not yet been decrypted, for the sync
 * engine's E2EE pass. Caller must cJSON_Delete the result.
 * ==========================================================================*/
cJSON *enil_db_get_messages_needing_decrypt(sqlite3 *db) {
  return msgid_raw_rows(db, kSQL_GetMessagesNeedingDecrypt,
                        "Db.get_messages_needing_decrypt");
}

/* ============================================================================
 * Return decrypted messages whose REPLACE field has not been merged into
 * contentMetadata yet — used to backfill sticon identities for $-markers.
 * ==========================================================================*/
cJSON *enil_db_get_messages_needing_replace(sqlite3 *db) {
  return msgid_raw_rows(db, kSQL_GetMessagesNeedingReplace,
                        "Db.get_messages_needing_replace");
}

/* Column projection for kSQL_GetMessageSticons (one row per $-marker). */
static const enil_col_t message_sticon_cols[] = {
  { "package_id",   ENIL_F_STR },
  { "sticon_id",    ENIL_F_STR },
  { "image_path",   ENIL_F_STR },
  { "image_width",  ENIL_F_INT },
  { "image_height", ENIL_F_INT }
};

/* ============================================================================
 * Return the ordered sticon resources for a message (one row per $-marker).
 * ==========================================================================*/
cJSON *enil_db_get_message_sticons(sqlite3 *db, const char *message_id) {
  sqlite3_stmt *stmt;
  int rc;

  if (!db || !message_id) return NULL;
  rc = sqlite3_prepare_v2(db, kSQL_GetMessageSticons, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.get_message_sticons", "%s", sqlite3_errmsg(db));
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, message_id, -1, SQLITE_TRANSIENT);
  return rows_to_cjson_array(stmt, message_sticon_cols, 5);
}

/* ============================================================================
 * Replace the stored raw_json blob for a message (after decrypt+REPLACE merge).
 * ==========================================================================*/
int enil_db_set_message_raw_json(sqlite3 *db, const char *message_id,
                                  const char *raw_json, const char *replace_json) {
  sqlite3_stmt *stmt;
  int           rc;
  if (!db || !message_id || !raw_json) return SQLITE_ERROR;
  rc = sqlite3_prepare_v2(db, kSQL_SetMessageRawJson, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(stmt, 1, raw_json,     -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, replace_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, message_id,   -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) {
    cJSON *root = cJSON_Parse(raw_json);
    cJSON *meta = root ? cJSON_GetObjectItem(root, "contentMetadata") : NULL;
    const char *own = meta ?
      cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STICON_OWNERSHIP")) : NULL;
    replace_message_sticon_rows(db, message_id, replace_json, own);
    cJSON_Delete(root);
  }
  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

/* ============================================================================
 * Update the displayed text column for a message row.
 * ==========================================================================*/
int enil_db_set_message_text(sqlite3 *db, const char *message_id,
                              const char *text, ENILDecryptStatus status) {
  enil_bind_t b[] = { ENIL_S(text), ENIL_I((int)status), ENIL_S(message_id) };
  if (!message_id) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_SetMessageText, "Db.set_message_text", b, 3);
}

/* ============================================================================
 * Persist a decrypted plaintext blob alongside the original ciphertext.
 * ==========================================================================*/
int enil_db_set_message_plaintext(sqlite3 *db, const char *message_id,
                                   const char *text, const char *raw_json,
                                   const char *replace_json,
                                   ENILDecryptStatus status) {
  sqlite3_stmt *stmt;
  int rc;

  if (!db || !message_id || !raw_json) return SQLITE_ERROR;
  rc = sqlite3_prepare_v2(db, kSQL_SetMessagePlaintext, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.set_message_plaintext", "prepare: %s", sqlite3_errmsg(db));
    return rc;
  }
  sqlite3_bind_text(stmt, 1, text,         -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, raw_json,     -1, SQLITE_TRANSIENT);
  sqlite3_bind_int (stmt, 3, (int)status);
  sqlite3_bind_text(stmt, 4, replace_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, message_id,   -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    ENIL_LOG("Db.set_message_plaintext", "step failed: %s", sqlite3_errmsg(db));
    return rc;
  }
  replace_message_sticon_rows(db, message_id, replace_json, NULL);
  return SQLITE_OK;
}

/* ============================================================================
 * Return messages that reference OBS media but have no local path yet.
 * ==========================================================================*/
/* Build one media entry from a stepped row of the media-columns SELECT. Shared
 * by the full-table sweep and the single-message lookup. */
static cJSON *media_row_from_stmt(sqlite3_stmt *stmt) {
  const char *message_id = (const char *)sqlite3_column_text(stmt, 0);
  const char *chat_id    = (const char *)sqlite3_column_text(stmt, 1);
  const char *media_path = (const char *)sqlite3_column_text(stmt, 2);
  const char *enc_km     = (const char *)sqlite3_column_text(stmt, 3);
  const char *sid        = (const char *)sqlite3_column_text(stmt, 4);
  const char *oid        = (const char *)sqlite3_column_text(stmt, 5);
  const char *obs_pop    = (const char *)sqlite3_column_text(stmt, 6);
  const char *e2ee_ver   = (const char *)sqlite3_column_text(stmt, 7);
  sqlite3_int64 plain_size = sqlite3_column_int64(stmt, 8);
  cJSON *row = cJSON_CreateObject();
  cJSON_AddStringToObject(row, "message_id", message_id ? message_id : "");
  cJSON_AddStringToObject(row, "chat_id",    chat_id    ? chat_id    : "");
  cJSON_AddStringToObject(row, "media_path", media_path ? media_path : "");
  cJSON_AddStringToObject(row, "sid", sid ? sid : "");
  cJSON_AddStringToObject(row, "oid", oid ? oid : "");
  cJSON_AddStringToObject(row, "obs_pop", obs_pop ? obs_pop : "");
  cJSON_AddStringToObject(row, "e2ee_version", e2ee_ver ? e2ee_ver : "");
  cJSON_AddNumberToObject(row, "plain_size", (double)plain_size);
  if (enc_km && enc_km[0])
    cJSON_AddStringToObject(row, "enc_km", enc_km);
  return row;
}

cJSON *enil_db_get_messages_needing_media(sqlite3 *db) {
  sqlite3_stmt *stmt;
  cJSON        *result;
  int           rc;

  if (!db) return NULL;
  rc = sqlite3_prepare_v2(db, kSQL_GetMessagesNeedingMedia, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.get_messages_needing_media", "%s", sqlite3_errmsg(db));
    return NULL;
  }

  result = cJSON_CreateArray();
  while (sqlite3_step(stmt) == SQLITE_ROW)
    cJSON_AddItemToArray(result, media_row_from_stmt(stmt));
  sqlite3_finalize(stmt);
  return result;
}

cJSON *enil_db_get_message_media_info(sqlite3 *db, const char *message_id) {
  sqlite3_stmt *stmt;
  cJSON        *row = NULL;

  if (!db || !message_id) return NULL;
  if (sqlite3_prepare_v2(db, kSQL_GetMessageMediaInfo, -1, &stmt, NULL) != SQLITE_OK) {
    ENIL_LOG("Db.get_message_media_info", "%s", sqlite3_errmsg(db));
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, message_id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW)
    row = media_row_from_stmt(stmt);
  sqlite3_finalize(stmt);
  return row;
}

int enil_db_sticon_package_meta_state(sqlite3 *db, const char *pkg_id) {
  sqlite3_stmt *stmt;
  int purchased = 0, nrows = 0, nnull = 0, state = 0;

  if (!db || !pkg_id || !pkg_id[0]) return 0;
  if (sqlite3_prepare_v2(db, kSQL_SticonPackageMetaState, -1, &stmt, NULL) != SQLITE_OK) {
    ENIL_LOG("Db.sticon_package_meta_state", "%s", sqlite3_errmsg(db));
    return 0;
  }
  sqlite3_bind_text(stmt, 1, pkg_id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    purchased = sqlite3_column_int(stmt, 0);
    nrows     = sqlite3_column_int(stmt, 1);
    nnull     = sqlite3_column_int(stmt, 2);
    if (purchased && nrows == 0) state = 1;       /* owned, unexpanded -> expand */
    else if (!purchased && nnull > 0) state = 2;  /* discovered, missing alt -> enrich */
  }
  sqlite3_finalize(stmt);
  return state;
}

/* ============================================================================
 * Store the encrypted key-material blob used to fetch an E2EE media file.
 * ==========================================================================*/
int enil_db_set_message_enc_km(sqlite3 *db, const char *message_id,
                                const char *enc_km) {
  enil_bind_t b[] = { ENIL_S(enc_km), ENIL_S(message_id) };
  if (!db || !message_id || !enc_km) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_SetMessageEncKm, "Db.set_message_enc_km", b, 2);
}

/* ============================================================================
 * Mark a message as deleted (unsend op 64/65, or a local unsend RPC). The row
 * stays — the renderer is expected to substitute a "Message unsent" placeholder.
 * ==========================================================================*/
/* ============================================================================
 * Apply one SSE op-139/op-140 reaction event to messages_v2.reactions_json.
 *
 * Each reactor has at most one current reaction per message. The stored
 * shape is a JSON array; entries carry either a predefined type (built-in
 * heart/thumbs/etc., 2..7) or a paid type (product/emoji from sticonshop):
 *   [{"reactorMid":"u…","predefinedReactionType":3,"createdTime":<ms>}]
 *   [{"reactorMid":"u…","productId":"…","emojiId":"…","createdTime":<ms>}]
 *
 * Pass is_remove=1 to drop the reactor's entry (param2.curr was absent in
 * the LINE event). Empty arrays are stored as NULL so the column stays
 * cheap to read.
 * ==========================================================================*/
int enil_db_message_reactions_update(sqlite3    *db,
                                     const char *message_id,
                                     const char *reactor_mid,
                                     int         is_remove,
                                     int         predefined_type,
                                     const char *product_id,
                                     const char *emoji_id,
                                     long long   created_time) {
  sqlite3_stmt *stmt;
  const unsigned char *existing;
  cJSON *arr, *entry;
  int i, n, removed = 0;
  char *out_text = NULL;
  int rc;

  if (!db || !message_id || !reactor_mid) return SQLITE_ERROR;

  rc = sqlite3_prepare_v2(db,
    "SELECT reactions_json FROM messages_v2 WHERE id = ?", -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(stmt, 1, message_id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    ENIL_LOG("Db.reactions_update", "message %s not found", message_id);
    return SQLITE_ERROR;
  }
  existing = sqlite3_column_text(stmt, 0);
  arr = (existing && existing[0])
    ? cJSON_Parse((const char *)existing) : NULL;
  sqlite3_finalize(stmt);
  if (!cJSON_IsArray(arr)) {
    if (arr) cJSON_Delete(arr);
    arr = cJSON_CreateArray();
    if (!arr) return SQLITE_NOMEM;
  }

  /* Remove any existing entry for this reactor (one reaction per user). */
  n = cJSON_GetArraySize(arr);
  for (i = n - 1; i >= 0; i--) {
    cJSON *item = cJSON_GetArrayItem(arr, i);
    cJSON *rm   = cJSON_GetObjectItem(item, "reactorMid");
    if (cJSON_IsString(rm) && rm->valuestring &&
        strcmp(rm->valuestring, reactor_mid) == 0) {
      cJSON_DeleteItemFromArray(arr, i);
      removed = 1;
    }
  }
  if (!is_remove) {
    entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "reactorMid", reactor_mid);
    if (predefined_type > 0)
      cJSON_AddNumberToObject(entry, "predefinedReactionType",
                              (double)predefined_type);
    if (product_id && product_id[0])
      cJSON_AddStringToObject(entry, "productId", product_id);
    if (emoji_id && emoji_id[0])
      cJSON_AddStringToObject(entry, "emojiId",  emoji_id);
    if (created_time > 0)
      cJSON_AddNumberToObject(entry, "createdTime", (double)created_time);
    cJSON_AddItemToArray(arr, entry);
  } else if (!removed) {
    /* Remove event for a reactor we didn't have on file — nothing to do. */
    cJSON_Delete(arr);
    return SQLITE_OK;
  }

  if (cJSON_GetArraySize(arr) > 0)
    out_text = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);

  rc = sqlite3_prepare_v2(db,
    "UPDATE messages_v2 SET reactions_json = ? WHERE id = ?",
    -1, &stmt, NULL);
  if (rc != SQLITE_OK) { free(out_text); return rc; }
  bind_text_or_null(stmt, 1, out_text);
  sqlite3_bind_text (stmt, 2, message_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(out_text);
  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int enil_db_message_mark_deleted(sqlite3 *db, const char *message_id) {
  enil_bind_t b[] = { ENIL_S(message_id) };
  if (!message_id) return SQLITE_ERROR;
  return enil_db_exec(db, "UPDATE messages_v2 SET is_deleted = 1 WHERE id = ?",
                      "Db.message_mark_deleted", b, 1);
}

/* ============================================================================
 * Mark a chat as fully seen locally. Drives `unreadCount = 0` and
 * `lastSeenMessageId = message_id` on the message_boxes_v2 row whose id is
 * the chat mid. Used after a successful TalkService.sendChatChecked and on
 * inbound SSE op 40 (SEND_CHAT_CHECKED) — both indicate this account has seen
 * the chat. Op 55 (NOTIFIED_READ_MESSAGE) is the peer-read path and uses
 * enil_db_message_box_set_peer_read below.
 * ==========================================================================*/
int enil_db_message_box_mark_seen(sqlite3 *db, const char *chat_mid,
                                   const char *message_id) {
  enil_bind_t b[] = { ENIL_S(message_id), ENIL_S(chat_mid) };
  if (!chat_mid) return SQLITE_ERROR;
  return enil_db_exec(db,
    "UPDATE message_boxes_v2 SET unreadCount = 0, lastSeenMessageId = ?"
    " WHERE id = ?", "Db.message_box_mark_seen", b, 2);
}

/* ============================================================================
 * Record a peer's read-up-to timestamp (SSE op 55). Stores the latest reader
 * mid and timestamp on the message_boxes_v2 row; only overwrites if the new
 * timestamp is >= the stored one (out-of-order events are ignored).
 * ==========================================================================*/
int enil_db_message_box_set_peer_read(sqlite3 *db, const char *chat_mid,
                                       const char *reader_mid,
                                       long long read_up_to_time) {
  enil_bind_t b[] = {
    ENIL_L(read_up_to_time), ENIL_S(reader_mid),
    ENIL_S(chat_mid), ENIL_L(read_up_to_time)
  };
  if (!chat_mid || !chat_mid[0]) return SQLITE_ERROR;
  return enil_db_exec(db,
    "UPDATE message_boxes_v2"
    "   SET peerLastReadTime  = ?,"
    "       peerLastReaderMid = ?"
    " WHERE id = ?"
    "   AND (peerLastReadTime IS NULL OR peerLastReadTime <= ?)",
    "Db.message_box_set_peer_read", b, 4);
}

/* ============================================================================
 * Return peerLastReadTime (ms) for a chat, or 0 when the row/column is missing.
 * Drives the "Read up to here" marker in the message panel; only meaningful
 * for 1:1 chats — callers should restrict use to mids beginning with 'u'.
 * ==========================================================================*/
long long enil_db_message_box_get_peer_last_read_time(sqlite3 *db,
                                                       const char *chat_mid) {
  return db_scalar_i64(db,
    "SELECT peerLastReadTime FROM message_boxes_v2 WHERE id = ?",
    chat_mid, 0, "Db.message_box_get_peer_last_read_time");
}

/* ============================================================================
 * Return malloc'd id of the last outgoing non-deleted message in `chat_mid`
 * whose createdTime is <= the chat's peerLastReadTime. NULL when no such
 * message exists (peer hasn't read anything yet, all candidate messages were
 * unsent, or the chat box is missing). Drives the setReadMarker() push.
 * ==========================================================================*/
char *enil_db_last_read_outgoing_msg_id(sqlite3 *db, const char *chat_mid,
                                         const char *my_mid) {
  sqlite3_stmt *stmt;
  char *result = NULL;
  int rc;
  if (!db || !chat_mid || !chat_mid[0] || !my_mid || !my_mid[0]) return NULL;
  rc = sqlite3_prepare_v2(db,
    "SELECT m.id FROM messages_v2 m"
    " WHERE m.chat_id = ?"
    "   AND m.\"from\" = ?"
    "   AND m.is_deleted = 0"
    "   AND m.createdTime <= ("
    "     SELECT peerLastReadTime FROM message_boxes_v2 WHERE id = ?)"
    " ORDER BY m.createdTime DESC LIMIT 1",
    -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.last_read_outgoing_msg_id", "prepare: %s",
             sqlite3_errmsg(db));
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, chat_mid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, my_mid,   -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, chat_mid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *v = sqlite3_column_text(stmt, 0);
    if (v && v[0]) result = strdup((const char *)v);
  }
  sqlite3_finalize(stmt);
  return result;
}

/* ============================================================================
 * Per-member group read state. Stores one row per (chatMid, readerMid) and
 * overwrites in place as the reader's high-water-mark advances. The guard
 * (readTime <= ?) ignores out-of-order SSE delivery, mirroring the 1:1
 * `set_peer_read` discipline. Only call this for c/C/r/R-prefixed chats —
 * the 1:1 path stays on message_boxes_v2.peerLastRead*.
 * ==========================================================================*/
int enil_db_message_box_set_group_reader(sqlite3 *db, const char *chat_mid,
                                          const char *reader_mid,
                                          long long read_up_to_time) {
  enil_bind_t b[] = {
    ENIL_S(chat_mid), ENIL_S(reader_mid), ENIL_L(read_up_to_time),
    ENIL_S(chat_mid), ENIL_S(reader_mid), ENIL_L(read_up_to_time)
  };
  if (!chat_mid || !chat_mid[0] || !reader_mid || !reader_mid[0])
    return SQLITE_ERROR;
  return enil_db_exec(db,
    "INSERT OR REPLACE INTO message_box_readers_v2"
    "  (chatMid, readerMid, readTime)"
    " SELECT ?, ?, ?"
    " WHERE NOT EXISTS ("
    "   SELECT 1 FROM message_box_readers_v2"
    "    WHERE chatMid = ? AND readerMid = ? AND readTime > ?)",
    "Db.message_box_set_group_reader", b, 6);
}

void enil_group_reader_free(enil_group_reader_t *r) {
  if (!r) return;
  free(r->reader_mid);          r->reader_mid = NULL;
  free(r->display_name);        r->display_name = NULL;
  free(r->last_read_message_id); r->last_read_message_id = NULL;
}

void enil_group_readers_free(enil_group_reader_t *arr, int count) {
  int i;
  if (!arr) return;
  for (i = 0; i < count; i++) enil_group_reader_free(&arr[i]);
  free(arr);
}

/* ============================================================================
 * For each member who has read at least one outgoing message in `chat_mid`,
 * return their reader_mid, contact display name (falling back to mid), and
 * the id of the last outgoing message at-or-before their readTime. `my_mid`
 * is excluded from the result — a user doesn't need a chip showing they
 * read their own messages. Rows whose lastReadMessageId resolves to NULL
 * (nothing outgoing in range yet) are skipped.
 * ==========================================================================*/
int enil_db_message_box_get_group_readers(sqlite3 *db, const char *chat_mid,
                                           const char *my_mid,
                                           enil_group_reader_t **out,
                                           int *out_count) {
  sqlite3_stmt *stmt;
  enil_group_reader_t *arr = NULL;
  int cap = 0, n = 0, rc;

  if (out) *out = NULL;
  if (out_count) *out_count = 0;
  if (!db || !chat_mid || !chat_mid[0] || !my_mid || !my_mid[0] || !out ||
      !out_count) return SQLITE_ERROR;

  rc = sqlite3_prepare_v2(db,
    "SELECT"
    "  r.readerMid,"
    "  COALESCE(c.displayNameOverridden, c.displayName, r.readerMid),"
    "  (SELECT m.id FROM messages_v2 m"
    "    WHERE m.chat_id = r.chatMid"
    "      AND m.\"from\" = ?"
    "      AND m.is_deleted = 0"
    "      AND m.createdTime <= r.readTime"
    "    ORDER BY m.createdTime DESC LIMIT 1)"
    " FROM message_box_readers_v2 r"
    " LEFT JOIN contacts_v2 c ON c.mid = r.readerMid"
    " WHERE r.chatMid = ? AND r.readerMid != ?",
    -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_box_get_group_readers", "prepare: %s",
             sqlite3_errmsg(db));
    return rc;
  }
  sqlite3_bind_text(stmt, 1, my_mid,   -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, chat_mid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, my_mid,   -1, SQLITE_TRANSIENT);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *mid  = sqlite3_column_text(stmt, 0);
    const unsigned char *name = sqlite3_column_text(stmt, 1);
    const unsigned char *mid2 = sqlite3_column_text(stmt, 2);
    if (!mid2 || !mid2[0]) continue; /* nothing outgoing in range yet */
    if (n == cap) {
      int ncap = cap ? cap * 2 : 4;
      enil_group_reader_t *t = (enil_group_reader_t *)realloc(arr,
                                  (size_t)ncap * sizeof(*arr));
      if (!t) { rc = SQLITE_NOMEM; break; }
      arr = t; cap = ncap;
    }
    arr[n].reader_mid           = mid  ? strdup((const char *)mid)  : NULL;
    arr[n].display_name         = name ? strdup((const char *)name) : NULL;
    arr[n].last_read_message_id = strdup((const char *)mid2);
    n++;
  }
  sqlite3_finalize(stmt);

  if (rc != SQLITE_OK && rc != SQLITE_DONE) {
    enil_group_readers_free(arr, n);
    return rc;
  }
  *out = arr;
  *out_count = n;
  return SQLITE_OK;
}

/* ============================================================================
 * Single-reader fast path for the SSE op-55 push (groups). Returns SQLITE_OK
 * and fills *out iff a matching outgoing message exists at or before the
 * reader's readTime; otherwise returns SQLITE_OK with *out zeroed (caller
 * should check out->last_read_message_id == NULL to mean "no chip to emit").
 * Returns a non-zero sqlite error code on prepare/bind failures.
 * ==========================================================================*/
int enil_db_message_box_get_group_reader(sqlite3 *db, const char *chat_mid,
                                          const char *reader_mid,
                                          const char *my_mid,
                                          enil_group_reader_t *out) {
  sqlite3_stmt *stmt;
  int rc;
  if (out) memset(out, 0, sizeof(*out));
  if (!db || !chat_mid || !chat_mid[0] || !reader_mid || !reader_mid[0] ||
      !my_mid || !my_mid[0] || !out) return SQLITE_ERROR;

  rc = sqlite3_prepare_v2(db,
    "SELECT"
    "  COALESCE(c.displayNameOverridden, c.displayName, r.readerMid),"
    "  (SELECT m.id FROM messages_v2 m"
    "    WHERE m.chat_id = r.chatMid"
    "      AND m.\"from\" = ?"
    "      AND m.is_deleted = 0"
    "      AND m.createdTime <= r.readTime"
    "    ORDER BY m.createdTime DESC LIMIT 1)"
    " FROM message_box_readers_v2 r"
    " LEFT JOIN contacts_v2 c ON c.mid = r.readerMid"
    " WHERE r.chatMid = ? AND r.readerMid = ?",
    -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.message_box_get_group_reader", "prepare: %s",
             sqlite3_errmsg(db));
    return rc;
  }
  sqlite3_bind_text(stmt, 1, my_mid,     -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, chat_mid,   -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, reader_mid, -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(stmt, 0);
    const unsigned char *mid2 = sqlite3_column_text(stmt, 1);
    if (mid2 && mid2[0]) {
      out->reader_mid           = strdup(reader_mid);
      out->display_name         = name ? strdup((const char *)name) : NULL;
      out->last_read_message_id = strdup((const char *)mid2);
    }
  }
  sqlite3_finalize(stmt);
  return SQLITE_OK;
}

/* ============================================================================
 * Return malloc'd lastDeliveredMessageId for `chat_mid`, or NULL if the box
 * has no row, no last-delivered id, or sqlite fails. This is the server's
 * authoritative high-water-mark for incoming messages and is what we hand to
 * TalkService.sendChatChecked.
 * ==========================================================================*/
char *enil_db_message_box_last_delivered(sqlite3 *db, const char *chat_mid) {
  return db_scalar_text(db,
    "SELECT lastDeliveredMessageId FROM message_boxes_v2 WHERE id = ?",
    chat_mid, "Db.message_box_last_delivered");
}

/* ============================================================================
 * Return malloc'd lastSeenMessageId for `chat_mid`, or NULL if the box is
 * missing or has never been seen. This is the account's OWN seen position
 * (written by getMessageBoxes on sync and by enil_db_message_box_mark_seen on
 * the optimistic Mark-as-Seen path), not the peer's — it drives the gray
 * "Seen" marker, the self-position sibling of the green peer "Read" marker.
 * Single primary-key seek on message_boxes_v2.id; caller free()s the result.
 * ==========================================================================*/
char *enil_db_message_box_last_seen(sqlite3 *db, const char *chat_mid) {
  return db_scalar_text(db,
    "SELECT lastSeenMessageId FROM message_boxes_v2 WHERE id = ?",
    chat_mid, "Db.message_box_last_seen");
}

/* ============================================================================
 * Server-authoritative unread count for `chat_mid` (0 if the box is missing or
 * NULL). The single "caught up?" signal: 0 means the account has seen the
 * newest message. Drives both the chat-list blue dot and the gray "Seen"
 * marker so the two never disagree. Single primary-key seek.
 * ==========================================================================*/
int enil_db_message_box_unread_count(sqlite3 *db, const char *chat_mid) {
  long long n = db_scalar_i64(db,
    "SELECT unreadCount FROM message_boxes_v2 WHERE id = ?",
    chat_mid, 0, "Db.message_box_unread_count");
  return n < 0 ? 0 : (int)n;
}

/* ============================================================================
 * Delete every local trace of a chat after the server confirmed sendChatRemoved.
 * Removes rows from chats_v2, message_boxes_v2, messages_v2, message_sticons,
 * and message_sticon_packages for any message in that chat. Wrapped in a single
 * transaction so a partial failure leaves the DB untouched.
 * ==========================================================================*/
int enil_db_chat_delete(sqlite3 *db, const char *chat_mid) {
  sqlite3_stmt *stmt;
  int rc;
  const char *stmts[] = {
    "DELETE FROM message_sticons WHERE message_id IN"
      " (SELECT id FROM messages_v2 WHERE chat_id = ?)",
    "DELETE FROM message_sticon_packages WHERE message_id IN"
      " (SELECT id FROM messages_v2 WHERE chat_id = ?)",
    "DELETE FROM messages_v2     WHERE chat_id = ?",
    "DELETE FROM message_boxes_v2 WHERE id = ?",
    "DELETE FROM chats_v2        WHERE chatMid = ?",
    NULL
  };
  int i;
  if (!db || !chat_mid) return SQLITE_ERROR;
  rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  if (rc != SQLITE_OK) return rc;
  for (i = 0; stmts[i]; i++) {
    rc = sqlite3_prepare_v2(db, stmts[i], -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
      ENIL_LOG("Db.chat_delete", "prepare %d: %s", i, sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return rc;
    }
    sqlite3_bind_text(stmt, 1, chat_mid, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      ENIL_LOG("Db.chat_delete", "step %d: %s", i, sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return rc;
    }
  }
  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  return rc;
}

/* ============================================================================
 * Record the on-disk media + thumbnail paths and dimensions for a message.
 * ==========================================================================*/
int enil_db_set_media_info(sqlite3 *db, const char *message_id,
                            const char *media_path,
                            int orig_width, int orig_height,
                            const char *thumb_path,
                            int thumb_width, int thumb_height) {
  enil_bind_t b[] = {
    ENIL_S(media_path), ENIL_I(orig_width),  ENIL_I(orig_height),
    ENIL_S(thumb_path), ENIL_I(thumb_width), ENIL_I(thumb_height),
    ENIL_S(message_id)
  };
  if (!message_id) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_SetMediaInfo, "Db.set_media_info", b, 7);
}

/* ----- sticker_packages_v2 (typed) ----- */

/* ============================================================================
 * Upsert a row in sticker_packages_v2.
 * ==========================================================================*/
int enil_db_sticker_package_upsert(sqlite3 *db, const sticker_package_t *p) {
  if (!db || !p || !p->id) { LOG("sticker_package_upsert", "missing id"); return SQLITE_ERROR; }
  return upsert_fields_with_raw(db, kSQL_UpsertStickerPackageV2,
                                "Db.sticker_package_upsert", p, p->raw,
                                sticker_package_fields,
                                sticker_package_fields_count, NULL);
}

/* ============================================================================
 * Insert a placeholder sticker_packages_v2 row for a package id referenced
 * by an incoming message before the metadata has been fetched.
 * ==========================================================================*/
int enil_db_sticker_package_discover(sqlite3 *db, const char *id) {
  enil_bind_t b[] = { ENIL_S(id) };
  if (!id || !id[0]) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_DiscoverStickerPackageV2,
                      "Db.sticker_package_discover", b, 1);
}

/* ============================================================================
 * Release malloc'd fields and raw cJSON inside a sticker_package_t.
 * ==========================================================================*/
void sticker_package_free(sticker_package_t *p) {
  if (!p) return;
  enil_field_free_all(p, sticker_package_fields, sticker_package_fields_count);
  if (p->raw) { cJSON_Delete(p->raw); p->raw = NULL; }
}

const enil_field_t sticker_package_fields[] = {
  { "id",                       offsetof(sticker_package_t, id),                       ENIL_F_STR  },
  { "name",                     offsetof(sticker_package_t, name),                     ENIL_F_STR  },
  { "latestVersion",            offsetof(sticker_package_t, latestVersion),            ENIL_F_STR  },
  { "grantedByDefault",         offsetof(sticker_package_t, grantedByDefault),         ENIL_F_BOOL },
  { "authorId",                 offsetof(sticker_package_t, authorId),                 ENIL_F_STR  },
  { "installedTime",            offsetof(sticker_package_t, installedTime),            ENIL_F_I64  },
  { "validUntil",               offsetof(sticker_package_t, validUntil),               ENIL_F_STR  },
  { "validFor",                 offsetof(sticker_package_t, validFor),                 ENIL_F_INT  },
  { "availability",             offsetof(sticker_package_t, availability),             ENIL_F_INT  },
  { "canAutoDownload",          offsetof(sticker_package_t, canAutoDownload),          ENIL_F_BOOL },
  { "promotionType",            offsetof(sticker_package_t, promotionType),            ENIL_F_INT  },
  { "stickerResourceType",      offsetof(sticker_package_t, stickerResourceType),      ENIL_F_INT  },
  { "stickerSize",              offsetof(sticker_package_t, stickerSize),              ENIL_F_INT  },
  { "suggestVersion",           offsetof(sticker_package_t, suggestVersion),           ENIL_F_STR  },
  { "defaultDisplayOnKeyboard", offsetof(sticker_package_t, defaultDisplayOnKeyboard), ENIL_F_BOOL },
  { "availableForPhotoEdit",    offsetof(sticker_package_t, availableForPhotoEdit),    ENIL_F_BOOL },
  { "stickerIdRanges",          offsetof(sticker_package_t, stickerIdRanges),          ENIL_F_JSON },
  { "isPurchased",              offsetof(sticker_package_t, isPurchased),              ENIL_F_BOOL },
};
const size_t sticker_package_fields_count =
  sizeof(sticker_package_fields) / sizeof(sticker_package_fields[0]);

/* ----- sticon_packages_v2 (typed) ----- */

/* ============================================================================
 * Upsert a row in sticon_packages_v2.
 * ==========================================================================*/
int enil_db_sticon_package_upsert(sqlite3 *db, const sticon_package_t *p) {
  if (!db || !p || !p->id) { LOG("sticon_package_upsert", "missing id"); return SQLITE_ERROR; }
  return upsert_fields_with_raw(db, kSQL_UpsertSticonPackageV2,
                                "Db.sticon_package_upsert", p, p->raw,
                                sticon_package_fields,
                                sticon_package_fields_count, NULL);
}

/* ============================================================================
 * Insert a placeholder sticon_packages_v2 row for a package id referenced
 * by an incoming message before the metadata has been fetched.
 * ==========================================================================*/
int enil_db_sticon_package_discover(sqlite3 *db, const char *id) {
  enil_bind_t b[] = { ENIL_S(id) };
  if (!id || !id[0]) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_DiscoverSticonPackageV2,
                      "Db.sticon_package_discover", b, 1);
}

/* ============================================================================
 * Release malloc'd fields and raw cJSON inside a sticon_package_t.
 * ==========================================================================*/
void sticon_package_free(sticon_package_t *p) {
  if (!p) return;
  enil_field_free_all(p, sticon_package_fields, sticon_package_fields_count);
  if (p->raw) { cJSON_Delete(p->raw); p->raw = NULL; }
}

const enil_field_t sticon_package_fields[] = {
  { "id",                      offsetof(sticon_package_t, id),                      ENIL_F_STR  },
  { "name",                    offsetof(sticon_package_t, name),                    ENIL_F_STR  },
  { "latestVersion",           offsetof(sticon_package_t, latestVersion),           ENIL_F_STR  },
  { "grantedByDefault",        offsetof(sticon_package_t, grantedByDefault),        ENIL_F_BOOL },
  { "authorId",                offsetof(sticon_package_t, authorId),                ENIL_F_STR  },
  { "installedTime",           offsetof(sticon_package_t, installedTime),           ENIL_F_I64  },
  { "validUntil",              offsetof(sticon_package_t, validUntil),              ENIL_F_STR  },
  { "validFor",                offsetof(sticon_package_t, validFor),                ENIL_F_INT  },
  { "availability",            offsetof(sticon_package_t, availability),            ENIL_F_INT  },
  { "canAutoDownload",         offsetof(sticon_package_t, canAutoDownload),         ENIL_F_BOOL },
  { "promotionType",           offsetof(sticon_package_t, promotionType),           ENIL_F_INT  },
  { "sticonResourceType",      offsetof(sticon_package_t, sticonResourceType),      ENIL_F_INT  },
  { "suggestVersion",          offsetof(sticon_package_t, suggestVersion),          ENIL_F_STR  },
  { "availableForPhotoEdit",   offsetof(sticon_package_t, availableForPhotoEdit),   ENIL_F_BOOL },
  { "applicationVersionRange", offsetof(sticon_package_t, applicationVersionRange), ENIL_F_JSON },
  { "isPurchased",             offsetof(sticon_package_t, isPurchased),             ENIL_F_BOOL },
};
const size_t sticon_package_fields_count =
  sizeof(sticon_package_fields) / sizeof(sticon_package_fields[0]);

/* ----- stickers_v2 / sticons_v2 (locally-derived per-item rows) ----- */

/* ============================================================================
 * Upsert a single sticker row in stickers_v2.
 * ==========================================================================*/
int enil_db_sticker_upsert(sqlite3 *db, const char *sticker_id,
                            const char *package_id,
                            const char *option, const char *hash) {
  enil_bind_t b[] = {
    ENIL_S(sticker_id), ENIL_S(package_id), ENIL_S(option), ENIL_S(hash)
  };
  if (!sticker_id || !package_id) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_UpsertStickerV2, "Db.sticker_upsert", b, 4);
}

/* ============================================================================
 * Upsert a single sticon row in sticons_v2.
 * ==========================================================================*/
int enil_db_sticon_upsert(sqlite3 *db, const char *sticon_id,
                           const char *package_id, const char *alt_text) {
  enil_bind_t b[] = { ENIL_S(sticon_id), ENIL_S(package_id), ENIL_S(alt_text) };
  if (!sticon_id || !package_id) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_UpsertSticonV2, "Db.sticon_upsert", b, 3);
}

static cJSON *pkg_id_list(sqlite3 *db, const char *sql, const char *log_tag) {
  sqlite3_stmt *stmt;
  cJSON        *result;
  if (!db) return NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    char tag[64];
    snprintf(tag, sizeof(tag), "Db.%s", log_tag ? log_tag : "?");
    ENIL_LOG(tag, "%s", sqlite3_errmsg(db));
    return NULL;
  }
  result = cJSON_CreateArray();
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    if (id && id[0]) cJSON_AddItemToArray(result, cJSON_CreateString(id));
  }
  sqlite3_finalize(stmt);
  return result;
}

/* ============================================================================
 * Return sticon package ids whose ShopService metadata has not been fetched.
 * ==========================================================================*/
cJSON *enil_db_sticon_packages_needing_meta(sqlite3 *db) {
  return pkg_id_list(db, kSQL_GetSticonPackagesNeedingMetaV2,
                     "sticon_packages_needing_meta");
}

/* ============================================================================
 * Return sticon package ids whose per-sticon alt-text labels are unresolved.
 * ==========================================================================*/
cJSON *enil_db_sticon_packages_needing_alt(sqlite3 *db) {
  return pkg_id_list(db, kSQL_GetSticonPackagesNeedingAlt,
                     "sticon_packages_needing_alt");
}

/* ============================================================================
 * Update the resource_type column for a sticon package after metadata fetch.
 * ==========================================================================*/
int enil_db_sticon_package_set_resource_type(sqlite3 *db, const char *id,
                                              int resource_type) {
  enil_bind_t b[] = { ENIL_I(resource_type), ENIL_S(id) };
  if (!id || !id[0]) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_SetSticonPackageResourceType,
                      "Db.sticon_package_set_resource_type", b, 2);
}

/* ============================================================================
 * Record the human-readable alt_text label for a sticon row.
 * ==========================================================================*/
int enil_db_set_sticon_alt_text(sqlite3 *db, const char *sticon_id,
                                 const char *package_id, const char *alt_text) {
  enil_bind_t b[] = { ENIL_S(alt_text), ENIL_S(sticon_id), ENIL_S(package_id) };
  if (!sticon_id || !package_id || !alt_text || !alt_text[0]) return SQLITE_ERROR;
  return enil_db_exec(db, kSQL_SetSticonAltText, "Db.set_sticon_alt_text", b, 3);
}

/* Column projection for the sticker/sticon "needs download" SELECTs. */
static const enil_col_t download_item_cols[] = {
  { "sticker_id",           ENIL_F_STR },
  { "package_id",           ENIL_F_STR },
  { "shop",                 ENIL_F_STR },
  { "option",               ENIL_F_STR },
  { "hash",                 ENIL_F_STR },
  { "sticon_resource_type", ENIL_F_STR }
};

static cJSON *items_needing_download(sqlite3 *db, const char *sql, int purchased) {
  sqlite3_stmt *stmt;
  if (!db) return NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    ENIL_LOG("Db.items_needing_download", "%s", sqlite3_errmsg(db));
    return NULL;
  }
  sqlite3_bind_int(stmt, 1, purchased);
  return rows_to_cjson_array(stmt, download_item_cols, 6);
}

/* ============================================================================
 * Return sticker rows whose image still needs to be downloaded from the CDN.
 * `purchased` selects only stickers in purchased packages when non-zero.
 * ==========================================================================*/
cJSON *enil_db_get_stickers_needing_download(sqlite3 *db, int purchased) {
  return items_needing_download(db, kSQL_GetStickersNeedingDownload, purchased);
}

/* ============================================================================
 * Return sticon rows whose image still needs to be downloaded from the CDN.
 * ==========================================================================*/
cJSON *enil_db_get_sticons_needing_download(sqlite3 *db, int purchased) {
  return items_needing_download(db, kSQL_GetSticonsNeedingDownload, purchased);
}

/* ============================================================================
 * Release malloc'd fields inside a sticker_row_t projection.
 * ==========================================================================*/
void sticker_row_free(sticker_row_t *s) {
  if (!s) return;
  enil_field_free_all(s, sticker_row_fields, sticker_row_fields_count);
}

const enil_field_t sticker_row_fields[] = {
  { "sticker_id",   offsetof(sticker_row_t, sticker_id),   ENIL_F_STR },
  { "package_id",   offsetof(sticker_row_t, package_id),   ENIL_F_STR },
  { "package_name", offsetof(sticker_row_t, package_name), ENIL_F_STR },
  { "shop",         offsetof(sticker_row_t, shop),         ENIL_F_STR },
  { "image_path",   offsetof(sticker_row_t, image_path),   ENIL_F_STR },
  { "thumb_path",   offsetof(sticker_row_t, thumb_path),   ENIL_F_STR },
  { "alt_text",     offsetof(sticker_row_t, alt_text),     ENIL_F_STR },
  { "image_width",  offsetof(sticker_row_t, image_width),  ENIL_F_INT },
  { "image_height", offsetof(sticker_row_t, image_height), ENIL_F_INT },
  { "thumb_width",  offsetof(sticker_row_t, thumb_width),  ENIL_F_INT },
  { "thumb_height", offsetof(sticker_row_t, thumb_height), ENIL_F_INT }
};
const size_t sticker_row_fields_count =
  sizeof(sticker_row_fields) / sizeof(sticker_row_fields[0]);

/* ============================================================================
 * Project every purchased sticker for the picker UI. Caller frees each
 * entry with sticker_row_free.
 * ==========================================================================*/
int enil_db_sticker_rows_get(sqlite3 *db, sticker_row_t **out, int *out_count) {
  sqlite3_stmt *stmt;
  int rc;

  if (out) *out = NULL;
  if (out_count) *out_count = 0;
  if (!db || !out || !out_count) return SQLITE_MISUSE;

  rc = sqlite3_prepare_v2(db, kSQL_GetPurchasedStickers, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.sticker_rows_get", "%s", sqlite3_errmsg(db));
    return rc;
  }
  return enil_db_collect(stmt, sizeof(sticker_row_t),
                         sticker_row_fields, sticker_row_fields_count, 0,
                         (void **)out, out_count);
}

/* ============================================================================
 * Release malloc'd fields inside a sticon_row_t projection.
 * ==========================================================================*/
void sticon_row_free(sticon_row_t *s) {
  if (!s) return;
  enil_field_free_all(s, sticon_row_fields, sticon_row_fields_count);
}

const enil_field_t sticon_row_fields[] = {
  { "sticker_id",   offsetof(sticon_row_t, sticker_id),   ENIL_F_STR },
  { "package_id",   offsetof(sticon_row_t, package_id),   ENIL_F_STR },
  { "package_name", offsetof(sticon_row_t, package_name), ENIL_F_STR },
  { "image_path",   offsetof(sticon_row_t, image_path),   ENIL_F_STR },
  { "alt_text",     offsetof(sticon_row_t, alt_text),     ENIL_F_STR },
  { "image_width",  offsetof(sticon_row_t, image_width),  ENIL_F_INT },
  { "image_height", offsetof(sticon_row_t, image_height), ENIL_F_INT }
};
const size_t sticon_row_fields_count =
  sizeof(sticon_row_fields) / sizeof(sticon_row_fields[0]);

/* ============================================================================
 * Project every purchased sticon for the picker UI. Caller frees each
 * entry with sticon_row_free.
 * ==========================================================================*/
int enil_db_sticon_rows_get(sqlite3 *db, sticon_row_t **out, int *out_count) {
  sqlite3_stmt *stmt;
  int rc;

  if (out) *out = NULL;
  if (out_count) *out_count = 0;
  if (!db || !out || !out_count) return SQLITE_MISUSE;

  rc = sqlite3_prepare_v2(db, kSQL_GetPurchasedSticons, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.sticon_rows_get", "%s", sqlite3_errmsg(db));
    return rc;
  }
  return enil_db_collect(stmt, sizeof(sticon_row_t),
                         sticon_row_fields, sticon_row_fields_count, 0,
                         (void **)out, out_count);
}

/* ============================================================================
 * Record the local image path and dimensions for a downloaded sticker.
 * ==========================================================================*/
int enil_db_set_sticker_image_path(sqlite3 *db, const char *sticker_id,
                                    const char *package_id,
                                    const char *image_path,
                                    int image_width, int image_height) {
  return set_path_dims(db, kSQL_SetStickerImagePath, "Db.set_sticker_image_path",
                       image_path, image_width, image_height,
                       sticker_id, package_id);
}

/* ============================================================================
 * Record the local thumbnail path and dimensions for a sticker.
 * ==========================================================================*/
int enil_db_set_sticker_thumb_path(sqlite3 *db, const char *sticker_id,
                                    const char *package_id,
                                    const char *thumb_path,
                                    int thumb_width, int thumb_height) {
  return set_path_dims(db, kSQL_SetStickerThumbPath, "Db.set_sticker_thumb_path",
                       thumb_path, thumb_width, thumb_height,
                       sticker_id, package_id);
}

/* ============================================================================
 * Record the local image path and dimensions for a downloaded sticon.
 * ==========================================================================*/
int enil_db_set_sticon_image_path(sqlite3 *db, const char *sticon_id,
                                   const char *package_id,
                                   const char *image_path,
                                   int image_width, int image_height) {
  return set_path_dims(db, kSQL_SetSticonImagePath, "Db.set_sticon_image_path",
                       image_path, image_width, image_height,
                       sticon_id, package_id);
}

/* ============================================================================
 * Record the local thumbnail path and dimensions for a sticon.
 * ==========================================================================*/
int enil_db_set_sticon_thumb_path(sqlite3 *db, const char *sticon_id,
                                   const char *package_id,
                                   const char *thumb_path,
                                   int thumb_width, int thumb_height) {
  return set_path_dims(db, kSQL_SetSticonThumbPath, "Db.set_sticon_thumb_path",
                       thumb_path, thumb_width, thumb_height,
                       sticon_id, package_id);
}


/* ============================================================================
 * Resolve a sticker_id (any package) to its on-disk path and dimensions.
 * Returns 1 on hit, 0 on miss.
 * ==========================================================================*/
/* Shared body for the two sticker-image-info readers. Prepare `sql`, bind
 * `nbind` text params from `binds[]` (the only thing that differs between the
 * any-package and per-package variants), then read (path,width,height) from
 * column 0..2 of the first row. Returns 1 on a hit with a non-NULL path (buf
 * filled, dims set), 0 otherwise. */
static int sticker_image_info(sqlite3 *db, const char *sql,
                              const char *const *binds, int nbind,
                              char *buf, size_t buf_size,
                              int *image_width, int *image_height) {
  sqlite3_stmt *stmt;
  int i, found = 0;
  if (!db || !buf || buf_size == 0 || !image_width || !image_height) return 0;
  *image_width = 0;
  *image_height = 0;
  buf[0] = '\0';
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
  for (i = 0; i < nbind; i++)
    sqlite3_bind_text(stmt, i + 1, binds[i], -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *path = (const char *)sqlite3_column_text(stmt, 0);
    *image_width  = sqlite3_column_int(stmt, 1);
    *image_height = sqlite3_column_int(stmt, 2);
    if (path) {
      strncpy(buf, path, buf_size - 1);
      buf[buf_size - 1] = '\0';
      found = 1;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

int enil_db_get_sticker_image_info(sqlite3 *db, const char *sticker_id,
                                    char *buf, size_t buf_size,
                                    int *image_width, int *image_height) {
  /* UNION ALL — same id may be either a sticker or a sticon. */
  const char *binds[2];
  if (!sticker_id) return 0;
  binds[0] = sticker_id;
  binds[1] = sticker_id;
  return sticker_image_info(db, kSQL_GetStickerImageInfo, binds, 2,
                            buf, buf_size, image_width, image_height);
}

/* ============================================================================
 * Resolve a (package_id, sticker_id) pair to its image path and dimensions.
 * ==========================================================================*/
int enil_db_get_sticker_image_info_for_package(sqlite3 *db,
                                                const char *package_id,
                                                const char *sticker_id,
                                                char *buf, size_t buf_size,
                                                int *image_width,
                                                int *image_height) {
  /* UNION ALL — try stickers_v2 then sticons_v2. */
  const char *binds[4];
  if (!package_id || !sticker_id) return 0;
  binds[0] = package_id;
  binds[1] = sticker_id;
  binds[2] = package_id;
  binds[3] = sticker_id;
  return sticker_image_info(db, kSQL_GetStickerImageInfoForPackage, binds, 4,
                            buf, buf_size, image_width, image_height);
}

/* ============================================================================
 * Single send-time metadata accessor for both shops: resolve an
 * (package_id, item_id) to the version / option / hash / resourceType needed
 * to build an outgoing sticker (contentType=7) or inline-sticon REPLACE block.
 * Stickershop rows live in stickers_v2 (option/hash present, stickerResourceType);
 * sticonshop rows live in sticons_v2 (no option/hash, sticonResourceType).
 * UNION ALL picks whichever exists. Returns 1 on hit, 0 on miss.
 * ==========================================================================*/
int enil_db_get_sticker_meta(sqlite3 *db, const char *package_id,
                              const char *sticker_id, ENILStickerMeta *out) {
  static const char *sql =
    "SELECT sp.latestVersion, s.option, s.hash, sp.stickerResourceType"
    " FROM stickers_v2 s"
    " JOIN sticker_packages_v2 sp ON sp.id = s.package_id"
    " WHERE s.sticker_id = ? AND s.package_id = ?"
    " UNION ALL"
    " SELECT cp.latestVersion, NULL, NULL, cp.sticonResourceType"
    " FROM sticons_v2 c"
    " JOIN sticon_packages_v2 cp ON cp.id = c.package_id"
    " WHERE c.sticon_id = ? AND c.package_id = ?"
    " LIMIT 1";
  sqlite3_stmt *stmt;
  const unsigned char *v;
  int hit = 0;
  if (!db || !package_id || !sticker_id || !out) return 0;
  memset(out, 0, sizeof(*out));
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(stmt, 1, sticker_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, package_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, sticker_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, package_id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    v = sqlite3_column_text(stmt, 0); if (v) out->version = strdup((const char *)v);
    v = sqlite3_column_text(stmt, 1); if (v) out->option  = strdup((const char *)v);
    v = sqlite3_column_text(stmt, 2); if (v) out->hash    = strdup((const char *)v);
    out->resource_type = sqlite3_column_int(stmt, 3);
    hit = 1;
  }
  sqlite3_finalize(stmt);
  return hit;
}

/* ============================================================================
 * Release malloc'd strings inside an ENILStickerMeta.
 * ==========================================================================*/
void enil_db_sticker_meta_free(ENILStickerMeta *m) {
  if (!m) return;
  free(m->version); free(m->option); free(m->hash);
  m->version = m->option = m->hash = NULL;
}

/* ============================================================================
 * Record a talkException SSE event for later inspection. received_at is wall
 * clock unix seconds. raw_json is the full SSE data payload as a string.
 * No retention policy yet; rows accumulate until a future UI/cleanup pass.
 * ==========================================================================*/
int enil_db_talk_exception_record(sqlite3    *db,
                                  long long   received_at,
                                  int         code,
                                  const char *reason,
                                  const char *raw_json) {
  sqlite3_stmt *stmt;
  int rc;
  if (!db) { LOG("talk_exception_record", "NULL db"); return SQLITE_ERROR; }
  rc = sqlite3_prepare_v2(db, kSQL_InsertTalkException, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.talk_exception_record", "prepare: %s", sqlite3_errmsg(db));
    return rc;
  }
  sqlite3_bind_int64(stmt, 1, received_at);
  sqlite3_bind_int  (stmt, 2, code);
  bind_text_or_null (stmt, 3, reason);
  bind_text_or_null (stmt, 4, raw_json);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    ENIL_LOG("Db.talk_exception_record", "%s", sqlite3_errmsg(db));
    return rc;
  }
  ENIL_LOG("Db.talk_exception_record", "code=%d reason=%s",
           code, reason ? reason : "(null)");
  return SQLITE_OK;
}

int enil_db_sse_event_record(sqlite3    *db,
                             long long   received_at,
                             const char *event_type,
                             int         op_type,
                             const char *op_name,
                             long long   revision,
                             const char *chat_id,
                             int         handled,
                             const char *raw_json) {
  sqlite3_stmt *stmt;
  int rc;
  if (!db) { LOG("sse_event_record", "NULL db"); return SQLITE_ERROR; }
  rc = sqlite3_prepare_v2(db, kSQL_InsertSSEEvent, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    ENIL_LOG("Db.sse_event_record", "prepare: %s", sqlite3_errmsg(db));
    return rc;
  }
  sqlite3_bind_int64(stmt, 1, received_at);
  bind_text_or_null (stmt, 2, event_type);
  if (op_type >= 0) sqlite3_bind_int (stmt, 3, op_type);
  else              sqlite3_bind_null(stmt, 3);
  bind_text_or_null (stmt, 4, op_name);
  if (revision > 0) sqlite3_bind_int64(stmt, 5, revision);
  else              sqlite3_bind_null (stmt, 5);
  bind_text_or_null (stmt, 6, chat_id);
  sqlite3_bind_text (stmt, 7, handled ? "YES" : "NO", -1, SQLITE_STATIC);
  bind_text_or_null (stmt, 8, raw_json);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    ENIL_LOG("Db.sse_event_record", "%s", sqlite3_errmsg(db));
    return rc;
  }
  ENIL_LOG("Db.sse_event_record", "type=%s op=%s rev=%lld handled=%d",
           event_type ? event_type : "(null)",
           op_name ? op_name : "-", revision, handled);
  return SQLITE_OK;
}
