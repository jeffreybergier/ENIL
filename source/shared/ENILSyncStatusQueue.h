#import <Foundation/Foundation.h>
#import "ENILAccount.h"   /* ENILSyncState, notification names, health gate */

/* Which of the two display layouts the UI should paint right now. The queue
 * decides this from syncState_; the UI maps it to its own widgets. */
typedef enum {
  ENILSyncDisplayModeBar    = 0, /* a sync is in flight: progress bar + label */
  ENILSyncDisplayModeSteady = 1  /* idle/live: centered status text (+ shimmer) */
} ENILSyncDisplayMode;

@class ENILSyncStatusQueue;

/* Thin render contract. The queue never touches AppKit/UIKit; it tells the UI
 * "something changed, re-read my display* snapshot" and "play the one-shot
 * shimmer now". Both callbacks are delivered on the main thread. */
@protocol ENILSyncStatusQueueDelegate <NSObject>
- (void)syncStatusQueueDidChange:(ENILSyncStatusQueue *)queue;
- (void)syncStatusQueueRequestsShimmer:(ENILSyncStatusQueue *)queue;
@end

/* Portable status/queue engine shared by the macOS SyncMiniViewController and
 * the iOS SyncMiniBarView. Foundation-only (NSTimer/NSNotification/NSDate),
 * MRC — it builds into the Tiger PPC macOS target as well as iOS, so it carries
 * no ARC and no framework dependency.
 *
 * On init it self-wires to the three sync notifications and runs the dwell /
 * coalesce / sticky-error machine internally; the owning view just implements
 * the delegate. Each queued status item displays for at least
 * kENILSyncMiniMinDisplaySec before the next can take over, unless a follow-up
 * post carries a matching `key` (then it updates in place).
 *
 * Pass the owning ENILAccount to -initWithAccount: to SCOPE the per-account
 * notifications (state + SSE event, both posted with object:account) to that
 * account, so on macOS one window's queue never reacts to another account's
 * state changes. A nil account (or plain -init) keeps the old global behavior,
 * which is correct on iOS where only one engine is ever live. */
@interface ENILSyncStatusQueue : NSObject {
 @private
  id<ENILSyncStatusQueueDelegate> delegate_; /* unsafe_unretained — 4.3 floor */
  NSMutableArray *queue_;
  id              currentItem_;     /* ENILSyncStatusItem * (file-private) */
  NSTimeInterval  currentShownAt_;
  NSTimer        *promoteTimer_;
  ENILSyncState   syncState_;
  id              account_;         /* unsafe_unretained notification-filter token */
}

/* Designated initializer. account is the notification-filter object (the
 * ENILAccount); nil = global (every account's notifications). Used only as an
 * NSNotificationCenter match token — never messaged — so an unretained ref is
 * safe. Plain -init delegates here with nil. */
- (id)initWithAccount:(id)account;

/* Back-pointer is assign/unsafe_unretained (4.3 floor: no zeroing weak). Plain
 * getter/setter rather than @property — this header also compiles under the
 * Tiger PPC GCC, where Objective-C 2.0 declared properties don't exist.
 * Assigning a non-nil delegate fires an immediate -syncStatusQueueDidChange: so
 * the view paints its initial (NEVER_SYNCED) state without a first
 * notification. */
- (id<ENILSyncStatusQueueDelegate>)delegate;
- (void)setDelegate:(id<ENILSyncStatusQueueDelegate>)delegate;

/* Render snapshot — the view reads these inside -syncStatusQueueDidChange:. */
- (ENILSyncDisplayMode)displayMode;
- (NSString *)displayLabel;   /* localized, ready to paint */
- (BOOL)displayDeterminate;   /* meaningful only in bar mode */
- (double)displayPercent;     /* 0..100 (only when determinate) */
- (BOOL)displayIsError;
- (BOOL)syncButtonEnabled;
- (ENILSyncState)syncState;

@end
