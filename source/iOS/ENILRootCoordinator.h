//
//  ENILRootCoordinator.h
//  ENIL
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

@class StickerViewController;

/* Owns the single UINavigationController and the one live ENILAccount. Mirrors
 * the macOS AppDelegate's orchestration (account discovery + worker-credential
 * gate) minus the per-account windows: on the phone exactly one account is
 * active at a time (the "one active account + switcher" decision in
 * PLAN.IOS.md). The switcher is folded into PreferencesViewController's
 * Accounts section rather than a standalone AccountSwitcherViewController. */
@interface ENILRootCoordinator : NSObject

/* window must be non-nil; the coordinator installs its navigation controller as
 * the window's rootViewController. Raises NSInvalidArgumentException on nil. */
- (instancetype)initWithWindow:(UIWindow *)window;

/* Runs the credential gate, then activates the first discovered account (or
 * shows the settings root when credentials / accounts are missing). */
- (void)start;

#pragma mark - Account management (driven by PreferencesViewController)

/* One NSDictionary per discovered account: {@"path", @"displayName",
 * @"active"(NSNumber BOOL)}. Reads each session.json from disk — no engine is
 * started for inactive accounts (the one-live-engine invariant from
 * PLAN.IOS.md). */
- (NSArray *)accountSummaries;
/* Stop the live engine and activate the account at `path`, swapping the nav
 * root to its chat list and dismissing the presented modal. No-op (YES) when
 * already active. Returns NO if the account failed to start. */
- (BOOL)switchToAccountAtPath:(NSString *)path;
/* Trash the account folder (a hard delete on iOS — see XP_trashItemAtPath). If
 * it was the active account, tear its engine down and activate the next one
 * (or drop to the settings root). Returns YES when the active account changed
 * (the coordinator has reshaped the root + dismissed the modal); NO when a
 * background account was removed in place (the caller just reloads its list). */
- (BOOL)logOutAccountAtPath:(NSString *)path;
/* Present the QR login modal from `presenter`. Add Account passes a nil
 * expected mid; Reauthenticate targets the active account's mid so a mismatched
 * scan is logged. */
- (void)presentAddAccountFromViewController:(UIViewController *)presenter;
- (void)presentReauthFromViewController:(UIViewController *)presenter;

/* YES when the active account wants the live SSE link (its persisted sseEnabled
 * flag); NO when no account is active. AppDelegate gates its bg assertion on it. */
- (BOOL)wantsSSELink;
/* Sum of unreadCount for the active account, or 0 when no account is active.
 * Drives the app-icon badge from AppDelegate without exposing the engine. */
- (int)activeUnreadCount;

#pragma mark - Shared per-account UI

/* The one sticker/sticon picker for the active account, built lazily and held
 * for the account's lifetime so its two resident web views stay warm — message
 * threads present this shared instance instead of building a fresh (slow-to-
 * load) picker on every open. Torn down with the engine, so a switched-in
 * account gets its own. Returns nil when no account is active. */
- (StickerViewController *)sharedStickerPicker;

@end
