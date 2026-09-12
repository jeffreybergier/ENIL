#import "ENILSyncStatusQueue.h"
#import "XPFoundation.h"   /* XPRunLoopCommonModes (Tiger weak-symbol guard) */

/* Minimum time a queued status item must remain on screen before the next one
 * can take over. Coalesced updates (matching `key`) bypass this — they update
 * the current item in place without resetting the dwell. */
static const NSTimeInterval kENILSyncMiniMinDisplaySec = 3.0;

/* ------------------------------------------------------------------------- */
/* ENILSyncStatusItem — a single entry in the display queue. `key` identifies
 * the item for coalescing: a follow-up notification with the same key updates
 * this item in place instead of enqueuing a new one (so per-phase progress
 * posts advance the bar smoothly while still obeying the 3s dwell against
 * unrelated events). File-private to the queue. */
@interface ENILSyncStatusItem : NSObject {
 @public
  NSString *key;       /* nil = anonymous one-shot */
  NSString *label;
  BOOL      determinate;
  BOOL      isError;   /* sticky red item — promote() never replaces it
                          automatically; only a same-key non-error follow-up
                          supersedes. */
  int       unitCount;
  int       unitTotal;
}
@end
@implementation ENILSyncStatusItem
- (void)dealloc;
{
  [key   release];
  [label release];
  [super dealloc];
}
@end

/* ------------------------------------------------------------------------- */

@interface ENILSyncStatusQueue ()
- (void)notifyChange;
- (void)promote;
- (void)schedulePromoteTimer;
- (void)updateForState:(ENILSyncState)state;
- (NSString *)steadyStateText;
@end

@implementation ENILSyncStatusQueue

- (id)init;
{
  return [self initWithAccount:nil];
}

- (id)initWithAccount:(id)account;
{
  if ((self = [super init])) {
    queue_     = [[NSMutableArray alloc] init];
    syncState_ = ENILSyncStateNeverSynced;
    account_   = account;  /* unsafe_unretained match token; never messaged */
    /* Progress posts come from the C layer with object:nil (no account handle),
     * so this one stays global. State + SSE events are posted with
     * object:account, so scoping them to account_ keeps one macOS window from
     * mirroring another account's state (e.g. a sticky "Link Off"). */
    [[NSNotificationCenter defaultCenter] addObserver:self
      selector:@selector(handleStatusNote:)
          name:ENILSyncStatusNotification
        object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self
      selector:@selector(handleStateNote:)
          name:ENILSyncStateChangedNotification
        object:account_];
    [[NSNotificationCenter defaultCenter] addObserver:self
      selector:@selector(handleSSENote:)
          name:ENILSSEEventNotification
        object:account_];
  }
  return self;
}

- (void)dealloc;
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [promoteTimer_ invalidate];
  [promoteTimer_ release];
  [queue_ release];
  [currentItem_ release];
  [super dealloc];
}

- (id<ENILSyncStatusQueueDelegate>)delegate; { return delegate_; }

/* Assigning the delegate paints the initial state immediately — the view is
 * fully built by the time it sets itself, and no first notification is
 * guaranteed (an already-synced account may sit idle at launch). */
- (void)setDelegate:(id<ENILSyncStatusQueueDelegate>)delegate;
{
  delegate_ = delegate;
  if (delegate_) [delegate_ syncStatusQueueDidChange:self];
}

- (void)notifyChange;
{
  if (delegate_) [delegate_ syncStatusQueueDidChange:self];
}

/* ----------------------------- render snapshot --------------------------- */

static double clampPercent(double value)
{
  if (value < 0.0) return 0.0;
  if (value > 100.0) return 100.0;
  return value;
}

static double progressPercentForItem(ENILSyncStatusItem *cur)
{
  if (!cur || cur->unitTotal <= 0) return 0.0;
  return clampPercent(((double)cur->unitCount / (double)cur->unitTotal) * 100.0);
}

- (ENILSyncDisplayMode)displayMode;
{
  return (syncState_ == ENILSyncStateSyncing) ? ENILSyncDisplayModeBar
                                              : ENILSyncDisplayModeSteady;
}

- (NSString *)displayLabel;
{
  ENILSyncStatusItem *cur = (ENILSyncStatusItem *)currentItem_;
  if (cur) return cur->label ? cur->label : @"";
  return [self steadyStateText];
}

- (BOOL)displayDeterminate;
{
  ENILSyncStatusItem *cur = (ENILSyncStatusItem *)currentItem_;
  return !cur || cur->determinate;
}

- (double)displayPercent;
{
  return progressPercentForItem((ENILSyncStatusItem *)currentItem_);
}

/* Bar mode with no item is never an error; steady mode with no item tints red
 * when the health gate is set (the Syncing->Error transition flushes the
 * sticky queue, so the categorized health label must re-assert the red here —
 * mirrors the macOS renderSteadyState rule). */
- (BOOL)displayIsError;
{
  ENILSyncStatusItem *cur = (ENILSyncStatusItem *)currentItem_;
  if (cur) return cur->isError;
  if ([self displayMode] == ENILSyncDisplayModeSteady)
    return [ENILAccount anyServiceFailed];
  return NO;
}

- (BOOL)syncButtonEnabled; { return syncState_ != ENILSyncStateSyncing; }
- (ENILSyncState)syncState; { return syncState_; }

/* Friendly display label for the work-phase keys posted by enil_sync.c.
 * Unknown keys (or nil) fall back to the C-side message. Both are routed
 * through NSLocalizedString so the active language picks the right text — the
 * C side stays free of any Cocoa localization concerns. */
static NSString *labelForKey(NSString *key, NSString *fallback)
{
  NSString *raw = nil;
  if      (!key)                                          raw = nil;
  else if ([key isEqualToString:@"sync.connecting"])      raw = @"Connecting";
  else if ([key isEqualToString:@"sync.account"])         raw = @"Account";
  else if ([key isEqualToString:@"sync.messages"])        raw = @"Messages";
  else if ([key isEqualToString:@"sync.decrypting"])      raw = @"Decrypting";
  else if ([key isEqualToString:@"sync.preparing"])       raw = @"Purchases";
  else if ([key isEqualToString:@"sync.downloading"])     raw = @"Downloads";
  else if ([key isEqualToString:@"sync.done"])            raw = @"Done";
  if (raw) return NSLocalizedString(raw, nil);
  if (fallback && [fallback length])
    return NSLocalizedString(fallback, nil); /* C-side message becomes key */
  return NSLocalizedString(@"Working…", nil);
}

- (NSString *)steadyStateText;
{
  /* Reauthenticate is a more specific actionable error than a generic LINE
   * health failure, so it wins. Otherwise a classified health failure overrides
   * both ENILSyncStateError and Live (so the user notices when SSE has been
   * torn down even though no manual sync was in progress). */
  if (syncState_ == ENILSyncStateSyncing)
    return NSLocalizedString(@"Syncing", nil);
  if (syncState_ == ENILSyncStateReauthNeeded)
    return NSLocalizedString(@"Reauthenticate", nil);
  /* A deliberate disconnect is the user's own choice, so it wins over a stale
   * health flag — "Link Off" must read unambiguously, never "LINE Error". */
  if (syncState_ == ENILSyncStateOffline)
    return NSLocalizedString(@"Link Off", nil);
  if ([ENILAccount workerFailed])
    return NSLocalizedString(@"Worker Error", nil);
  if ([ENILAccount lineFailed])
    return NSLocalizedString(@"LINE Error", nil);

  switch (syncState_) {
    case ENILSyncStateNeverSynced:  return NSLocalizedString(@"Sync Required",  nil);
    case ENILSyncStateLive:         return NSLocalizedString(@"Syncing Live",   nil);
    case ENILSyncStateSyncNeeded:   return NSLocalizedString(@"Sync Requested", nil);
    case ENILSyncStateError:        return NSLocalizedString(@"Sync Error",     nil);
    case ENILSyncStateReauthNeeded: return NSLocalizedString(@"Reauthenticate", nil);
    case ENILSyncStateSyncing:      return NSLocalizedString(@"Syncing",        nil);
    case ENILSyncStateReconnecting: return NSLocalizedString(@"Reconnecting",   nil);
    case ENILSyncStateOffline:      return NSLocalizedString(@"Link Off",       nil);
  }
  return @"";
}

/* ----------------------------- state machine ----------------------------- */

- (void)updateForState:(ENILSyncState)state;
{
  ENILSyncState prev = syncState_;
  syncState_ = state;

  /* Leaving Syncing — flush any leftover queue items so the steady-state text
   * shows immediately (a sync that ended with stale per-phase posts shouldn't
   * leave them displayed once the bar is gone). */
  if (prev == ENILSyncStateSyncing && state != ENILSyncStateSyncing) {
    [queue_ removeAllObjects];
    [currentItem_ release];
    currentItem_ = nil;
    [promoteTimer_ invalidate];
    [promoteTimer_ release];
    promoteTimer_ = nil;
  }

  [self notifyChange];
}

- (void)handleStateNote:(NSNotification *)note;
{
  int s = [[[note userInfo] objectForKey:@"state"] intValue];
  [self updateForState:(ENILSyncState)s];
}

/* ------------------------------- the queue ------------------------------- */

/* Locate a queued item by key. Returns nil if no key match (anonymous items
 * NEVER coalesce — each is its own entry). The current item is checked
 * separately by the caller. */
static ENILSyncStatusItem *findByKey(NSArray *queue, NSString *key)
{
  NSUInteger i, n;
  if (!key) return nil;
  n = [queue count];
  for (i = 0; i < n; i++) {
    ENILSyncStatusItem *it = (ENILSyncStatusItem *)[queue objectAtIndex:i];
    if (it->key && [it->key isEqualToString:key]) return it;
  }
  return nil;
}

static void updateItemFields(ENILSyncStatusItem *it, NSString *label,
                             BOOL determinate, int unitCount, int unitTotal,
                             BOOL isError)
{
  if (label != it->label) {
    [it->label release];
    it->label = [label copy];
  }
  it->determinate = determinate;
  it->unitCount   = unitCount;
  it->unitTotal   = unitTotal;
  it->isError     = isError;
}

- (void)handleStatusNote:(NSNotification *)note;
{
  NSDictionary *info = [note userInfo];
  NSString *key      = [info objectForKey:@"key"];
  NSString *message  = [info objectForKey:@"message"];
  BOOL determinate   = ![[info objectForKey:@"indeterminate"] boolValue];
  int unitCount      = [[info objectForKey:@"unitCount"] intValue];
  int unitTotal      = [[info objectForKey:@"unitTotal"] intValue];
  BOOL isError       = [[info objectForKey:@"error"] intValue] != 0;
  NSString *label    = labelForKey(key, message);
  ENILSyncStatusItem *cur = (ENILSyncStatusItem *)currentItem_;
  ENILSyncStatusItem *existing;

  /* Coalesce with the currently displayed item — no dwell reset, just an
   * in-place field swap so the bar progresses. The error transition matters
   * both ways: an error item replacing a non-error item (same key) becomes
   * sticky; a non-error item replacing a sticky error item allows the promote
   * loop to advance again. */
  if (cur && key && cur->key && [cur->key isEqualToString:key]) {
    updateItemFields(cur, label, determinate, unitCount, unitTotal, isError);
    [self notifyChange];
    /* A determinate item that has reached 100% should be allowed to promote
     * immediately — fire the promote check now in case the next queued item is
     * waiting on dwell completion. Skip for sticky errors. */
    if (!isError && determinate && unitTotal > 0 && unitCount >= unitTotal)
      [self promote];
    /* Cleared the sticky error — give the promote loop a kick so the next
     * queued item can take over (sticky error suppressed it). */
    if (!isError && cur->isError)
      [self promote];
    return;
  }

  /* Coalesce with an enqueued (not yet displayed) item — same trick. */
  existing = findByKey(queue_, key);
  if (existing) {
    updateItemFields(existing, label, determinate, unitCount, unitTotal,
                     isError);
    return;
  }

  /* Brand-new item. */
  {
    ENILSyncStatusItem *it = [[ENILSyncStatusItem alloc] init];
    it->key = [key copy];
    updateItemFields(it, label, determinate, unitCount, unitTotal, isError);
    [queue_ addObject:it];
    [it release];
  }
  [self promote];
}

/* Try to advance the current item. Honors the minimum-dwell rule unless the
 * current item has reached 100% (determinate completion = OK to swap
 * immediately). Schedules a wake-up timer when dwell isn't yet satisfied. */
- (void)promote;
{
  NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
  ENILSyncStatusItem *cur = (ENILSyncStatusItem *)currentItem_;

  if (cur) {
    BOOL dwellExpired = (now - currentShownAt_) >= kENILSyncMiniMinDisplaySec;
    BOOL completed    = cur->determinate && cur->unitTotal > 0 &&
                        cur->unitCount >= cur->unitTotal;
    /* Sticky error: never auto-promote. The only way to replace this item is a
     * same-key non-error post (handled by the coalesce branch above). Without
     * this guard the 3s dwell would drop the red row back to the steady-state
     * label and the user would never see why messages stopped arriving. */
    if (cur->isError) return;
    if (!dwellExpired && !completed) {
      [self schedulePromoteTimer];
      return;
    }
  }

  if ([queue_ count] == 0) {
    /* During an active sync, hold the current item on screen — the bar and
     * label stay put between phases (and between sub-task boundaries within a
     * phase) so the bar never visibly resets to 0 until a real new phase posts
     * and takes over. Outside Syncing we clear, restoring the steady-state text
     * for transient items. */
    if (syncState_ == ENILSyncStateSyncing) return;
    [currentItem_ release];
    currentItem_ = nil;
    [self notifyChange];
    return;
  }

  /* Pop the head and display it. */
  [currentItem_ release];
  currentItem_ = [[queue_ objectAtIndex:0] retain];
  [queue_ removeObjectAtIndex:0];
  currentShownAt_ = now;
  [self notifyChange];
  [self schedulePromoteTimer];
}

- (void)promoteTimerFired:(NSTimer *)timer;
{
  (void)timer;
  [promoteTimer_ invalidate];
  [promoteTimer_ release];
  promoteTimer_ = nil;
  [self promote];
}

/* Arms a one-shot timer for the soonest moment the current item could be
 * replaced — OR the moment it should be cleared back to the steady-state label
 * if no follow-up is queued. The fire-on-empty-queue path matters for
 * transient items like the SSE "Received message" row: without it the item
 * would sit on screen forever after the dwell since no later event ever nudges
 * promote() to re-evaluate.
 *
 * Scheduled in the common run-loop modes (not the default mode the AppKit
 * original used) so the dwell keeps ticking while an iOS UITableView is
 * mid-scroll — scroll-tracking would otherwise stall a default-mode timer.
 * Harmless on macOS, where the bar isn't inside a tracking scroller. The mode
 * comes from XPRunLoopCommonModes, not NSRunLoopCommonModes: the latter is a
 * 10.5+ symbol that weak-imports to NULL on Tiger and crashes when read (see
 * XPFoundation.h). */
- (void)schedulePromoteTimer;
{
  NSTimeInterval now;
  NSTimeInterval remaining;
  if (!currentItem_) return;
  if (promoteTimer_) return;
  now = [NSDate timeIntervalSinceReferenceDate];
  remaining = (currentShownAt_ + kENILSyncMiniMinDisplaySec) - now;
  if (remaining < 0.05) remaining = 0.05;
  promoteTimer_ = [[NSTimer timerWithTimeInterval:remaining
                                           target:self
                                         selector:@selector(promoteTimerFired:)
                                         userInfo:nil
                                          repeats:NO] retain];
  [[NSRunLoop currentRunLoop] addTimer:promoteTimer_
                               forMode:XPRunLoopCommonModes];
}

/* --------------------------------- SSE ----------------------------------- */

/* Pings animate the steady-state "Syncing Live" text instead of enqueuing a
 * row (otherwise the bar would flicker every ~20s). The gate lives here so both
 * platforms share it: shimmer only when idle-live with nothing transient up. */
- (void)handleSSENote:(NSNotification *)note;
{
  NSString *type = [[note userInfo] objectForKey:@"type"];
  if (![type isEqualToString:@"ping"]) return;
  if (syncState_ != ENILSyncStateLive) return;
  if (currentItem_) return;
  if (delegate_) [delegate_ syncStatusQueueRequestsShimmer:self];
}

@end
