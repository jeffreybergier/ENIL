#import <AppKit/AppKit.h>

/* NSView that forwards -viewDidMoveToWindow to a target once a window is
 * present. Lets a controller defer window-scale-dependent work (e.g.
 * AIFontAwesome rasterisation) until the backing scale is known and correct.
 * The action is also re-sent on every later window attach, so moving to a
 * display with a different backing scale re-runs it. */
@interface ENILWindowAwareView : NSView {
 @private
  id  windowTarget_;   /* not retained — controller owns the view */
  SEL windowAction_;
}
- (void)setWindowChangeTarget:(id)target action:(SEL)action;
@end
