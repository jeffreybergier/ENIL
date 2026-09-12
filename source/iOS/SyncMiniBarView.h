//
//  SyncMiniBarView.h
//  ENIL
//

#import <UIKit/UIKit.h>

/* The iOS counterpart to the macOS SyncMiniViewController's bottom bar. A thin,
 * content-hugging status view (a centered label + an always-determinate
 * progress bar) meant to ride CENTERED in the navigation controller's real
 * bottom UIToolbar via a
 * flexible-space sandwich: [flexibleSpace, customViewItem, flexibleSpace]. The
 * platform UIToolbar draws the chrome — gradient, gloss, hairlines — so this
 * view paints nothing of its own (clearColor background); it just sizes itself
 * to its text and lets Apple's bar show through. The one bit of hand-drawn
 * polish is a light-gray shimmer sweep over the steady-state text, fired on
 * each SSE ping as a live-connection heartbeat. It owns the portable
 * ENILSyncStatusQueue and is its render delegate; all queue, dwell, coalescing
 * and sticky-error logic lives in the shared queue. */
@interface SyncMiniBarView : UIView
/* Designated initializer. `account` is forwarded to the internal
 * ENILSyncStatusQueue so its per-account notifications (state + SSE event) are
 * scoped to that account — mirroring the macOS chain. iOS keeps one live engine
 * at a time, so nil (or -initWithFrame:) is functionally identical today; the
 * parameter exists to stay correct if multiple engines ever coexist. */
- (instancetype)initWithFrame:(CGRect)frame account:(id)account;
@end
