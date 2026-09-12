#import "XPAppKit.h"

// --- KeyValueTableView Interface ---
@interface KeyValueTableView : NSView <XPTableViewDataSource, XPTableViewDelegate> {
 @private
  NSScrollView *scrollView_;
  NSTableView *tableView_;
  NSDictionary *data_;
  NSArray *sortedKeys_;
}
- (void)setData:(NSDictionary *)data;
- (NSDictionary *)data;
@end

// --- AboutWindowController Interface ---
@interface AboutWindowController : NSWindowController
@end
