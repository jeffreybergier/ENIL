#import <UIKit/UIKit.h>

/* Main-thread-only, isolated diagnostic controller. Owns its bulletin until
 * withdrawal or termination. Delayed work is cancelled during teardown. */
@interface MainViewController : UIViewController <UITableViewDataSource, UITableViewDelegate> {
  UITextView *output_;
  NSMutableString *log_;
  id request_;
  id observer_;
  NSMutableArray *bulletins_;
  NSMutableArray *publicNotes_;
  UITableView *bulletinTable_;
  UILabel *status_;
  NSString *selectedID_;
  BOOL receivedBulletin_;
  NSUInteger publicSequence_;
  BOOL pending_;
  UIBackgroundTaskIdentifier task_;
}
- (void)runCommand:(NSString *)command;
@end
