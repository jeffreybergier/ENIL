#import "ENILWindowAwareView.h"
#import "XPFoundation.h"

@implementation ENILWindowAwareView

- (void)setWindowChangeTarget:(id)target action:(SEL)action;
{
  windowTarget_ = target;
  windowAction_ = action;
}

- (void)viewDidMoveToWindow;
{
  [super viewDidMoveToWindow];
  if (![self window]) return;
  if (!windowTarget_ || !windowAction_) return;
  if (![windowTarget_ respondsToSelector:windowAction_]) {
    ENILLog(@"ENILWindowAwareView.viewDidMoveToWindow", @"target does not "
          @"respond to %@", NSStringFromSelector(windowAction_));
    return;
  }
  [windowTarget_ performSelector:windowAction_ withObject:self];
}

@end
