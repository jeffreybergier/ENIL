#import <AppKit/AppKit.h>
#import "XPAppKit.h"

@class QRLoginWindowController;

@protocol QRLoginWindowControllerDelegate <NSObject>
/* All fire on the main thread. didFinishWithAccountDir: relocates the staged
 * session and opens the account's window *behind* this one; it returns NO on
 * any local failure so the controller can surface it (the success path is
 * silent). On YES the controller closes its window and calls
 * qrLoginWindowControllerDidComplete: so the delegate can drop its ref.
 * qrLoginWindowControllerDidFail: handles cancel and post-success local
 * failure. */
- (BOOL)qrLoginWindowController:(QRLoginWindowController *)c
        didFinishWithAccountDir:(NSString *)accountDir;
- (void)qrLoginWindowControllerDidComplete:(QRLoginWindowController *)c;
- (void)qrLoginWindowControllerDidFail:(QRLoginWindowController *)c;
@end

/* Three-step wizard with animated sizing and bottom Aqua navigation: client,
 * QR scan, then phone verification. Drives the handshake via
 * +[ENILAccount runQRLoginAtPath:observer:cancelFlag:] on a background thread.
 * The blocking long-polls never touch the main runloop; ENILAccount marshals
 * the observer callbacks back to the main thread. Closing the window flips
 * cancelFlag_, which aborts the in-flight long-poll connection within ~1s
 * (see -windowWillClose:). */
@interface QRLoginWindowController : NSWindowController {
 @private
  NSString      *accountDir_;
  NSView        *clientPage_;
  NSView        *loginPage_;
  NSView        *verificationPage_;
  NSView        *currentPage_; /* weak; one of the retained pages */
  NSTextField   *selectionStatusField_;
  NSBox         *loginBox_;
  NSImageView   *qrImageView_;
  NSTextField   *statusField_;
  NSTextField   *verificationStatusField_;
  NSTextField   *pinField_;
  NSButton      *chromeButton_;
  NSButton      *windowsButton_;
  NSButton      *androidButton_;
  NSButton      *nextButton_;
  NSButton      *backButton_;
  NSButton      *retryButton_;
  NSButton      *restartButton_;
  NSTextField   *recoveryHelpField_;
  NSString      *expectedMid_; /* nil = Add Account; non-nil = Reauthenticate
                                  (the mid we expect the scan to match) */
  id <QRLoginWindowControllerDelegate> delegate_; /* weak */
  BOOL           running_;
  NSMutableDictionary *preparedPaths_; /* one staging directory per client */
  NSMutableArray *stagingPaths_; /* includes unprepared directories for cleanup */
  BOOL           goingBack_; /* wait for the worker before changing identity */
  BOOL           done_;       /* terminal handling reached (success / sheet /
                                 user-close) — guards against the late
                                 background thread resurrecting the window */
  volatile int   cancelFlag_; /* read by the C flow via cb.cancel; set when
                                 the user closes the window */
}
- (id)initWithAccountDir:(NSString *)accountDir
             expectedMid:(NSString *)expectedMid
                delegate:(id <QRLoginWindowControllerDelegate>)delegate;
- (NSString *)accountDir;
- (NSString *)expectedMid;
- (void)start;
@end
