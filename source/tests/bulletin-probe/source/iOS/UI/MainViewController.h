#import <UIKit/UIKit.h>

/* Main-thread-only, isolated diagnostic controller. Owns its bulletin until
 * withdrawal or termination. Delayed work is cancelled during teardown. */
@interface MainViewController : UIViewController {
  UITextView *output_;
  NSMutableString *log_;
  id request_;
  BOOL pending_;
  UIBackgroundTaskIdentifier task_;
}
- (void)runCommand:(NSString *)command;
@end
