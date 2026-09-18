/* ENILAccount — public ObjC facade. View controllers import ENILAccount.h
 * only; the data layer (enil_db.h, ENILDictionary.h, enil_html.h) lives here. */

#import "ENILAccount.h"
#import "ENILUserDefaults.h"
#import "ENILKeychain.h"
#import "ENILDictionary.h"
#include "cJSON.h"
#import "XPFoundation.h"
#import <AltivecCore/AltivecCore.h>
#include "enil_account.h"
#import "enil_db.h"
#include "enil_html.h"
#include "enil_http.h"
#include "enil_cocoa_progress.h"
#include "enil_session.h"
#include "enil_line.h"
#include "enil_talkserv.h"
#include "enil_sse.h"
#include "enil_qrlogin.h"
#include "enil_native_login.h"
#include "enil_login_store.h"
#include "enil_worker.h"
#include "enil_health.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#import "enil_cocoa_log.h"

NSString * const ENILErrorDomain               = @"com.enil.error";
NSString * const ENILSyncStatusNotification    = @"ENILSyncStatusNotification";
NSString * const ENILSyncDidFinishNotification = @"ENILSyncDidFinishNotification";
NSString * const ENILMessageJSNotification     = @"ENILMessageJSNotification";
NSString * const ENILSSEEventNotification      = @"ENILSSEEventNotification";
NSString * const ENILSyncStateChangedNotification = @"ENILSyncStateChangedNotification";

static NSError *makeError(ENILErrorCode code, NSString *desc);

static void queue_pending_temp(NSMutableDictionary *map, NSString *chatId, NSString *tempId);
static void remove_pending_temp(NSMutableDictionary *map, NSString *tempId);
static NSString *pop_pending_temp_for_chat(NSMutableDictionary *map, NSString *chatId);

static NSError *makeError(ENILErrorCode code, NSString *desc)
{
  NSDictionary *info = [NSDictionary dictionaryWithObject:desc
                                                   forKey:NSLocalizedDescriptionKey];
  return [NSError errorWithDomain:ENILErrorDomain code:code userInfo:info];
}

static void queue_pending_temp(NSMutableDictionary *map, NSString *chatId, NSString *tempId)
{
  NSMutableArray *queue;
  if (!map || !chatId || !tempId) return;
  queue = [map objectForKey:chatId];
  if (!queue) {
    queue = [NSMutableArray array];
    [map setObject:queue forKey:chatId];
  }
  [queue addObject:tempId];
}

static void remove_pending_temp(NSMutableDictionary *map, NSString *tempId)
{
  NSEnumerator *keyEnum;
  NSString *chatId;
  if (!map || !tempId) return;
  keyEnum = [map keyEnumerator];
  while ((chatId = [keyEnum nextObject])) {
    NSMutableArray *queue = [map objectForKey:chatId];
    NSUInteger idx = [queue indexOfObject:tempId];
    if (idx != NSNotFound) {
      [queue removeObjectAtIndex:idx];
      if ([queue count] == 0) [map removeObjectForKey:chatId];
      return;
    }
  }
}

static NSString *pop_pending_temp_for_chat(NSMutableDictionary *map, NSString *chatId)
{
  NSMutableArray *queue;
  NSString *tempId;
  if (!map || !chatId) return nil;
  queue = [map objectForKey:chatId];
  if (!queue || [queue count] == 0) return nil;
  tempId = [[[queue objectAtIndex:0] retain] autorelease];
  [queue removeObjectAtIndex:0];
  if ([queue count] == 0) [map removeObjectForKey:chatId];
  return tempId;
}

@interface ENILAccount ()
- (void)beginMiniSyncUI;
- (void)endMiniSyncUI;
- (void)handleSSETalkExceptionResult:(NSDictionary *)info;
- (void)scheduleNextTokenRefresh;
- (void)cancelTokenRefreshTimer;
- (void)tokenRefreshTimerFired:(NSTimer *)timer;
- (void)tokenRefreshInBackground;
- (void)tokenRefreshDidFinish:(NSDictionary *)result;
- (void)postMessageJS:(NSDictionary *)info;
- (void)postSSEEvent:(NSDictionary *)info;
- (void)coalesceMessageJS:(NSDictionary *)info;
- (void)coalesceSSEEvent:(NSDictionary *)info;
- (void)bumpCoalesceTimer;
- (void)flushCoalescedUI:(NSTimer *)timer;
- (void)flushCoalescedJS;
- (void)flushCoalescedSSE;
- (void)syncDidFinish:(NSNotification *)note;
- (void)syncInBackground;
- (void)sendTextInBackground:(NSDictionary *)params;
- (void)sendInlineSticons_BG:(NSDictionary *)params;
- (void)sendStickerInBackground:(NSDictionary *)params;
- (void)sendImageInBackground:(NSDictionary *)params;
- (void)markChatSeenInBackground:(NSDictionary *)params;
- (void)removeChatInBackground:(NSDictionary *)params;
- (NSString *)displayNameForOutgoingMessage;
- (void)confirmTempId:(NSString *)tempId realMessageId:(const char *)messageId;
- (void)failTempId:(NSString *)tempId chatId:(NSString *)chatId;
@end

/* Messages loaded per page (initial render and each "Load earlier"). */
static const int kENILMessagePageSize = 40;

/* C → ObjC trampolines for the QR-login flow. enil_qrlogin_run invokes these
 * on the background login thread; ctx is the ENILQRLoginObserver. Each hops to
 * the main thread so observers are pure UI code. -------------------------- */

static void qr_url_trampoline(const char *url, void *ctx)
{
  id <ENILQRLoginObserver> obs = (id <ENILQRLoginObserver>)ctx;
  NSString *s = url ? [NSString stringWithUTF8String:url] : nil;
  [(id)obs performSelectorOnMainThread:@selector(qrLoginDidEmitURL:)
                            withObject:s
                         waitUntilDone:NO];
}

static void qr_pin_trampoline(const char *pin, void *ctx)
{
  id <ENILQRLoginObserver> obs = (id <ENILQRLoginObserver>)ctx;
  NSString *s = pin ? [NSString stringWithUTF8String:pin] : nil;
  [(id)obs performSelectorOnMainThread:@selector(qrLoginDidEmitPIN:)
                            withObject:s
                         waitUntilDone:NO];
}

static void qr_status_trampoline(const char *msg, void *ctx)
{
  id <ENILQRLoginObserver> obs = (id <ENILQRLoginObserver>)ctx;
  if (![obs respondsToSelector:@selector(qrLoginDidEmitStatus:)]) return;
  NSString *s = msg ? [NSString stringWithUTF8String:msg] : nil;
  [(id)obs performSelectorOnMainThread:@selector(qrLoginDidEmitStatus:)
                            withObject:s
                         waitUntilDone:NO];
}

@implementation ENILAccount

- (id)initWithAccountPath:(NSString *)path;
{
  if ((self = [super init])) {
    enilDir_      = [path retain];
    pendingSends_ = [[NSMutableDictionary alloc] init];
    pendingSendOrderByChat_ = [[NSMutableDictionary alloc] init];
    health_       = enil_health_create([[path lastPathComponent] UTF8String]);
  }
  return self;
}

- (void)dealloc;
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [self cancelTokenRefreshTimer];
  if (sseClient_) enil_sse_free(sseClient_); /* joins SSE thread (uses health_) */
  enil_health_destroy(health_);              /* safe: SSE thread now joined */
  health_ = NULL;
  [coalesceTimer_ invalidate];               /* SSE thread joined: no new arms */
  [coalesceTimer_ release];
  [coalescedJS_ release];
  [coalescedSSE_ release];
  enil_db_close(db_);
  [enilDir_ release];
  [accessToken_ release];
  [myMid_ release];
  [pendingSends_ release];
  [pendingSendOrderByChat_ release];
  [super dealloc];
}

- (void)setSyncState:(ENILSyncState)state;
{
  syncState_ = state;
  [self announceSyncState];
}

/* Re-broadcast the current sync state. An observer that subscribed AFTER the
 * last change — e.g. the iOS chat-list status bar, built after -startSSE
 * already went Live — calls this once it is listening, to pick up the state it
 * missed instead of sitting at NeverSynced. Idempotent: re-asserts the true
 * state, so a redundant call is harmless. */
- (void)announceSyncState;
{
  NSDictionary *info = [NSDictionary dictionaryWithObject:
    [NSNumber numberWithInt:(int)syncState_] forKey:@"state"];
  [[NSNotificationCenter defaultCenter]
    postNotificationName:ENILSyncStateChangedNotification
                  object:self
                userInfo:info];
}

/* Historical pair (kept for source compatibility — UI no longer flips into
 * Syncing on SSE bursts). Per-event feedback is now posted via
 * enil_status_post() in sse_event_cb so the steady-state shimmer keeps
 * playing and a transient row scrolls into the queue instead of a
 * progress bar appearing/vanishing on every received message. */
- (void)beginMiniSyncUI;
{
  (void)miniSyncDepth_;
}

- (void)endMiniSyncUI;
{
}

- (void)handleSSETalkExceptionResult:(NSDictionary *)info;
{
  int code = [[info objectForKey:@"code"] intValue];
  int action = [[info objectForKey:@"action"] intValue];

  if (action == ENIL_ACCOUNT_SSE_TALK_ACTION_REAUTH_NEEDED) {
    ENILLog(@"ENILAccount.handleSSETalkException", @"code=%d -> reauth needed",
            code);
    [self setSyncState:ENILSyncStateReauthNeeded];
    return;
  }

  if (action == ENIL_ACCOUNT_SSE_TALK_ACTION_REFRESH_RECONNECT) {
    ENILLog(@"ENILAccount.handleSSETalkException",
            @"code=%d -> refresh+reconnect", code);
    /* The SSE stream has already been freed (should_stop_sse) by the time we
     * get here. Reflect that the live connection is GONE while the async
     * refresh runs, otherwise the bar keeps claiming "Syncing Live" for a
     * stream that no longer exists — and if the refresh stalls (e.g. a wedged
     * worker socket across an iOS background/resume) it sits on that lie until
     * the refresh finally fails. -tokenRefreshDidFinish: restores Live on
     * success or drops to Error on failure, so this state is always transient. */
    [self setSyncState:ENILSyncStateReconnecting];
    [self cancelTokenRefreshTimer];
    [NSThread detachNewThreadSelector:@selector(tokenRefreshInBackground)
                             toTarget:self
                           withObject:nil];
    return;
  }

  if (action == ENIL_ACCOUNT_SSE_TALK_ACTION_NONE) return;

  /* Any other talkException means the server has rejected this session for
   * now — stop any scheduled refresh too, otherwise the proactive timer keeps
   * firing in the background after we've surfaced the error. */
  [self cancelTokenRefreshTimer];
  if (action == ENIL_ACCOUNT_SSE_TALK_ACTION_SYNC_NEEDED)
    [self setSyncState:ENILSyncStateSyncNeeded];
  else
    [self setSyncState:ENILSyncStateError];
}

- (void)cancelTokenRefreshTimer;
{
  if (!tokenRefreshTimer_) return;
  [tokenRefreshTimer_ invalidate];
  [tokenRefreshTimer_ release];
  tokenRefreshTimer_ = nil;
}

- (void)scheduleNextTokenRefresh;
{
  NSString *sessionPath;
  long long eta, now;
  NSTimeInterval delay;
  [self cancelTokenRefreshTimer];
  if (!accessToken_) return;
  sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  eta = enil_session_get_token_refresh_eta([sessionPath fileSystemRepresentation]);
  if (eta <= 0) {
    ENILLog(@"ENILAccount.scheduleNextTokenRefresh", @"no eta in session.json; skipping");
    return;
  }
  now = (long long)time(NULL);
  delay = (NSTimeInterval)(eta - now);
  if (delay < 1.0) delay = 1.0;
  tokenRefreshTimer_ = [[NSTimer scheduledTimerWithTimeInterval:delay
                                                         target:self
                                                       selector:@selector(tokenRefreshTimerFired:)
                                                       userInfo:nil
                                                        repeats:NO] retain];
  ENILLog(@"ENILAccount.scheduleNextTokenRefresh", @"in %.0fs", (double)delay);
}

- (void)tokenRefreshTimerFired:(NSTimer *)timer;
{
  (void)timer;
  [tokenRefreshTimer_ release];
  tokenRefreshTimer_ = nil;
  [NSThread detachNewThreadSelector:@selector(tokenRefreshInBackground)
                           toTarget:self
                         withObject:nil];
}

- (void)tokenRefreshInBackground;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  enil_health_bind(health_); /* scope LINE gate to this account */
  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  int line_code = 0;
  char *new_token = enil_line_token_refresh([sessionPath fileSystemRepresentation],
                                            &line_code);
  NSDictionary *result = [NSDictionary dictionaryWithObjectsAndKeys:
    [NSNumber numberWithBool:(new_token != NULL)], @"ok",
    [NSNumber numberWithInt:line_code],            @"lineCode",
    nil];
  free(new_token);
  [self performSelectorOnMainThread:@selector(tokenRefreshDidFinish:)
                         withObject:result
                      waitUntilDone:NO];
  [pool release];
}

- (void)tokenRefreshDidFinish:(NSDictionary *)result;
{
  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  BOOL      ok       = [[result objectForKey:@"ok"]       boolValue];
  int       lineCode = [[result objectForKey:@"lineCode"] intValue];
  if (!ok) {
    long long initial_ms = 1000, max_ms = 32000;
    double mult = 2.0, jitter = 0.2;
    double jitter_mul;
    NSTimeInterval retry_s;
    /* 10201 AUTH_INVALID_REQUEST: the refresh token itself is dead — same
     * recovery as SSE talkException 117. No amount of backoff will recover;
     * persist reauth-needed so the state survives relaunch and stop
     * retrying. */
    if (lineCode == 10201) {
      ENILLog(@"ENILAccount.tokenRefreshDidFinish", @"code=10201 -> reauth needed");
      [self cancelTokenRefreshTimer];
      tokenRefreshBackoffMs_ = 0;
      enil_session_set_reauth_needed([sessionPath fileSystemRepresentation]);
      [self setSyncState:ENILSyncStateReauthNeeded];
      return;
    }
    /* Transient failure: back off per refreshApiRetryPolicy. multiplier=2
     * means delay doubles each attempt; capped at maxDelayInMillis. Jitter
     * is symmetric: [delay * (1 - jitter), delay * (1 + jitter)]. */
    enil_session_get_refresh_retry_policy([sessionPath fileSystemRepresentation],
                                          &initial_ms, &max_ms, &mult, &jitter);
    if (tokenRefreshBackoffMs_ <= 0) tokenRefreshBackoffMs_ = initial_ms;
    else {
      tokenRefreshBackoffMs_ = (long long)(tokenRefreshBackoffMs_ * mult);
      if (tokenRefreshBackoffMs_ > max_ms) tokenRefreshBackoffMs_ = max_ms;
    }
    jitter_mul = 1.0 + (((double)random() / (double)RAND_MAX) * 2.0 - 1.0) * jitter;
    retry_s = ((double)tokenRefreshBackoffMs_ * jitter_mul) / 1000.0;
    if (retry_s < 0.1) retry_s = 0.1;
    [self cancelTokenRefreshTimer];
    tokenRefreshTimer_ = [[NSTimer scheduledTimerWithTimeInterval:retry_s
                                                           target:self
                                                         selector:@selector(tokenRefreshTimerFired:)
                                                         userInfo:nil
                                                          repeats:NO] retain];
    ENILLog(@"ENILAccount.tokenRefreshDidFinish", @"failed (code=%d); retry in %.2fs (backoff=%lldms)",
          lineCode, (double)retry_s, tokenRefreshBackoffMs_);
    [self setSyncState:ENILSyncStateError];
    return;
  }
  /* Success — pick up the new token from session.json and restart SSE. */
  tokenRefreshBackoffMs_ = 0;
  {
    char *token = NULL, *mid = NULL;
    if (enil_session_validate([sessionPath fileSystemRepresentation], &token, &mid)) {
      if (token && token[0]) {
        [accessToken_ release];
        accessToken_ = [[NSString alloc] initWithUTF8String:token];
      }
    }
    free(token);
    free(mid);
  }
  [self stopSSE];
  [self startSSE];
  [self scheduleNextTokenRefresh];
}

- (void)syncDidFinish:(NSNotification *)note;
{
  NSString *sessionPath;
  char *token = NULL;
  char *mid = NULL;
  BOOL ok = [[[note userInfo] objectForKey:@"ok"] boolValue];
  if (ok) {
    sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
    if (enil_session_validate([sessionPath fileSystemRepresentation], &token, &mid)) {
      NSString *newToken = token ? [NSString stringWithUTF8String:token] : nil;
      if (newToken && [newToken length]) {
        [accessToken_ release];
        accessToken_ = [newToken retain];
      }
    }
    free(token);
    free(mid);
    [self startSSE]; /* restarts SSE with updated localRev; posts ENILSyncStateLive */
    [self scheduleNextTokenRefresh];
  } else
    [self setSyncState:ENILSyncStateError];
}

- (void)syncInBackground;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  enil_account_sync_all(health_, db_,
                        [accessToken_ UTF8String],
                        myMid_ ? [myMid_ UTF8String] : NULL,
                        [sessionPath fileSystemRepresentation]);
  [pool release];
}

- (void)startSync;
{
  [self stopSSE];
  [self setSyncState:ENILSyncStateSyncing];
  [NSThread detachNewThreadSelector:@selector(syncInBackground)
                           toTarget:self
                         withObject:nil];
}

- (NSDictionary *)profile;
{
  talk_profile_t *heap;
  if (!db_) return nil;
  heap = (talk_profile_t *)calloc(1, sizeof(*heap));
  if (enil_db_talk_profile_get(db_, heap) != 0) {
    free(heap);
    return nil;
  }
  return [[[ENILDictionary alloc]
            initWithBase:heap
                  fields:talk_profile_fields
                   count:talk_profile_fields_count
                  freeFn:(enil_struct_free_fn)talk_profile_free] autorelease];
}

- (NSArray *)contacts;
{
  talk_contact_t *rows = NULL;
  int n = 0, i;
  NSMutableArray *result;
  if (!db_) return [NSArray array];
  enil_db_talk_contact_get_all(db_, &rows, &n);
  result = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
  for (i = 0; i < n; i++) {
    talk_contact_t *heap = (talk_contact_t *)malloc(sizeof(talk_contact_t));
    if (!heap) { talk_contact_free(&rows[i]); continue; }
    *heap = rows[i]; /* transfer string ownership to heap */
    [result addObject:[[[ENILDictionary alloc]
                        initWithBase:heap
                              fields:talk_contact_fields
                               count:talk_contact_fields_count
                              freeFn:(enil_struct_free_fn)talk_contact_free] autorelease]];
  }
  free(rows); /* strings owned by ENILDictionary instances; only free the array */
  return result;
}

- (NSArray *)chats;
{
  chat_summary_t *rows = NULL;
  int n = 0, i;
  NSMutableArray *result;
  if (!db_) return [NSArray array];
  enil_db_chat_summary_get_all(db_, &rows, &n);
  result = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
  for (i = 0; i < n; i++) {
    chat_summary_t *heap = (chat_summary_t *)malloc(sizeof(chat_summary_t));
    if (!heap) { chat_summary_free(&rows[i]); continue; }
    *heap = rows[i]; /* transfer string ownership to heap */
    [result addObject:[[[ENILDictionary alloc]
                        initWithBase:heap
                              fields:chat_summary_fields
                               count:chat_summary_fields_count
                              freeFn:(enil_struct_free_fn)chat_summary_free] autorelease]];
  }
  free(rows);
  return result;
}

/* Single chat-list row by mid — the surgical counterpart to -chats. Returns nil
 * when the chat has no message box (never messaged or just deleted), which the
 * UI reads as "drop this row from the list". One sqlite lookup, no roster
 * rebuild: this is what lets an incoming message skip re-querying every chat
 * and every contact. */
- (NSDictionary *)chatSummaryForId:(NSString *)chatId;
{
  chat_summary_t *heap;
  if (!db_ || ![chatId isKindOfClass:[NSString class]] || ![chatId length])
    return nil;
  heap = (chat_summary_t *)calloc(1, sizeof(*heap));
  if (!heap) return nil;
  if (enil_db_chat_summary_get_one(db_, [chatId UTF8String], heap) != 0) {
    free(heap);
    return nil;
  }
  return [[[ENILDictionary alloc]
            initWithBase:heap
                  fields:chat_summary_fields
                   count:chat_summary_fields_count
                  freeFn:(enil_struct_free_fn)chat_summary_free] autorelease];
}

- (NSArray *)purchasedStickers;
{
  sticker_row_t *rows = NULL;
  int n = 0, i;
  NSMutableArray *result;
  if (!db_) return [NSArray array];
  enil_db_sticker_rows_get(db_, &rows, &n);
  result = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
  for (i = 0; i < n; i++) {
    sticker_row_t *heap = (sticker_row_t *)malloc(sizeof(sticker_row_t));
    if (!heap) { sticker_row_free(&rows[i]); continue; }
    *heap = rows[i];
    [result addObject:[[[ENILDictionary alloc]
                        initWithBase:heap
                              fields:sticker_row_fields
                               count:sticker_row_fields_count
                              freeFn:(enil_struct_free_fn)sticker_row_free] autorelease]];
  }
  free(rows);
  return result;
}

- (NSArray *)purchasedSticons;
{
  sticon_row_t *rows = NULL;
  int n = 0, i;
  NSMutableArray *result;
  if (!db_) return [NSArray array];
  enil_db_sticon_rows_get(db_, &rows, &n);
  result = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
  for (i = 0; i < n; i++) {
    sticon_row_t *heap = (sticon_row_t *)malloc(sizeof(sticon_row_t));
    if (!heap) { sticon_row_free(&rows[i]); continue; }
    *heap = rows[i];
    [result addObject:[[[ENILDictionary alloc]
                        initWithBase:heap
                              fields:sticon_row_fields
                               count:sticon_row_fields_count
                              freeFn:(enil_struct_free_fn)sticon_row_free] autorelease]];
  }
  free(rows);
  return result;
}

/* Borrow a UTF-8 buffer from a dictionary string value. NULL when the key is
 * absent or non-string. The returned pointer is valid only while `d` (and the
 * enclosing autorelease pool) lives — used solely for synchronous C calls. */
static const char *enil_dict_cstr(id d, NSString *key) {
  id v = [d objectForKey:key];
  return [v isKindOfClass:[NSString class]] ? [(NSString *)v UTF8String] : NULL;
}

- (NSString *)renderStickerPickerHTMLForItems:(NSArray *)items
                                      compact:(BOOL)compact
                                          css:(NSString *)css
                                    thumbSize:(int)thumbSize;
{
  NSUInteger count = [items count], i;
  ENILHTMLSticker *st;
  char *html;
  NSString *result;

  if (count == 0) return @"";
  st = (ENILHTMLSticker *)calloc(count, sizeof(*st));
  if (!st) return @"";

  for (i = 0; i < count; i++) {
    id d = [items objectAtIndex:i];
    id thumb = [d objectForKey:@"thumb_path"];
    id image = [d objectForKey:@"image_path"];
    st[i].sticker_id   = enil_dict_cstr(d, @"sticker_id");
    st[i].package_id   = enil_dict_cstr(d, @"package_id");
    st[i].package_name = enil_dict_cstr(d, @"package_name");
    st[i].image_path   = enil_dict_cstr(d, @"image_path");
    st[i].alt_text     = enil_dict_cstr(d, @"alt_text");
    st[i].image_width  = [[d objectForKey:@"image_width"] intValue];
    st[i].image_height = [[d objectForKey:@"image_height"] intValue];
    st[i].is_animated  = ([thumb length] > 0 && [image length] > 0
                          && ![thumb isEqual:image]) ? 1 : 0;
  }

  html = compact
    ? enil_html_sticon_picker_page_with_css(st, (int)count,
                                            [css UTF8String], thumbSize, 4)
    : enil_html_sticker_picker_page_with_css(st, (int)count,
                                             [css UTF8String], thumbSize, 4);
  free(st);

  if (!html) return @"";
  result = [NSString stringWithUTF8String:html];
  enil_html_free(html);
  return result;
}

- (NSURL *)writeStickerPickerHTMLForSticonMode:(BOOL)sticonMode
                                            css:(NSString *)css
                                      thumbSize:(int)thumbSize;
{
  NSArray *items = sticonMode ? [self purchasedSticons]
                              : [self purchasedStickers];
  NSString *html = [self renderStickerPickerHTMLForItems:items
                                                 compact:sticonMode
                                                     css:css
                                               thumbSize:thumbSize];
  if (![html length]) return nil;

  /* Two stable filenames so a mode switch never overwrites the other tab's
   * cache — useful if a future reload-skip optimization wants to check
   * whether the on-disk file is still fresh. The files live at the
   * per-account root next to session.json / enil.sqlite; see CLAUDE.md
   * "Per-Account Data Layout". */
  NSString *name = sticonMode ? @"sticon-picker.html" : @"sticker-picker.html";
  NSString *path = [enilDir_ stringByAppendingPathComponent:name];
  NSError *err = nil;
  BOOL ok = [html writeToFile:path
                   atomically:YES
                     encoding:NSUTF8StringEncoding
                        error:&err];
  if (!ok) {
    ENILLog(@"ENILAccount.writeStickerPickerHTML", @"write failed at %@: %@",
          path, err);
    return nil;
  }
  return [NSURL fileURLWithPath:path];
}

- (NSString *)htmlPageForChatId:(NSString *)chatId;
{
  return [self htmlPageForChatId:chatId oldestTime:NULL hasMore:NULL];
}

- (NSString *)htmlPageForChatId:(NSString *)chatId
                     oldestTime:(long long *)outOldest
                        hasMore:(BOOL *)outHasMore;
{
  int more = 0;
  char *html;
  NSString *result;

  if (outOldest)  *outOldest = 0;
  if (outHasMore) *outHasMore = NO;

  html = enil_account_chat_page_html(db_,
                                     [chatId length] ? [chatId UTF8String] : NULL,
                                     myMid_ ? [myMid_ UTF8String] : NULL,
                                     [[ENILUserDefaults messageCSS] UTF8String],
                                     [[ENILUserDefaults emptyMessageCSS] UTF8String],
                                     kENILMessagePageSize,
                                     outOldest, &more);
  if (outHasMore) *outHasMore = more ? YES : NO;
  if (!html) return @"";
  result = [NSString stringWithUTF8String:html];
  enil_account_free(html);
  return result;
}

- (NSURL *)writeChatPageHTMLForChatId:(NSString *)chatId
                            outOldest:(long long *)outOldest
                           outHasMore:(BOOL *)outHasMore;
{
  if (outOldest)  *outOldest  = 0;
  if (outHasMore) *outHasMore = NO;

  /* chats/ holds per-chat rendered HTML files; created lazily on first
   * write rather than at account-start so cold accounts don't grow an
   * empty directory. XP_create… is a no-op when the path already exists. */
  NSString *chatsDir = [enilDir_ stringByAppendingPathComponent:@"chats"];
  NSError *err = nil;
  if (![[NSFileManager defaultManager] XP_createDirectoryAtPath:chatsDir
                                    withIntermediateDirectories:YES
                                                     attributes:nil
                                                          error:&err]) {
    ENILLog(@"ENILAccount.writeChatPageHTML", @"chats dir create failed at %@: %@",
          chatsDir, err);
    return nil;
  }

  NSString *html;
  NSString *filename;
  if (![chatId length]) {
    /* "No Chat Selected" placeholder. Filename uses a leading underscore
     * which mids never have, so there's no collision with a real chatId. */
    html = [NSString stringWithFormat:
      @"<html><head>" ENIL_HTML_ZOOM_SCRIPT "<style>%@</style></head><body>"
       "<p>No Chat Selected</p></body></html>",
      [ENILUserDefaults emptyMessageCSS]];
    filename = @"_empty.html";
  } else {
    long long oldest = 0;
    BOOL more = NO;
    html = [self htmlPageForChatId:chatId oldestTime:&oldest hasMore:&more];
    if (outOldest)  *outOldest  = oldest;
    if (outHasMore) *outHasMore = more;
    filename = [chatId stringByAppendingPathExtension:@"html"];
  }
  if (![html length]) return nil;

  NSString *path = [chatsDir stringByAppendingPathComponent:filename];
  err = nil;
  if (![html writeToFile:path
              atomically:YES
                encoding:NSUTF8StringEncoding
                   error:&err]) {
    ENILLog(@"ENILAccount.writeChatPageHTML", @"write failed at %@: %@", path, err);
    return nil;
  }
  return [NSURL fileURLWithPath:path];
}

/* Build the prependMessages([...],more) JS for the page of messages older
 * than `before`. Returns nil when there is nothing older. *outHasMore and
 * *outOldest report whether more remains and the new oldest cursor. */
- (NSString *)prependMessagesJSForChatId:(NSString *)chatId
                              beforeTime:(long long)before
                                 hasMore:(BOOL *)outHasMore
                           newOldestTime:(long long *)outOldest;
{
  int more = 0;
  char *js;
  NSString *result;

  if (outHasMore) *outHasMore = NO;
  if (outOldest)  *outOldest = before;
  js = enil_account_prepend_messages_js(db_,
                                        [chatId length] ? [chatId UTF8String] : NULL,
                                        myMid_ ? [myMid_ UTF8String] : NULL,
                                        before, kENILMessagePageSize,
                                        &more, outOldest);
  if (outHasMore) *outHasMore = more ? YES : NO;

  if (!js) return nil;
  result = [NSString stringWithUTF8String:js];
  enil_account_free(js);
  return result;
}

- (BOOL)chatExistsForMid:(NSString *)mid;
{
  if (![mid isKindOfClass:[NSString class]] || ![mid length]) return NO;
  if (!db_) return NO;
  /* 1:1 chats live in message_boxes_v2, not chats_v2 (which is
   * group/room only). Check both so existing chats aren't treated as new. */
  const char *m = [mid UTF8String];
  return (enil_db_message_box_exists(db_, m)
          || enil_db_chat_exists(db_, m)) ? YES : NO;
}

- (void)postMessageJS:(NSDictionary *)info;
{
  [[NSNotificationCenter defaultCenter]
    postNotificationName:ENILMessageJSNotification
                  object:self
                userInfo:info];
}

- (NSString *)displayNameForOutgoingMessage;
{
  NSDictionary *prof = [self profile];
  NSString *name = prof ? [prof objectForKey:@"displayName"] : nil;
  return (name && [name length]) ? name : @"Me";
}

- (void)confirmTempId:(NSString *)tempId realMessageId:(const char *)messageId;
{
  if (!tempId || !messageId || !messageId[0]) return;
  NSString *realId = [NSString stringWithUTF8String:messageId];
  @synchronized(pendingSends_) {
    [pendingSends_ setObject:tempId forKey:realId];
  }
}

- (void)failTempId:(NSString *)tempId chatId:(NSString *)chatId;
{
  if (!tempId || !chatId) return;
  @synchronized(pendingSends_) {
    remove_pending_temp(pendingSendOrderByChat_, tempId);
  }
  char *jsRaw = enil_html_js_fail([tempId UTF8String]);
  NSString *js = jsRaw ? [NSString stringWithUTF8String:jsRaw] : nil;
  free(jsRaw);
  if (!js) return;
  NSDictionary *info = [NSDictionary dictionaryWithObjectsAndKeys:
    chatId, @"chat_id", js, @"js", nil];
  [self performSelectorOnMainThread:@selector(postMessageJS:)
                         withObject:info
                      waitUntilDone:NO];
}

- (BOOL)sendText:(NSString *)text toChatId:(NSString *)chatId;
{
  if (!text || !chatId) return NO;

  enil_status_post("send.message", "Sending message", 1);

  long long nowMs = (long long)([[NSDate date] timeIntervalSince1970] * 1000.0);
  NSString *tempId = [NSString stringWithFormat:@"local-%lld", nowMs];

  NSString *senderName = [self displayNameForOutgoingMessage];

  char *jsRaw = enil_html_js_append_text(db_, [tempId UTF8String],
                                         [senderName UTF8String],
                                         [text UTF8String], nowMs);
  NSString *js = jsRaw ? [NSString stringWithUTF8String:jsRaw] : nil;
  free(jsRaw);

  if (js) {
    NSDictionary *info = [NSDictionary dictionaryWithObjectsAndKeys:
      chatId, @"chat_id", js, @"js", nil];
    [self postMessageJS:info];
  }
  @synchronized(pendingSends_) {
    queue_pending_temp(pendingSendOrderByChat_, chatId, tempId);
  }

  NSDictionary *params = [NSDictionary dictionaryWithObjectsAndKeys:
    text,       @"text",
    chatId,     @"chat_id",
    tempId,     @"temp_id",
    nil];
  [NSThread detachNewThreadSelector:@selector(sendTextInBackground:)
                           toTarget:self
                         withObject:params];
  return YES;
}

- (void)sendTextInBackground:(NSDictionary *)params;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  NSString *text       = [params objectForKey:@"text"];
  NSString *chatId     = [params objectForKey:@"chat_id"];
  NSString *tempId     = [params objectForKey:@"temp_id"];

  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  char *messageId = NULL;
  int ok = enil_account_send_text(health_, db_,
                                  [sessionPath fileSystemRepresentation],
                                  [chatId UTF8String],
                                  [text UTF8String],
                                  &messageId);
  if (ok) {
    NSString *realId = [NSString stringWithUTF8String:messageId];
    [self confirmTempId:tempId realMessageId:messageId];
    [self markChatSeen:chatId messageId:realId];
  } else {
    [self failTempId:tempId chatId:chatId];
  }
  enil_account_free(messageId);

  [pool release];
}

/* Borrow package_id/sticon_id/alt_text C strings from an NSArray of sticon
 * resource dictionaries into three parallel arrays. Returns the item count, or
 * 0 (arrays freed/cleared) if empty, oversized, or any entry lacks
 * package_id/sticon_id. Pointers are valid only while `resources` and the
 * autorelease pool live. Mirrors enil_cocoa.m's sticon_items_from_cfarray. */
static int enil_sticon_arrays(NSArray *resources, const char ***pkg,
                              const char ***sid, const char ***alt) {
  NSUInteger i, n = [resources count];
  *pkg = NULL; *sid = NULL; *alt = NULL;
  if (n == 0 || n > 1024) return 0;
  *pkg = (const char **)calloc(n, sizeof(char *));
  *sid = (const char **)calloc(n, sizeof(char *));
  *alt = (const char **)calloc(n, sizeof(char *));
  if (!*pkg || !*sid || !*alt) {
    free(*pkg); free(*sid); free(*alt);
    *pkg = NULL; *sid = NULL; *alt = NULL;
    return 0;
  }
  for (i = 0; i < n; i++) {
    id d = [resources objectAtIndex:i];
    const char *a = enil_dict_cstr(d, @"alt_text");
    (*pkg)[i] = enil_dict_cstr(d, @"package_id");
    (*sid)[i] = enil_dict_cstr(d, @"sticon_id");
    (*alt)[i] = a ? a : "";
    if (!(*pkg)[i] || !(*sid)[i]) {
      free(*pkg); free(*sid); free(*alt);
      *pkg = NULL; *sid = NULL; *alt = NULL;
      return 0;
    }
  }
  return (int)n;
}

- (BOOL)sendInlineSticons:(NSArray *)resources text:(NSString *)text toChatId:(NSString *)chatId;
{
  if (!resources || ![resources count] || !text || !chatId) return NO;

  enil_status_post("send.message", "Sending message", 1);

  long long nowMs = (long long)([[NSDate date] timeIntervalSince1970] * 1000.0);
  NSString *tempId = [NSString stringWithFormat:@"local-%lld", nowMs];

  NSString *senderName = [self displayNameForOutgoingMessage];

  const char **pkg = NULL, **sid = NULL, **alt = NULL;
  int cnt = enil_sticon_arrays(resources, &pkg, &sid, &alt);
  char *jsRaw = cnt ? enil_html_js_append_inline_sticon(
                        db_, [tempId UTF8String], [senderName UTF8String],
                        [text UTF8String], nowMs, cnt, pkg, sid, alt)
                    : NULL;
  free(pkg); free(sid); free(alt);
  NSString *js = jsRaw ? [NSString stringWithUTF8String:jsRaw] : nil;
  enil_html_free(jsRaw);
  if (js) {
    NSDictionary *info = [NSDictionary dictionaryWithObjectsAndKeys:
      chatId, @"chat_id", js, @"js", nil];
    [self postMessageJS:info];
  }
  @synchronized(pendingSends_) {
    queue_pending_temp(pendingSendOrderByChat_, chatId, tempId);
  }

  NSDictionary *params = [NSDictionary dictionaryWithObjectsAndKeys:
    resources,  @"resources",
    text,       @"text",
    chatId,     @"chat_id",
    tempId,     @"temp_id",
    nil];
  [NSThread detachNewThreadSelector:@selector(sendInlineSticons_BG:)
                           toTarget:self
                         withObject:params];
  return YES;
}

- (void)sendInlineSticons_BG:(NSDictionary *)params;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  NSArray  *resources  = [params objectForKey:@"resources"];
  NSString *text       = [params objectForKey:@"text"];
  NSString *chatId     = [params objectForKey:@"chat_id"];
  NSString *tempId     = [params objectForKey:@"temp_id"];

  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  const char **pkgIds = NULL, **stiIds = NULL, **altTxts = NULL;
  int count = enil_sticon_arrays(resources, &pkgIds, &stiIds, &altTxts);
  char *messageId = NULL;
  int ok = count
    ? enil_account_send_inline_sticon(health_, db_,
                                      [sessionPath fileSystemRepresentation],
                                      [chatId UTF8String],
                                      [text UTF8String],
                                      count, pkgIds, stiIds, altTxts,
                                      &messageId)
    : 0;
  free(pkgIds); free(stiIds); free(altTxts);

  if (ok) {
    NSString *realId = [NSString stringWithUTF8String:messageId];
    [self confirmTempId:tempId realMessageId:messageId];
    [self markChatSeen:chatId messageId:realId];
  } else {
    [self failTempId:tempId chatId:chatId];
  }
  enil_account_free(messageId);
  [pool release];
}

- (BOOL)sendStickerId:(NSString *)stickerId packageId:(NSString *)packageId toChatId:(NSString *)chatId;
{
  if (!stickerId || !packageId || !chatId) return NO;

  enil_status_post("send.message", "Sending sticker", 1);

  long long nowMs = (long long)([[NSDate date] timeIntervalSince1970] * 1000.0);
  NSString *tempId = [NSString stringWithFormat:@"local-%lld", nowMs];

  char *jsRaw = enil_html_js_append_sticker(db_, [tempId UTF8String],
                                            [packageId UTF8String],
                                            [stickerId UTF8String], nowMs);
  NSString *js = jsRaw ? [NSString stringWithUTF8String:jsRaw] : nil;
  free(jsRaw);
  if (js) {
    NSDictionary *info = [NSDictionary dictionaryWithObjectsAndKeys:
      chatId, @"chat_id", js, @"js", nil];
    [self postMessageJS:info];
  }
  @synchronized(pendingSends_) {
    queue_pending_temp(pendingSendOrderByChat_, chatId, tempId);
  }

  NSDictionary *params = [NSDictionary dictionaryWithObjectsAndKeys:
    stickerId,  @"sticker_id",
    packageId,  @"package_id",
    chatId,     @"chat_id",
    tempId,     @"temp_id",
    nil];
  [NSThread detachNewThreadSelector:@selector(sendStickerInBackground:)
                           toTarget:self
                         withObject:params];
  return YES;
}

- (void)sendStickerInBackground:(NSDictionary *)params;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  NSString *stickerId   = [params objectForKey:@"sticker_id"];
  NSString *packageId   = [params objectForKey:@"package_id"];
  NSString *chatId      = [params objectForKey:@"chat_id"];
  NSString *tempId      = [params objectForKey:@"temp_id"];
  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  char *messageId = NULL;
  int ok = enil_account_send_sticker(health_, db_,
                                     [sessionPath fileSystemRepresentation],
                                     [chatId UTF8String],
                                     [stickerId UTF8String],
                                     [packageId UTF8String],
                                     &messageId);

  if (ok) {
    NSString *realId = [NSString stringWithUTF8String:messageId];
    [self confirmTempId:tempId realMessageId:messageId];
    [self markChatSeen:chatId messageId:realId];
  } else {
    [self failTempId:tempId chatId:chatId];
  }
  enil_account_free(messageId);

  [pool release];
}

- (BOOL)sendImageAtPath:(NSString *)path toChatId:(NSString *)chatId;
{
  if (!path || ![path length] || !chatId || ![chatId length]) return NO;
  NSFileManager *fm = [NSFileManager defaultManager];
  if (![fm fileExistsAtPath:path]) {
    ENIL_LOG("ENILAccount.sendImage", "file not found: %s", [path UTF8String]);
    return NO;
  }

  enil_status_post("send.message", "Sending image", 1);

  long long nowMs = (long long)([[NSDate date] timeIntervalSince1970] * 1000.0);
  NSString *tempId = [NSString stringWithFormat:@"local-%lld", nowMs];

  NSString *chatDir = [NSString stringWithFormat:@"media/%@", chatId];
  NSString *chatDirAbs = [enilDir_ stringByAppendingPathComponent:chatDir];
  if (![fm XP_createDirectoryAtPath:chatDirAbs
        withIntermediateDirectories:YES attributes:nil error:NULL]) {
    ENIL_LOG("ENILAccount.sendImage", "cannot create media dir");
    return NO;
  }

  NSString *mediaRel = [NSString stringWithFormat:@"media/%@/%@.jpg",
                        chatId, tempId];
  NSString *thumbRel = [NSString stringWithFormat:@"media/%@/%@_t.jpg",
                        chatId, tempId];
  NSString *mediaAbs = [enilDir_ stringByAppendingPathComponent:mediaRel];
  NSString *thumbAbs = [enilDir_ stringByAppendingPathComponent:thumbRel];

  [fm XP_removeItemAtPath:mediaAbs error:NULL];

  int origW = 0, origH = 0, thumbW = 0, thumbH = 0;
  if (!enil_account_prepare_image([path fileSystemRepresentation],
                                  [mediaAbs fileSystemRepresentation],
                                  [thumbAbs fileSystemRepresentation],
                                  &origW, &origH, &thumbW, &thumbH)) {
    [fm XP_removeItemAtPath:mediaAbs error:NULL];
    return NO;
  }

  NSString *senderName = [self displayNameForOutgoingMessage];
  char *jsRaw = enil_html_js_append_image([tempId UTF8String],
                                          [senderName UTF8String],
                                          [mediaRel UTF8String],
                                          [thumbRel UTF8String],
                                          origW, origH,
                                          thumbW, thumbH,
                                          nowMs);
  NSString *js = jsRaw ? [NSString stringWithUTF8String:jsRaw] : nil;
  free(jsRaw);
  if (js) {
    NSDictionary *info2 = [NSDictionary dictionaryWithObjectsAndKeys:
      chatId, @"chat_id", js, @"js", nil];
    [self postMessageJS:info2];
  }
  @synchronized(pendingSends_) {
    queue_pending_temp(pendingSendOrderByChat_, chatId, tempId);
  }

  NSDictionary *params = [NSDictionary dictionaryWithObjectsAndKeys:
    chatId,                                @"chat_id",
    tempId,                                @"temp_id",
    mediaAbs,                              @"media_abs",
    thumbAbs,                              @"thumb_abs",
    [NSNumber numberWithInt:origW],        @"orig_w",
    [NSNumber numberWithInt:origH],        @"orig_h",
    [NSNumber numberWithInt:thumbW],       @"thumb_w",
    [NSNumber numberWithInt:thumbH],       @"thumb_h",
    nil];
  [NSThread detachNewThreadSelector:@selector(sendImageInBackground:)
                           toTarget:self
                         withObject:params];
  return YES;
}

- (void)sendImageInBackground:(NSDictionary *)params;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  NSString *chatId   = [params objectForKey:@"chat_id"];
  NSString *tempId   = [params objectForKey:@"temp_id"];
  NSString *mediaAbs = [params objectForKey:@"media_abs"];
  NSString *thumbAbs = [params objectForKey:@"thumb_abs"];
  int origW   = [[params objectForKey:@"orig_w"]  intValue];
  int origH   = [[params objectForKey:@"orig_h"]  intValue];
  int thumbW  = [[params objectForKey:@"thumb_w"] intValue];
  int thumbH  = [[params objectForKey:@"thumb_h"] intValue];

  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];

  NSData *jpegData  = [NSData dataWithContentsOfFile:mediaAbs];
  NSData *thumbData = thumbAbs ? [NSData dataWithContentsOfFile:thumbAbs] : nil;
  if (![jpegData length]) {
    ENIL_LOG("ENILAccount.sendImage", "cannot read JPEG bytes: %s", [mediaAbs UTF8String]);
    [self failTempId:tempId chatId:chatId];
    [pool release];
    return;
  }

  enil_account_image_send_t sp;
  memset(&sp, 0, sizeof(sp));
  sp.jpeg_data    = (const unsigned char *)[jpegData bytes];
  sp.jpeg_len     = (size_t)[jpegData length];
  sp.orig_width   = origW;
  sp.orig_height  = origH;
  sp.thumb_data   = thumbData ? (const unsigned char *)[thumbData bytes] : NULL;
  sp.thumb_len    = thumbData ? (size_t)[thumbData length] : 0;
  sp.thumb_width  = thumbW;
  sp.thumb_height = thumbH;

  char *messageId = NULL;
  int ok = enil_account_send_image(health_, db_,
                                   [sessionPath fileSystemRepresentation],
                                   [chatId UTF8String], &sp, &messageId);

  if (ok) {
    /* Rename optimistic files to the real message id so the renderer finds
     * them after the next refresh, and stamp the paths into messages_v2. */
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *chatDir = [NSString stringWithFormat:@"media/%@", chatId];
    NSString *realRel = [NSString stringWithFormat:@"%@/%s.jpg",
                         chatDir, messageId];
    NSString *thumbRealRel = [NSString stringWithFormat:@"%@/%s_t.jpg",
                              chatDir, messageId];
    NSString *realAbs      = [enilDir_ stringByAppendingPathComponent:realRel];
    NSString *thumbRealAbs = [enilDir_ stringByAppendingPathComponent:thumbRealRel];

    [fm XP_removeItemAtPath:realAbs       error:NULL];
    [fm XP_removeItemAtPath:thumbRealAbs  error:NULL];
    [fm XP_copyItemAtPath:mediaAbs toPath:realAbs error:NULL];
    if (thumbAbs)
      [fm XP_copyItemAtPath:thumbAbs toPath:thumbRealAbs error:NULL];

    enil_db_set_media_info(db_, messageId,
                           [realRel UTF8String], origW, origH,
                           [thumbRealRel UTF8String], thumbW, thumbH);

    NSString *realId = [NSString stringWithUTF8String:messageId];
    [self confirmTempId:tempId realMessageId:messageId];
    [self markChatSeen:chatId messageId:realId];
  } else {
    [self failTempId:tempId chatId:chatId];
  }
  enil_account_free(messageId);

  [pool release];
}

- (BOOL)markChatSeen:(NSString *)chatId messageId:(NSString *)messageId;
{
  if (!chatId || !messageId) return NO;
  /* The DB update (enil_db_message_box_mark_seen) is NOT done here.
   * Writing to the DB optimistically creates a split source of truth:
   * the mark-as-seen button in the message pane reads the fresh DB
   * (unreadCount == 0) and disables, but the chat list caches its
   * rows_ and only rebuilds them on sync/SSE notifications — so the
   * blue unread dot survives until the SSE push arrives.  By skipping
   * the local write we keep the single source of truth (the SSE push)
   * and just disable the button in memory until that push confirms.
   *
   * Move the gray "Seen" marker to the just-seen message in place: marking seen
   * advances the seen position to `messageId` (now the newest), and the Seen
   * line is always shown — even when caught up — so it re-anchors at the bottom
   * below that row rather than clearing. Posted on the main thread because this
   * method is also reachable from background send-confirm paths;
   * MessageListViewController filters by the active chat, so it's a no-op when
   * this chat isn't on screen. */
  {
    char *seenRaw = enil_html_js_set_seen_marker([messageId UTF8String]);
    NSString *seenJS = seenRaw ? [NSString stringWithUTF8String:seenRaw] : nil;
    free(seenRaw);
    if (seenJS) {
      NSDictionary *seenInfo = [NSDictionary dictionaryWithObjectsAndKeys:
        chatId, @"chat_id", seenJS, @"js", nil];
      [self performSelectorOnMainThread:@selector(postMessageJS:)
                             withObject:seenInfo
                          waitUntilDone:NO];
    }
  }

  NSDictionary *params = [NSDictionary dictionaryWithObjectsAndKeys:
    chatId,    @"chat_id",
    messageId, @"message_id",
    nil];
  [NSThread detachNewThreadSelector:@selector(markChatSeenInBackground:)
                           toTarget:self withObject:params];
  return YES;
}

- (void)markChatSeenInBackground:(NSDictionary *)params;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  NSString *chatId    = [params objectForKey:@"chat_id"];
  NSString *messageId = [params objectForKey:@"message_id"];
  const char *token = accessToken_ ? [accessToken_ UTF8String] : NULL;
  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  enil_account_mark_chat_seen(health_, [sessionPath fileSystemRepresentation], token,
                              [chatId UTF8String],
                              [messageId UTF8String]);
  [pool release];
}

- (BOOL)markChatSeenAtServerHead:(NSString *)chatId;
{
  if (!chatId) return NO;
  char *raw = enil_db_message_box_last_delivered(db_, [chatId UTF8String]);
  if (!raw) {
    ENIL_LOG("ENILAccount.markChatSeenAtServerHead", "no lastDeliveredMessageId for %s", [chatId UTF8String]);
    return NO;
  }
  NSString *msgId = [NSString stringWithUTF8String:raw];
  free(raw);
  return [self markChatSeen:chatId messageId:msgId];
}

- (int)unreadCountForChatId:(NSString *)chatId;
{
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return 0;
  /* Single source of truth: the same chat-summary rows that drive the
     unread dot in the chat list cell (mb.unreadCount via kSQL_GetChatSummaries). */
  NSArray *summaries = [self chats];
  NSUInteger i;
  for (i = 0; i < [summaries count]; i++) {
    NSDictionary *s = [summaries objectAtIndex:i];
    if ([chatId isEqualToString:[s objectForKey:@"chatMid"]]) {
      int n = [[s objectForKey:@"unreadCount"] intValue];
      return n > 0 ? n : 0;
    }
  }
  return 0;
}

- (int)totalUnreadCount;
{
  /* Same chat-summary rows as -unreadCountForChatId:, summed. One query path
     keeps the badge in lock-step with the per-chat unread dots. */
  NSArray *summaries = [self chats];
  int total = 0;
  NSUInteger i;
  for (i = 0; i < [summaries count]; i++) {
    int n = [[[summaries objectAtIndex:i] objectForKey:@"unreadCount"] intValue];
    if (n > 0) total += n;
  }
  return total;
}

- (long long)lastDeliveredTimeForChatId:(NSString *)chatId;
{
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return 0;
  /* Same chat-summary join (kSQL_GetChatSummaries already selects
     mb.lastDeliveredTime); reusing it keeps one query path. */
  NSArray *summaries = [self chats];
  NSUInteger i;
  for (i = 0; i < [summaries count]; i++) {
    NSDictionary *s = [summaries objectAtIndex:i];
    if ([chatId isEqualToString:[s objectForKey:@"chatMid"]]) {
      long long t = [[s objectForKey:@"lastDeliveredTime"] longLongValue];
      return t > 0 ? t : 0;
    }
  }
  return 0;
}

- (BOOL)removeChat:(NSString *)chatId
   lastReadMessageId:(NSString *)lastReadMessageId
 lastReadMessageTime:(long long)lastReadMessageTime;
{
  if (!chatId) return NO;
  NSDictionary *params = [NSDictionary dictionaryWithObjectsAndKeys:
    chatId, @"chat_id",
    lastReadMessageId ? lastReadMessageId : @"", @"last_read_id",
    [NSNumber numberWithLongLong:lastReadMessageTime], @"last_read_time",
    nil];
  [NSThread detachNewThreadSelector:@selector(removeChatInBackground:)
                           toTarget:self withObject:params];
  return YES;
}

- (void)removeChatInBackground:(NSDictionary *)params;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  NSString *chatId       = [params objectForKey:@"chat_id"];
  NSString *lastReadId   = [params objectForKey:@"last_read_id"];
  long long lastReadTime = [[params objectForKey:@"last_read_time"] longLongValue];
  NSString *sessionPath  = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  const char *lastReadC  = ([lastReadId length] > 0) ? [lastReadId UTF8String] : NULL;
  enil_account_remove_chat(health_, db_,
                           [sessionPath fileSystemRepresentation],
                           [chatId UTF8String],
                           lastReadC, lastReadTime);
  [pool release];
}

- (BOOL)deleteChatLocally:(NSString *)chatId;
{
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return NO;
  if (!db_) return NO;
  if (enil_db_chat_delete(db_, [chatId UTF8String]) != 0) {
    ENIL_LOG("ENILAccount.deleteChatLocally", "failed for %s", [chatId UTF8String]);
    return NO;
  }
  return YES;
}

- (NSString *)enilDir;     { return enilDir_; }

- (NSURL *)fileURLForPurchasesPackage:(NSString *)packageId asset:(NSString *)filename;
{
  NSString *path = [NSString stringWithFormat:@"%@/purchases/%@/%@",
      enilDir_, packageId, filename];
  return [NSURL fileURLWithPath:path];
}

- (NSString *)accessToken; { return accessToken_; }

- (void)postSSEEvent:(NSDictionary *)info;
{
  [[NSNotificationCenter defaultCenter]
    postNotificationName:ENILSSEEventNotification
                  object:self
                userInfo:info];
}

/* ---- SSE burst coalescing -------------------------------------------------
 * A push burst can deliver many events back-to-back. Emitting each one's UI
 * work live (a synchronous WebView JS eval + a notification fan-out + a
 * chat-list row reload, per message) saturates the run loop and the app feels
 * frozen for the length of the stream. The SSE callback instead funnels its
 * two main-thread emissions HERE, where they buffer and a single timer flushes
 * the whole batch at once: one concatenated WebView eval per chat, the events
 * replayed together. The run loop stays free WHILE messages stream in.
 *
 * The timer is a RESETTING debounce: each new item pushes the flush out to
 * kCoalesceIdle from now, so a backlog flushes once it has been quiet for
 * kCoalesceIdle — not sliced into chunks by a fixed window. This matters
 * because messages are fed in slowly: enil_account_process_sse_event blocks the
 * SSE thread on a per-message worker decrypt + asset/metadata fetch, so a
 * 10-message backlog trickles in over several seconds. A fixed (non-resetting)
 * window would fire mid-trickle and split the backlog across several repaints.
 *
 * kCoalesceMaxWait caps the debounce: a batch can never be held longer than
 * this from its first item, so an unending live stream (items always <idle
 * apart) still repaints periodically instead of starving. The "Received
 * message" mini-bar row fires per message regardless, so the user always sees
 * liveness even while the chat repaint is batched.
 *
 * Only the SSE path is coalesced. The user's own sends still call
 * -postMessageJS: directly, so an outgoing bubble appears instantly. */
static const NSTimeInterval kCoalesceIdle    = 1.0; /* flush this long after the LAST event */
static const NSTimeInterval kCoalesceMaxWait = 8.0; /* ...but never hold a batch longer than this */

- (void)coalesceMessageJS:(NSDictionary *)info;
{
  if (!info) return;
  if (!coalescedJS_) coalescedJS_ = [[NSMutableArray alloc] init];
  [coalescedJS_ addObject:info];
  [self bumpCoalesceTimer];
}

- (void)coalesceSSEEvent:(NSDictionary *)info;
{
  if (!info) return;
  if (!coalescedSSE_) coalescedSSE_ = [[NSMutableArray alloc] init];
  [coalescedSSE_ addObject:info];
  [self bumpCoalesceTimer];
}

/* (Re)schedule the flush kCoalesceIdle out, clamped so it never lands later
 * than coalesceDeadline_ (first-item + kCoalesceMaxWait). */
- (void)bumpCoalesceTimer;
{
  NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
  NSTimeInterval delay = kCoalesceIdle;
  if (coalesceDeadline_ == 0.0) coalesceDeadline_ = now + kCoalesceMaxWait;
  if (now + delay > coalesceDeadline_) delay = coalesceDeadline_ - now;
  if (delay < 0.0) delay = 0.0;
  [coalesceTimer_ invalidate];
  [coalesceTimer_ release];
  coalesceTimer_ = [[NSTimer scheduledTimerWithTimeInterval:delay
                                                     target:self
                                                   selector:@selector(flushCoalescedUI:)
                                                   userInfo:nil
                                                    repeats:NO] retain];
}

- (void)flushCoalescedUI:(NSTimer *)timer;
{
  (void)timer;
  [coalesceTimer_ invalidate];
  [coalesceTimer_ release];
  coalesceTimer_ = nil;
  coalesceDeadline_ = 0.0;
  [self flushCoalescedJS];
  [self flushCoalescedSSE];
}

/* Concatenate every buffered snippet per chat (first-seen order preserved, so
 * an append still precedes its later seen-marker / reaction update) and post
 * one ENILMessageJSNotification per chat — collapsing N synchronous WebView
 * evals into one per chat. */
- (void)flushCoalescedJS;
{
  NSMutableArray *order;       /* chat_ids, first-seen order */
  NSMutableDictionary *byChat; /* chat_id -> NSMutableString of joined js */
  NSUInteger i, n;
  if (![coalescedJS_ count]) return;
  order  = [NSMutableArray array];
  byChat = [NSMutableDictionary dictionary];
  n = [coalescedJS_ count];
  for (i = 0; i < n; i++) {
    NSDictionary    *entry = [coalescedJS_ objectAtIndex:i];
    NSString        *cid   = [entry objectForKey:@"chat_id"];
    NSString        *js    = [entry objectForKey:@"js"];
    NSMutableString *acc;
    if (![cid length] || ![js length]) continue;
    acc = [byChat objectForKey:cid];
    if (!acc) {
      acc = [NSMutableString string];
      [byChat setObject:acc forKey:cid];
      [order addObject:cid];
    }
    [acc appendString:js];
    [acc appendString:@"\n"]; /* statement separator; ASI terminates each call */
  }
  n = [order count];
  for (i = 0; i < n; i++) {
    NSString *cid = [order objectAtIndex:i];
    [self postMessageJS:[NSDictionary dictionaryWithObjectsAndKeys:
                          cid, @"chat_id", [byChat objectForKey:cid], @"js", nil]];
  }
  [coalescedJS_ removeAllObjects];
}

/* Replay the buffered events in arrival order. Each observer's contract is
 * preserved exactly (per-message banners, per-chat list reseats) — the only
 * change is they now run as one batch after the window instead of interleaved
 * with the incoming stream. */
- (void)flushCoalescedSSE;
{
  NSUInteger i, n;
  if (![coalescedSSE_ count]) return;
  n = [coalescedSSE_ count];
  for (i = 0; i < n; i++)
    [self postSSEEvent:[coalescedSSE_ objectAtIndex:i]];
  [coalescedSSE_ removeAllObjects];
}

/* text/sticon (0) and sticker (7) only; nil for every other content type. The
 * row is owned by the caller (not freed here). */
- (NSDictionary *)notificationDictFromMessageRow:(cJSON *)row;
{
  cJSON *ctItem = cJSON_GetObjectItem(row, "content_type");
  if (!cJSON_IsNumber(ctItem)) return nil;
  int ct = ctItem->valueint;
  if (ct != 0 && ct != 7) return nil;

  const char *nameC = cJSON_GetStringValue(cJSON_GetObjectItem(row, "sender_name"));
  const char *textC = cJSON_GetStringValue(cJSON_GetObjectItem(row, "text"));

  NSString *title = nameC ? [NSString stringWithUTF8String:nameC] : nil;
  if ([title length] == 0) title = @"ENIL";

  NSString *body = nil;
  if (ct == 7) body = NSLocalizedString(@"Sticker", nil);
  else if (textC) body = [NSString stringWithUTF8String:textC];
  if ([body length] == 0) body = NSLocalizedString(@"New message", nil);

  return [NSDictionary dictionaryWithObjectsAndKeys:title, @"title", body, @"body", nil];
}

- (NSDictionary *)notificationForSSEMessageData:(NSString *)data;
{
  enil_account_sse_message_info_t mi;
  cJSON *row;
  NSDictionary *out;

  if ([data length] == 0) return nil;

  /* Fills `mi` from the out-struct; its return value is truthy-on-success
   * (1 = IDs extracted), so don't gate on it — the field checks below cover a
   * failed parse (everything stays zeroed). Mirrors sse_event_cb's usage. */
  memset(&mi, 0, sizeof(mi));
  enil_account_sse_message_info([data UTF8String], &mi);
  if (mi.op_type != ENIL_OP_RECEIVE_MESSAGE || !mi.has_message || !mi.message_id[0])
    return nil;

  row = enil_db_get_message_by_id(db_, mi.message_id);
  if (!row) return nil;

  out = [self notificationDictFromMessageRow:row];
  cJSON_Delete(row);
  return out;
}

static NSString *pending_temp_id_for_sse_message(ENILAccount *account,
                                                 const enil_account_sse_message_info_t *msgInfo)
{
  NSString *tempId = nil;

  if (!account || !msgInfo) return nil;

  if (msgInfo->message_id[0]) {
    NSString *realId = [NSString stringWithUTF8String:msgInfo->message_id];
    @synchronized(account->pendingSends_) {
      tempId = [[[account->pendingSends_ objectForKey:realId] retain] autorelease];
      if (tempId) {
        [account->pendingSends_ removeObjectForKey:realId];
        remove_pending_temp(account->pendingSendOrderByChat_, tempId);
      }
    }
  }

  if (!tempId && account->myMid_ && msgInfo->from_mid[0] &&
      strcmp(msgInfo->from_mid, [account->myMid_ UTF8String]) == 0) {
    NSString *chatKey = nil;
    if (msgInfo->to_type == 0 && msgInfo->to_mid[0])
      chatKey = [NSString stringWithUTF8String:msgInfo->to_mid];
    else if (msgInfo->to_type == 2 && msgInfo->to_mid[0])
      chatKey = [NSString stringWithUTF8String:msgInfo->to_mid];
    if (chatKey) {
      @synchronized(account->pendingSends_) {
        tempId = pop_pending_temp_for_chat(account->pendingSendOrderByChat_, chatKey);
      }
    }
  }

  return tempId;
}

static void sse_event_cb(const ENILSSEEvent *ev, void *ctx)
{
  ENILAccount *account = (ENILAccount *)ctx;
  NSAutoreleasePool *pool;
  NSString *type;
  NSString *data;
  NSString *sessionPath;
  NSDictionary *info;
  NSString *tempId = nil;
  const char *my_mid;
  enil_account_sse_result_t result;
  int i;

  pool = [[NSAutoreleasePool alloc] init];
  type = [NSString stringWithUTF8String:ev->type];
  data = [NSString stringWithUTF8String:ev->data];
  sessionPath = [account->enilDir_ stringByAppendingPathComponent:@"session.json"];
  my_mid = account->myMid_ ? [account->myMid_ UTF8String] : NULL;

  if (strcmp(ev->type, "message") == 0) {
    enil_account_sse_message_info_t msgInfo;
    memset(&msgInfo, 0, sizeof(msgInfo));
    enil_account_sse_message_info(ev->data, &msgInfo);
    tempId = pending_temp_id_for_sse_message(account, &msgInfo);
  }

  enil_account_process_sse_event(account->health_,
                                 account->db_,
                                 account->accessToken_ ?
                                   [account->accessToken_ UTF8String] : NULL,
                                 my_mid,
                                 [sessionPath fileSystemRepresentation],
                                 ev->type,
                                 ev->data,
                                 tempId ? [tempId UTF8String] : NULL,
                                 &result);

  if (result.should_stop_sse) {
    [account performSelectorOnMainThread:@selector(stopSSE)
                              withObject:nil
                           waitUntilDone:NO];
  }

  for (i = 0; i < result.js_update_count; i++) {
    enil_account_sse_js_update_t *update = &result.js_updates[i];
    if (!update->chat_id || !update->js) continue;
    NSString *chatId = [NSString stringWithUTF8String:update->chat_id];
    NSString *js     = [NSString stringWithUTF8String:update->js];
    NSDictionary *jsInfo = [NSDictionary dictionaryWithObjectsAndKeys:
      chatId, @"chat_id", js, @"js", nil];
    [account performSelectorOnMainThread:@selector(coalesceMessageJS:)
                              withObject:jsInfo
                           waitUntilDone:NO];
  }

  if (result.talk_action != ENIL_ACCOUNT_SSE_TALK_ACTION_NONE) {
    NSDictionary *talkInfo = [NSDictionary dictionaryWithObjectsAndKeys:
      [NSNumber numberWithInt:result.talk_exception_code], @"code",
      [NSNumber numberWithInt:(int)result.talk_action],    @"action",
      nil];
    [account performSelectorOnMainThread:@selector(handleSSETalkExceptionResult:)
                              withObject:talkInfo
                           waitUntilDone:NO];
  }

  if (result.should_start_sync) {
    [account performSelectorOnMainThread:@selector(startSync)
                              withObject:nil
                           waitUntilDone:NO];
  }

  /* Carry the affected chat id (when this event moved a chat's order/unread) so
   * the chat-list observer can update one row instead of rebuilding the whole
   * roster. Empty affected_chat_id => no list-relevant change; key is omitted. */
  if (result.affected_chat_id && result.affected_chat_id[0]) {
    info = [NSDictionary dictionaryWithObjectsAndKeys:
      type, @"type",
      data, @"data",
      [NSString stringWithUTF8String:result.affected_chat_id], @"chat_id",
      nil];
  } else {
    info = [NSDictionary dictionaryWithObjectsAndKeys:
      type, @"type",
      data, @"data",
      nil];
  }
  [account performSelectorOnMainThread:@selector(coalesceSSEEvent:)
                            withObject:info
                         waitUntilDone:NO];

  enil_account_sse_result_free(&result);
  [pool release];
}

- (void)startSSE;
{
  NSString *sessionPath;
  long long localRev;
  if (sseClient_) {
    enil_sse_free(sseClient_);
    sseClient_ = NULL;
  }
  /* The user's persisted "link off" choice wins over every reconnect trigger
   * (launch, post-sync, wake). Reflect it as Offline and make no connection. */
  if (!sseEnabled_) {
    ENILLog(@"ENILAccount.startSSE", @"disabled by user; not starting");
    [self setSyncState:ENILSyncStateOffline];
    return;
  }
  if (!accessToken_) {
    [self setSyncState:ENILSyncStateNeverSynced];
    return;
  }
  sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  /* Checked before localRev so a dead-token account doesn't masquerade as
     "never synced". Cleared implicitly when reauth replaces session.json. */
  if (enil_session_has_reauth_needed([sessionPath fileSystemRepresentation])) {
    ENILLog(@"ENILAccount.startSSE", @"reauth needed; not starting");
    [self setSyncState:ENILSyncStateReauthNeeded];
    return;
  }
  if (!enil_db_has_local_rev(db_)) {
    ENILLog(@"ENILAccount.startSSE", @"localRev missing; not starting");
    [self setSyncState:ENILSyncStateNeverSynced];
    return;
  }
  localRev = enil_db_get_local_rev(db_);
  sseClient_ = enil_sse_create([accessToken_ UTF8String], localRev,
                               [sessionPath fileSystemRepresentation], db_, health_);
  if (!sseClient_) {
    [self setSyncState:ENILSyncStateSyncNeeded];
    return;
  }
  if (!enil_sse_start(sseClient_, sse_event_cb, self)) {
    enil_sse_free(sseClient_);
    sseClient_ = NULL;
    [self setSyncState:ENILSyncStateError];
    return;
  }
  [self setSyncState:ENILSyncStateLive];
  ENILLog(@"ENILAccount.startSSE", @"started localRev=%lld", localRev);
}

- (void)stopSSE;
{
  if (!sseClient_) return;
  enil_sse_free(sseClient_);
  sseClient_ = NULL;
  ENILLog(@"ENILAccount.stopSSE", @"stopped");
}

/* Instant recovery hook for system wake / network change. If a stream is
 * live, kick it (non-blocking — no pthread_join on the caller's thread, so
 * this is safe to call from the main thread on wake). If nothing is running,
 * fall back to a normal start, which re-applies the reauth/localRev gates. */
- (void)forceReconnectSSE;
{
  if (sseClient_) {
    ENILLog(@"ENILAccount.forceReconnectSSE", @"kicking live stream");
    enil_sse_kick(sseClient_);
  } else {
    ENILLog(@"ENILAccount.forceReconnectSSE", @"no live stream; starting");
    [self startSSE];
  }
}

- (BOOL)isSSEEnabled;
{
  return sseEnabled_ ? YES : NO;
}

- (void)setSSEEnabled:(BOOL)enabled;
{
  NSString *sessionPath;
  if ((sseEnabled_ != 0) == (enabled != NO)) return;  /* no-op on no change */
  sseEnabled_ = enabled ? 1 : 0;
  sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  enil_session_set_sse_enabled([sessionPath fileSystemRepresentation], sseEnabled_);
  ENILLog(@"ENILAccount.setSSEEnabled", @"%@", enabled ? @"enabled" : @"disabled");
  /* -startSSE re-reads the gate and applies the localRev/reauth checks; the
   * disable path stops the stream and announces Offline so the bar stops
   * claiming "Syncing Live". */
  if (enabled) {
    [self startSSE];
  } else {
    [self stopSSE];
    [self setSyncState:ENILSyncStateOffline];
  }
}

- (BOOL)startWithError:(NSError **)outError;
{
  NSString *cacert = [AltivecCore certPath];
  NSParameterAssert(cacert);
  enil_cainfo_set([cacert fileSystemRepresentation]);

  if (![[NSFileManager defaultManager] XP_createDirectoryAtPath:enilDir_
                                    withIntermediateDirectories:YES
                                                     attributes:nil
                                                          error:outError]) {
    return NO;
  }

  NSString *sessionPath = [enilDir_ stringByAppendingPathComponent:@"session.json"];
  char *access_token = NULL, *mid = NULL;
  int e2eeRecovered = 0;
  enil_account_start_result_t startResult =
    enil_account_start_core([enilDir_ fileSystemRepresentation], &db_,
                            &access_token, &mid, &sseEnabled_,
                            &e2eeRecovered);
  if (startResult == ENIL_ACCOUNT_START_INVALID_SESSION) {
    if (outError) *outError = makeError(ENILErrorInvalidSession,
      [NSString stringWithFormat:@"session.json is missing or invalid at: %@", sessionPath]);
    return NO;
  }
  if (startResult == ENIL_ACCOUNT_START_DB_OPEN) {
    if (outError) *outError = makeError(ENILErrorDirectoryCreate, @"Cannot open database");
    return NO;
  }
  if (startResult == ENIL_ACCOUNT_START_DB_CREATE) {
    if (outError) *outError = makeError(ENILErrorDirectoryCreate, @"Cannot create database tables");
    return NO;
  }

  [accessToken_ release];
  accessToken_ = [[NSString alloc] initWithUTF8String:access_token];
  free(access_token);
  if (mid) {
    [myMid_ release];
    myMid_ = [[NSString alloc] initWithUTF8String:mid];
    free(mid);
  }

  /* Self-heal: if a prior login captured tokens but not the E2EE keychain
     (capture_e2ee_keys failed yet persisted the raw metaData), recover it
     offline now — no rate-limited re-login. No-op when keys already exist. */
  if (!e2eeRecovered) {
    ENILLog(@"ENILAccount.start", @"E2EE keychain absent and not offline-"
          @"recoverable — encrypted messages will not decrypt until re-login");
  }

  [[NSNotificationCenter defaultCenter] addObserver:self
    selector:@selector(syncDidFinish:)
        name:ENILSyncDidFinishNotification
      object:nil];

  /* Arm the proactive refresh timer based on whatever's already in session.json.
   * If the session is stale and the eta is in the past, startSync's own refresh
   * will overwrite it before this fires; if not, the timer takes care of it. */
  [self scheduleNextTokenRefresh];

  ENILLog(@"ENILAccount.start", @"ready - dir: %@",
          enilDir_);
  return YES;
}

- (BOOL)isOneToOneChatId:(NSString *)chatId;
{
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return NO;
  return enil_account_is_one_to_one_mid([chatId UTF8String]) ? YES : NO;
}

+ (NSData *)qrModulesForString:(NSString *)string size:(int *)outSize;
{
  unsigned char *raw;
  int size;
  if (outSize) *outSize = 0;
  if (![string isKindOfClass:[NSString class]] || ![string length]) return nil;
  raw = enil_account_qr_modules_for_string([string UTF8String], &size);
  if (!raw) return nil;
  if (outSize) *outSize = size;
  return [NSData dataWithBytesNoCopy:raw
                               length:(NSUInteger)(size * size)
                         freeWhenDone:YES];
}

+ (BOOL)prepareQRLoginAtPath:(NSString *)accountDir
               clientProfile:(NSString *)profile
         reauthenticatingMid:(NSString *)mid;
{
  return enil_login_store_prepare([accountDir fileSystemRepresentation],
    [profile UTF8String], [mid length] ? [mid UTF8String] : NULL) ? YES : NO;
}

+ (BOOL)canRestartQRLoginAtPath:(NSString *)accountDir;
{
  return enil_login_store_can_restart([accountDir fileSystemRepresentation]) ? YES : NO;
}

+ (BOOL)restartQRLoginAtPath:(NSString *)accountDir;
{
  return enil_login_store_restart([accountDir fileSystemRepresentation]) ? YES : NO;
}

+ (BOOL)activateQRLoginAtPath:(NSString *)stagingDir accountPath:(NSString *)accountDir;
{
  return enil_login_store_activate([stagingDir fileSystemRepresentation],
                                    [accountDir fileSystemRepresentation]) ? YES : NO;
}

+ (BOOL)runQRLoginAtPath:(NSString *)accountDir
                observer:(id <ENILQRLoginObserver>)observer
              cancelFlag:(const volatile int *)cancelFlag;
{
  if (![accountDir isKindOfClass:[NSString class]] || ![accountDir length]) return NO;

  enil_qrlogin_callbacks_t cb;
  cb.on_qr_url = qr_url_trampoline;
  cb.on_pin    = qr_pin_trampoline;
  cb.on_status = qr_status_trampoline;
  cb.ctx       = observer;
  cb.cancel    = cancelFlag;

  NSString *path = [accountDir stringByAppendingPathComponent:@"session.json"];
  if (enil_session_bind_identity([path fileSystemRepresentation])) {
    const enil_identity_t *identity = enil_identity_current();
    if (identity && !strcmp(identity->transport, "native-thrift")) {
      char *durable = enil_login_store_select([accountDir fileSystemRepresentation]);
      if (!durable) return NO;
      BOOL ok = enil_native_login_run(durable, &cb) &&
        enil_qrlogin_recover_e2ee(durable) &&
        enil_login_store_stage([accountDir fileSystemRepresentation], durable);
      free(durable);
      enil_identity_bind(NULL);
      return ok;
    }
  }
  return enil_qrlogin_run([accountDir fileSystemRepresentation], &cb) ? YES : NO;
}

+ (BOOL)workerFailed     { return enil_account_worker_failed() ? YES : NO; }
+ (BOOL)lineFailed       { return enil_account_line_failed()   ? YES : NO; }
+ (BOOL)anyServiceFailed { return enil_account_any_service_failed() ? YES : NO; }

+ (void)bootstrapProcessWithCACertPath:(NSString *)caCertPath;
{
  if (![caCertPath isKindOfClass:[NSString class]] || ![caCertPath length]) {
    [NSException raise:NSInvalidArgumentException
                format:@"[ENILAccount bootstrapProcessWithCACertPath:] caCertPath is required"];
  }

  /* Fonts dir for the renderer: chat/picker pages are rooted at the per-account
   * enilDir, so @font-face needs an absolute file:// URL into the bundle. */
  NSString *fonts = [[[NSBundle mainBundle] resourcePath]
    stringByAppendingPathComponent:@"Fonts"];
  enil_html_set_font_dir([fonts fileSystemRepresentation]);

  /* curl CA bundle is process-global and must be set before ANY network call. */
  enil_cainfo_set([caCertPath fileSystemRepresentation]);

  /* LINE Accept-Language / X-LAL track the lproj NSBundle actually picked for
   * NSLocalizedString (via -preferredLocalizations), not raw system locale —
   * single source of truth so headers never desync from displayed text. */
  NSArray *prefs = [[NSBundle mainBundle] preferredLocalizations];
  NSString *lang = [prefs count] ? [prefs objectAtIndex:0] : @"en";
  const char *acceptLang = "en-US";
  const char *xLal       = "en_US";
  if ([lang isEqualToString:@"ja"]) {
    acceptLang = "ja-JP";
    xLal       = "ja_JP";
  }
  ENILLog(@"ENILAccount.bootstrapProcess",
          @"fonts=%@ language: preferred=%@ accept=%s x-lal=%s",
          fonts, lang, acceptLang, xLal);
  enil_line_set_language(acceptLang, xLal);
}

+ (void)syncWorkerCredentialsFromKeychain;
{
  ENILKeychain *kc = [ENILKeychain sharedKeychain];
  NSString *url    = [kc workerURL];
  NSString *secret = [kc workerSecret];
  enil_worker_set_credentials(url    ? [url    UTF8String] : NULL,
                              secret ? [secret UTF8String] : NULL);
}

+ (NSString *)validatedMidForSessionAtPath:(NSString *)path;
{
  if (![path isKindOfClass:[NSString class]] || ![path length]) return nil;
  char *mid;
  NSString *result = nil;
  mid = enil_account_validated_mid_for_session([path fileSystemRepresentation]);
  if (mid) {
    result = [NSString stringWithUTF8String:mid];
  }
  enil_account_free(mid);
  return result;
}

+ (NSString *)displayNameForSessionAtPath:(NSString *)path;
{
  if (![path isKindOfClass:[NSString class]] || ![path length]) return nil;
  char *name = enil_account_display_name_for_session([path fileSystemRepresentation]);
  NSString *result = name ? [NSString stringWithUTF8String:name] : nil;
  enil_account_free(name);
  return result;
}

@end
