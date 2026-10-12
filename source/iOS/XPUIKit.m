//
//  XPUIKit.m
//  ENIL
//

#import "XPUIKit.h"
#import "XPFoundation.h"   /* ENILLog */
#import "UIViewController+ENILModal.h"

@implementation UIDevice (XPUIKit)

- (BOOL)XP_isOperatingSystemAtLeastMajorVersion:(NSInteger)majorVersion
{
  NSString *systemVersion = [self systemVersion];
  return ([systemVersion integerValue] >= majorVersion) ? YES : NO;
}

@end

@implementation UIViewController (XPUIKit)

- (void)XP_showAlertWithTitle:(NSString *)title
                      message:(NSString *)message
                 dismissTitle:(NSString *)dismissTitle
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  if (NSClassFromString(@"UIAlertController")) {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title
      message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:dismissTitle
      style:UIAlertActionStyleCancel handler:nil]];
    [self enil_presentModalViewController:alert];
  } else {
    UIAlertView *alert = [[UIAlertView alloc] initWithTitle:title message:message
      delegate:nil cancelButtonTitle:dismissTitle otherButtonTitles:nil];
    [alert show];
  }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

- (void)XP_layoutBelowBars
{
  if (![self respondsToSelector:@selector(setEdgesForExtendedLayout:)]) return;
  /* The runtime guard keeps this iOS-7-only setter off the iOS 6 path. */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
#endif
  [self setEdgesForExtendedLayout:UIRectEdgeNone];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

@end

/* One pragma chokepoint per floor-unavailable API, mirroring XPAppKit: the
 * push/ignored/pop wraps the unavailable-on-floor call inside the wrapper, so
 * every caller stays warning-clean. */

@implementation UIWebView (XPUIKit)

- (UIScrollView *)XP_scrollView
{
  /* iOS 4.3 floor: -scrollView doesn't exist yet. Return nil rather than throw;
   * the caller's `setDecelerationRate:` message then no-ops on nil. The pragma
   * silences -Wunguarded-availability for naming the iOS-5 accessor against that floor
   * (the respondsToSelector: gate is what keeps it safe at runtime). */
  if (![self respondsToSelector:@selector(scrollView)]) return nil;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
#endif
  return [self scrollView];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

@end

@implementation UIColor (XPUIKit)

+ (UIColor *)messagesBackgroundColor
{
  /* f-suffixed literals keep the division in float so CGFloat (float on armv7)
   * takes no narrowing-conversion warning. */
  return [UIColor colorWithRed:220.0f/255.0f
                         green:226.0f/255.0f
                          blue:236.0f/255.0f
                         alpha:1.0f];
}

@end

@implementation UIView (XPUIKit)

- (void)XP_setBackgroundTransparent
{
  [self setOpaque:NO];
  [self setBackgroundColor:[UIColor clearColor]];
}

- (void)XP_removeShadow
{
  NSEnumerator *e = [[self subviews] objectEnumerator];
  UIView *v;
  while ((v = [e nextObject])) {
    if (![v isKindOfClass:[UIImageView class]]) break;  /* hit the content view */
    [v setHidden:YES];
  }
}

@end
