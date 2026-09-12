#import <AppKit/AppKit.h>
#import "AICookieCutterWindowController.h"
#import "ENILSyncStatusQueue.h"

@class SyncMiniStatusTextView;

/* AIViewController subclass so the parent (StatusViewController) can adopt us
 * via AI_addChildViewController: — on Middle/Modern AppKit wires the responder
 * chain through views; on Legacy the cookie cutter wires the child → parent hop
 * and AIViewController -setView: wires [view → self].
 *
 * The queue / dwell / coalesce / state machine now lives in the portable
 * ENILSyncStatusQueue (source/shared); this controller is just its AppKit
 * render delegate — it owns the four bar widgets, the layout, and the shimmer
 * timer, and repaints from the queue's display* snapshot. */
@interface SyncMiniViewController : AIViewController <ENILSyncStatusQueueDelegate> {
 @private
  NSProgressIndicator *progress_;
  NSButton            *syncButton_;
  NSButton            *linkButton_;   /* SSE on/off toggle, left edge */
  NSTextField         *syncingLabel_;
  SyncMiniStatusTextView *statusView_;
  NSTimer             *shineTimer_;
  NSTimeInterval       shineStart_;
  ENILSyncStatusQueue *queue_;
  BOOL                 linkShowsOff_;  /* cached: link glyph currently link-slash */
  id                   account_;       /* unsafe_unretained; scopes the queue */
}

/* account scopes the status queue's per-account notifications so one window's
 * bar never mirrors another account's state. nil = global (legacy). */
- (id)initWithAccount:(id)account;

@end
