#import <AppKit/AppKit.h>
#import "AICookieCutterWindowController.h"

@class SyncMiniViewController;

/* AIViewController subclass so the parent (ChatListViewController) can adopt
 * us via AI_addChildViewController:. SyncMiniViewController is itself an
 * AIViewController and is adopted the same way — no manual setNextResponder:
 * wiring on either tier. */
@interface StatusViewController : AIViewController {
 @private
  /* Non-owning typed alias for [self view]; base owns the retain. */
  NSView                 *barView_;
  SyncMiniViewController *syncMiniController_;
  id                      account_;  /* unsafe_unretained; forwarded to the mini bar */
}

/* account scopes the bar's status queue to one account (see
 * SyncMiniViewController). nil = global (legacy). */
- (id)initWithAccount:(id)account;

@end
