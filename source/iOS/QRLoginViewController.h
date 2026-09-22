//
//  QRLoginViewController.h
//  ENIL
//

#import <UIKit/UIKit.h>

@class QRLoginViewController;

/* All fire on the main thread. -didFinishWithAccountDir: relocates the staged
 * session and activates the account; it returns NO on any local failure so the
 * controller can surface it (the success path is otherwise silent). On YES the
 * controller calls -qrLoginViewControllerDidComplete: so the delegate can tear
 * down the modal + drop its refs; -qrLoginViewControllerDidFail: handles cancel
 * and post-success local failure (discard staging + dismiss). The iOS peer of
 * QRLoginWindowControllerDelegate. */
@protocol QRLoginViewControllerDelegate <NSObject>
- (BOOL)qrLoginViewController:(QRLoginViewController *)c
       didFinishWithAccountDir:(NSString *)accountDir;
- (void)qrLoginViewControllerDidComplete:(QRLoginViewController *)c;
- (void)qrLoginViewControllerDidFail:(QRLoginViewController *)c;
@end

/* Animated client, QR-scan and phone-verification pages, with native navigation-bar
 * Back/Next controls and saved-login recovery. Drives the QR-login handshake via
 * +[ENILAccount runQRLoginAtPath:observer:cancelFlag:] on a background thread.
 * The blocking long-polls never touch the main runloop; ENILAccount marshals
 * the observer callbacks back to the main thread. Cancel (the nav-bar button)
 * flips the cancel flag, aborting the in-flight long-poll within ~1s. The iOS
 * peer of source/macOS/QRLoginWindowController. */
@interface QRLoginViewController : UITableViewController
- (instancetype)initWithAccountDir:(NSString *)accountDir
                       expectedMid:(NSString *)expectedMid
                          delegate:(id <QRLoginViewControllerDelegate>)delegate;
- (NSString *)accountDir;
- (NSString *)expectedMid;
@end
