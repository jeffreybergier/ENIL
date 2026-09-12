#import "ENILBottomToolbarView.h"
#import "XPAppKit.h"

@implementation ENILBottomToolbarView

- (BOOL)isOpaque;
{
  return YES;
}

- (void)drawRect:(NSRect)dirtyRect;
{
  NSRect bounds = [self bounds];
  (void)dirtyRect;

  [XPColorWindowFrame set];
  NSRectFill(bounds);

  [XPColorControlHighlight set];
  NSRectFill(NSMakeRect(bounds.origin.x,
                        bounds.origin.y + bounds.size.height - 1.0,
                        bounds.size.width, 1.0));
}

@end
