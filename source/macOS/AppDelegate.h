#import "XPAppKit.h"
#import "ENILAccount.h"
#import "AboutWindowController.h"
#import "ChatWindowController.h"
#import "QRLoginWindowController.h"
#import "PreferencesWindowController.h"

// --- MainMenu Interface ---
@interface MainMenu : NSObject
+ (void)setupMenu;
@end

// --- AppDelegate Interface ---
@interface AppDelegate : NSObject
    <XPApplicationDelegate, QRLoginWindowControllerDelegate> {
 @private
  NSMutableArray            *_accounts;   /* of ENILAccountContext, one per window */
  NSMenu                    *_accountsMenu; /* the "Accounts" submenu (not retained
                                               by us — the main menu owns it) */
  AboutWindowController     *_aboutWindowController;
  PreferencesWindowController *_preferencesWindowController;
  QRLoginWindowController *_qrLoginWindowController;
}
- (void)showAboutWindow:(id)sender;
- (void)showPreferencesWindow:(id)sender;
- (void)addAccount:(id)sender;
@end
