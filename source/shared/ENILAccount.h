#import <Foundation/Foundation.h>
#include "enil_db.h"
#include "enil_sse.h"

extern NSString * const ENILErrorDomain;
extern NSString * const ENILSyncStatusNotification;      /* userInfo: @{step, total, unitCount, unitTotal, message, error} */
extern NSString * const ENILSyncDidFinishNotification;   /* userInfo: @{@"ok": NSNumber(BOOL)} */
extern NSString * const ENILMessageJSNotification;       /* userInfo: @{@"chat_id": NSString, @"js": NSString} */
extern NSString * const ENILSSEEventNotification;        /* userInfo: @{@"type": NSString, @"data": NSString, @"chat_id": NSString (optional, present when the event moved one chat's order/unread)} */
extern NSString * const ENILSyncStateChangedNotification;/* userInfo: @{@"state": NSNumber(ENILSyncState)} */

typedef enum {
  ENILErrorDirectoryCreate = 1,
  ENILErrorMissingSession  = 2,
  ENILErrorInvalidSession  = 3,
} ENILErrorCode;

/* Observer for the QR-login class method below. All callbacks are delivered
 * on the MAIN thread (ENILAccount marshals them off the background login
 * thread), so implementations are straight-line UI code. */
@protocol ENILQRLoginObserver <NSObject>
/* The QR URL to render and scan with the LINE phone app. */
- (void)qrLoginDidEmitURL:(NSString *)url;
/* The 6-digit PIN to confirm on the phone — only on the certificate-reject
 * path (always on a first-ever login). */
- (void)qrLoginDidEmitPIN:(NSString *)pin;
@optional
/* Raw C status key (e.g. "waiting for scan (N/M)"); localize at the UI. */
- (void)qrLoginDidEmitStatus:(NSString *)status;
@end

typedef enum {
  ENILSyncStateNeverSynced = 0,
  ENILSyncStateSyncing     = 1,
  ENILSyncStateSyncNeeded  = 2,
  ENILSyncStateLive        = 3,
  ENILSyncStateError       = 4,
  ENILSyncStateReauthNeeded = 5, /* token died (talkException 117); user must
                                    rescan a QR. Persisted via session.json
                                    needsReauth so it survives relaunch. */
  ENILSyncStateOffline      = 6, /* SSE intentionally disabled by the user (the
                                    bottom-bar link toggle). Persisted via
                                    session.json sseEnabled. Set ONLY by the
                                    toggle, so the UI treats it as the single
                                    source of truth for the link/link-slash
                                    glyph. */
  ENILSyncStateReconnecting = 7, /* SSE was torn down for an in-flight token
                                    refresh (talkException 119). Transient: the
                                    refresh restores Live on success, or drops to
                                    Error on failure. Set so the bar never reads
                                    "Syncing Live" while the stream is actually
                                    down. Not persisted. */
} ENILSyncState;

@interface ENILAccount : NSObject {
 @private
  NSString            *enilDir_;
  NSString            *accessToken_;
  NSString            *myMid_;
  sqlite3             *db_;
  ENILSSEClient       *sseClient_;
  int                  sseEnabled_;   /* desired SSE state; persisted in session.json */
  enil_health_t       *health_;       /* per-account LINE failure gate */
  ENILSyncState        syncState_;
  int                  miniSyncDepth_;
  NSMutableDictionary *pendingSends_; /* real_message_id -> temp_id; guarded by @synchronized */
  NSMutableDictionary *pendingSendOrderByChat_; /* chat_id -> NSMutableArray<temp_id> */
  NSTimer             *tokenRefreshTimer_;     /* fires when token is due for refresh */
  long long            tokenRefreshBackoffMs_; /* current retry delay; 0 = back to policy initial */
  NSMutableArray      *coalescedJS_;   /* SSE-driven {chat_id, js} awaiting a batched flush */
  NSMutableArray      *coalescedSSE_;  /* SSE event userInfos awaiting a batched flush */
  NSTimer             *coalesceTimer_; /* resetting debounce timer; see -coalesceMessageJS: */
  NSTimeInterval       coalesceDeadline_; /* abs time the batch MUST flush (max-wait cap); 0 = idle */
}

/* QR-code module grid for `string` — a clean ObjC wrapper over the qrcodegen
 * C lib so UI rendering code (e.g. ENILQRImage) never touches qrcodegen.h.
 * Returns size*size bytes, one per module (1 = dark, 0 = light), or nil on
 * bad input / encode failure. *outSize (may be NULL) = modules per side,
 * excluding the quiet zone (a render-side concern). */
+ (NSData *)qrModulesForString:(NSString *)string size:(int *)outSize;
/* Prepare fresh staging before requesting a QR. profile is chrome/desktopwin
 * for Add Account. Reauthentication copies the existing MID's exact identity. */
+ (BOOL)prepareQRLoginAtPath:(NSString *)accountDir
               clientProfile:(NSString *)profile
         reauthenticatingMid:(NSString *)mid;
/* Runs the blocking LINE QR-login handshake for the account at `accountDir`,
 * writing <accountDir>/session.json on success. This BLOCKS on long-polls for
 * tens of seconds — call it on a background thread, never the main runloop.
 * `observer` callbacks are delivered on the main thread. `cancelFlag` (may be
 * NULL) is polled by the flow; set it non-zero to abandon at the next poll
 * boundary. Returns YES on success. */
+ (BOOL)runQRLoginAtPath:(NSString *)accountDir
                observer:(id <ENILQRLoginObserver>)observer
              cancelFlag:(const volatile int *)cancelFlag;
/* Process-global network-health gate (see enil_health.h). These are sticky
 * failure flags shared across every account and network funnel, so they are
 * class-level — they read no per-instance state. */
+ (BOOL)workerFailed;
+ (BOOL)lineFailed;
+ (BOOL)anyServiceFailed;

/* One-time process configuration shared by the macOS and iOS app delegates.
 * Derives the cross-platform values itself — font directory from the main
 * bundle's Fonts/ subfolder, LINE Accept-Language / X-LAL from the bundle's
 * -preferredLocalizations — and pushes them into the C layer. `caCertPath`
 * is the one platform-specific input (the curl CA bundle lives in different
 * places on macOS vs iOS) and must be non-nil: curl needs it before any
 * network call. Raises NSInvalidArgumentException if caCertPath is nil. */
+ (void)bootstrapProcessWithCACertPath:(NSString *)caCertPath;
/* Read ENILKeychain's worker URL/secret and push them into the C worker
 * layer. Idempotent — call at launch and on ENILKeychainDidChangeNotification. */
+ (void)syncWorkerCredentialsFromKeychain;
/* Validate the session.json at `path`; returns the account mid, or nil if the
 * session is invalid / has no mid. The access token is validated but not
 * returned — callers only ever need identity. */
+ (NSString *)validatedMidForSessionAtPath:(NSString *)path;
/* displayName from the session.json at `path`, or nil if absent/invalid. */
+ (NSString *)displayNameForSessionAtPath:(NSString *)path;

- (id)initWithAccountPath:(NSString *)path;
- (BOOL)startWithError:(NSError **)outError;
- (void)startSync;
- (void)startSSE;
- (void)stopSSE;
- (void)forceReconnectSSE;
/* User-facing SSE on/off, persisted in session.json. -isSSEEnabled is the
 * desired state (drives the bottom-bar link/link-slash glyph), NOT live
 * connection status. -setSSEEnabled: persists, then connects (startSSE) or
 * disconnects (stopSSE + ENILSyncStateOffline). The persisted flag is honored
 * by every -startSSE caller (launch, post-sync, wake), so "off" sticks. */
- (BOOL)isSSEEnabled;
- (void)setSSEEnabled:(BOOL)enabled;
/* Re-broadcast the current sync state (ENILSyncStateChangedNotification) so a
 * late observer — e.g. a status bar built after -startSSE already went Live —
 * can pick up the state it missed. Idempotent. */
- (void)announceSyncState;
/* Build a local-notification payload (@{@"title": sender, @"body": preview})
 * for an incoming SSE "message" event, or nil when it should not raise one:
 * a non-RECEIVE op (our own sends included) or any content type other than
 * text/sticon (0) and sticker (7). Reads the already-persisted messages_v2
 * row — so call it only after the SSE dispatch that wrote the message has run
 * (i.e. from an ENILSSEEventNotification handler). `data` is the
 * notification's @"data" value (the raw event JSON). */
- (NSDictionary *)notificationForSSEMessageData:(NSString *)data;
- (NSDictionary *)profile;
- (NSArray *)contacts;
- (NSArray *)chats;
/* One chat-list row by mid; nil when the chat has no message box. The surgical
 * counterpart to -chats for live single-chat updates (see ENILSSEEventNotification's
 * optional @"chat_id"). */
- (NSDictionary *)chatSummaryForId:(NSString *)chatId;
- (NSArray *)purchasedStickers;
- (NSArray *)purchasedSticons;
- (NSString *)renderStickerPickerHTMLForItems:(NSArray *)items
                                      compact:(BOOL)compact
                                          css:(NSString *)css
                                    thumbSize:(int)thumbSize;
/* Render the sticker / sticon picker HTML and write it into enilDir_, then
 * return the file:// URL of the written file. Two stable filenames live at
 * the per-account directory root:
 *   sticker-picker.html  ← sticonMode == NO  (standard 108/100 grid)
 *   sticon-picker.html   ← sticonMode == YES (compact   48/42  grid)
 * Returns nil on render or disk failure. The file is overwritten on every
 * call; the caller is expected to feed the URL to WKWebView via
 * -loadFileURL:allowingReadAccessToURL: scoped to enilDir_. */
- (NSURL *)writeStickerPickerHTMLForSticonMode:(BOOL)sticonMode
                                            css:(NSString *)css
                                      thumbSize:(int)thumbSize;
- (NSString *)htmlPageForChatId:(NSString *)chatId;
/* Initial page render. *outOldest = created_at of the oldest rendered
 * message (the pagination cursor); *outHasMore = YES if older history
 * exists. Either out pointer may be NULL. */
- (NSString *)htmlPageForChatId:(NSString *)chatId
                     oldestTime:(long long *)outOldest
                        hasMore:(BOOL *)outHasMore;
/* Render the initial page of `chatId` and write it into
 * <enilDir>/chats/<chatId>.html, then return the file:// URL. A nil/empty
 * chatId writes a "No Chat Selected" placeholder to chats/_empty.html
 * instead (mids never start with `_` so there's no collision). Out
 * parameters carry the same pagination cursor as the string variant;
 * both may be NULL. Returns nil on disk failure. The caller is expected
 * to feed the URL to a WKWebView via -loadFileURL:allowingReadAccessToURL:
 * scoped to enilDir_ (the only WKWebView load entry point that grants the
 * page a real file:// origin — see CLAUDE.md "Per-Account Data Layout"). */
- (NSURL *)writeChatPageHTMLForChatId:(NSString *)chatId
                            outOldest:(long long *)outOldest
                           outHasMore:(BOOL *)outHasMore;
/* prependMessages([...],more) JS for the page older than `before`, or nil if
 * nothing older. *outOldest = new oldest cursor, *outHasMore = more remains. */
- (NSString *)prependMessagesJSForChatId:(NSString *)chatId
                              beforeTime:(long long)before
                                 hasMore:(BOOL *)outHasMore
                           newOldestTime:(long long *)outOldest;
/* YES if a chats_v2 row already exists for `mid`. NO for nil/empty input or
 * a never-messaged friend (implicit 1:1 chat not yet materialized). */
- (BOOL)chatExistsForMid:(NSString *)mid;
- (BOOL)sendText:(NSString *)text toChatId:(NSString *)chatId;
- (BOOL)sendInlineSticons:(NSArray *)resources text:(NSString *)text toChatId:(NSString *)chatId;
- (BOOL)sendStickerId:(NSString *)stickerId packageId:(NSString *)packageId toChatId:(NSString *)chatId;
/* Send a PNG or JPEG file at `path` to `chatId`. Generates a thumbnail and
 * inserts an optimistic bubble immediately; the wire-send runs on a background
 * thread. Currently the background thread is stubbed and always fails. */
- (BOOL)sendImageAtPath:(NSString *)path toChatId:(NSString *)chatId;
/* Mark `messageId` as the last-seen marker for `chatId` and zero its unread
 * count locally. Runs the RPC on a background thread. */
- (BOOL)markChatSeen:(NSString *)chatId messageId:(NSString *)messageId;
/* Same, but uses message_boxes_v2.lastDeliveredMessageId — the server-side
 * high-water-mark. Use this for the "Mark as Seen" footer click so messages
 * that arrived after page render are still covered. */
- (BOOL)markChatSeenAtServerHead:(NSString *)chatId;
/* unreadCount for the chat from message_boxes_v2 (0 if none/unknown).
 * Drives the "Mark as Seen" toolbar item's enabled state. */
- (int)unreadCountForChatId:(NSString *)chatId;
/* Sum of unreadCount across every chat (0 if all caught up). Reuses the same
 * chat-summary rows as -unreadCountForChatId:, so it stays consistent with the
 * per-chat unread dots. Drives the app-icon / Dock badge. */
- (int)totalUnreadCount;
/* lastDeliveredTime (ms since epoch) for the chat's message_box_v2 row.
 * Returns 0 for unknown / never-delivered chats. Drives the window subtitle. */
- (long long)lastDeliveredTimeForChatId:(NSString *)chatId;
/* Leave/delete a chat. On success every local row for that chat is removed.
 * Pass the last message id/time the user saw (may be nil/0). */
- (BOOL)removeChat:(NSString *)chatId
   lastReadMessageId:(NSString *)lastReadMessageId
 lastReadMessageTime:(long long)lastReadMessageTime;
/* Local-only delete: drops every local row for the chat (chats_v2,
 * message_boxes_v2, messages_v2, message_sticons*) without any LINE API
 * call. There is no LINE RPC to delete a chat. Returns NO on bad input. */
- (BOOL)deleteChatLocally:(NSString *)chatId;
- (NSString *)enilDir;
- (NSURL *)fileURLForPurchasesPackage:(NSString *)packageId asset:(NSString *)filename;
- (NSString *)accessToken;
/* Protocol fact: is this mid a 1:1 user chat (vs group/room/square)?
 * Wraps the shared mid-prefix rule so UI never parses mids directly. */
- (BOOL)isOneToOneChatId:(NSString *)chatId;

@end
