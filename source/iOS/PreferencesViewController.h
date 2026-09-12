//
//  PreferencesViewController.h
//  ENIL
//

#import <UIKit/UIKit.h>

@class ENILRootCoordinator;

/* Settings + account switcher. Three grouped sections:
 *
 *   1. Accounts — the multi-account switcher (folded in here instead of a
 *      standalone AccountSwitcherViewController): one row per account from disk
 *      (checkmark = active, tap = switch), swipe-to-delete = Log Out, plus
 *      Reauthenticate + Add Account rows that drive the coordinator's QR flow.
 *   2. Cloudflare Worker — the iOS port of the macOS PreferencesWindowController
 *      worker pane: Worker URL + shared secret written to ENILKeychain via
 *      -saveWorkerURL:secret:. Saving posts ENILKeychainDidChangeNotification,
 *      which ENILRootCoordinator observes to continue a launch gated on missing
 *      credentials.
 *   3. Linked Libraries — read-only version list of the bundled C libraries,
 *      mirroring the macOS AboutWindowController key/value table. The cJSON /
 *      sqlite version calls are made here directly (sanctioned — see AGENTS.md).
 *
 * Used both as the credential gate / empty-state (nav root, no Done button) and
 * presented modally from the chat list's Accounts button. The account verbs are
 * delegated to `coordinator`; without one the Accounts section is hidden. */
@interface PreferencesViewController : UITableViewController
/* Back-pointer (assign/unsafe_unretained per the 4.3 floor — no zeroing weak).
 * The coordinator outlives every Preferences instance it owns. */
@property (nonatomic, assign) ENILRootCoordinator *coordinator;
/* Set YES by a modal presenter (the chat list's Accounts button): the Done
 * button then self-dismisses after committing. Left NO as the nav root (gate /
 * empty-state) — nothing to dismiss to, and -presentingViewController is iOS
 * 5.0+ (past the 4.3 floor), so the presenter declares intent instead. */
@property (nonatomic, assign) BOOL dismissesOnDone;
@end
