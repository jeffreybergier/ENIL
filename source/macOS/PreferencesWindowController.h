#import "XPAppKit.h"

/* Single tab/single pane for now: worker endpoint + shared secret. Reads from
 * ENILKeychain on -windowDidLoad; on Save writes back through ENILKeychain
 * (which posts ENILKeychainDidChangeNotification — AppDelegate picks that up
 * to reconfigure the C worker layer). */
@interface PreferencesWindowController : NSWindowController {
 @private
  NSTextField       *urlField_;
  NSSecureTextField *secretField_;
  NSTextField       *envHint_;
}
- (id)init;
- (void)saveAction:(id)sender;
- (void)cancelAction:(id)sender;
@end
