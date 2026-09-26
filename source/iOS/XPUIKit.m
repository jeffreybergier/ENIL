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

static id XPUIKitInvokeObjectGetter(id target, SEL selector)
{
  NSMethodSignature *signature;
  NSInvocation *invocation;
  __unsafe_unretained id result;

  if ((target == nil) || ![target respondsToSelector:selector]) return nil;
  signature = [target methodSignatureForSelector:selector];
  if ((signature == nil) || ([signature numberOfArguments] != 2U)) return nil;
  invocation = [NSInvocation invocationWithMethodSignature:signature];
  [invocation setTarget:target];
  [invocation setSelector:selector];
  [invocation invoke];
  result = nil;
  [invocation getReturnValue:&result];
  return result;
}

static void XPUIKitInvokeObjectSetter(id target, SEL selector, id value)
{
  NSMethodSignature *signature;
  NSInvocation *invocation;
  __unsafe_unretained id argument;

  if ((target == nil) || ![target respondsToSelector:selector]) return;
  signature = [target methodSignatureForSelector:selector];
  if ((signature == nil) || ([signature numberOfArguments] != 3U)) return;
  invocation = [NSInvocation invocationWithMethodSignature:signature];
  [invocation setTarget:target];
  [invocation setSelector:selector];
  argument = value;
  [invocation setArgument:&argument atIndex:2];
  [invocation invoke];
}

static NSUInteger XPUIKitInvokeUnsignedIntegerGetter(id target, SEL selector)
{
  NSMethodSignature *signature;
  NSInvocation *invocation;
  NSUInteger result;

  if ((target == nil) || ![target respondsToSelector:selector]) return 0U;
  signature = [target methodSignatureForSelector:selector];
  if ((signature == nil) || ([signature numberOfArguments] != 2U)) return 0U;
  invocation = [NSInvocation invocationWithMethodSignature:signature];
  [invocation setTarget:target];
  [invocation setSelector:selector];
  [invocation invoke];
  result = 0U;
  [invocation getReturnValue:&result];
  return result;
}

static id XPUIKitCreateNotificationSettings(Class settingsClass,
                                             NSUInteger types)
{
  SEL selector;
  NSMethodSignature *signature;
  NSInvocation *invocation;
  __unsafe_unretained id categoriesArgument;
  __unsafe_unretained id result;

  selector = @selector(settingsForTypes:categories:);
  if ((settingsClass == Nil) ||
      ![(id)settingsClass respondsToSelector:selector]) return nil;
  signature = [(id)settingsClass methodSignatureForSelector:selector];
  if ((signature == nil) || ([signature numberOfArguments] != 4U)) return nil;
  invocation = [NSInvocation invocationWithMethodSignature:signature];
  [invocation setTarget:settingsClass];
  [invocation setSelector:selector];
  categoriesArgument = nil;
  [invocation setArgument:&types atIndex:2];
  [invocation setArgument:&categoriesArgument atIndex:3];
  [invocation invoke];
  result = nil;
  [invocation getReturnValue:&result];
  return result;
}

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

@implementation UIApplication (XPUIKit)

- (void)XP_cancelAllLocalNotifications
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* iOS 10+ SDK */
#endif
  [self cancelAllLocalNotifications];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

@end

@interface XPUserNotificationCenter ()
- (void)deliverTitle:(NSString *)title body:(NSString *)body;
@end

@implementation XPUserNotificationCenter

+ (XPUserNotificationCenter *)defaultCenter
{
  static XPUserNotificationCenter *instance = nil;
  if (!instance) instance = [[XPUserNotificationCenter alloc] init];
  return instance;
}

/* Register for alert+sound+badge on iOS 8+; older OSes have no prompt. Runtime
 * dispatch keeps -Wunguarded-availability quiet against the 4.3 floor.
 * UIUserNotificationType bits: Badge=1 | Sound=2 | Alert=4 = 7. */
- (void)requestAuthorization
{
  @try {
    UIApplication *app = [UIApplication sharedApplication];
    SEL reg = @selector(registerUserNotificationSettings:);
    if (![app respondsToSelector:reg]) return;            /* iOS < 8 */
    Class cls = NSClassFromString(@"UIUserNotificationSettings");
    id settings = XPUIKitCreateNotificationSettings(cls, (NSUInteger)7);
    if (settings != nil) {
      ENILLog(@"XPUserNotificationCenter.requestAuthorization", @"registering alerts, sounds and badges");
      XPUIKitInvokeObjectSetter(app, reg, settings);
    }
  } @catch (NSException *e) {
    ENILLog(@"XPUserNotificationCenter.requestAuthorization", @"failed: %@", e);
  }
}

- (XPNotificationAuthStatus)authorizationStatus
{
  UIApplication *app = [UIApplication sharedApplication];
  SEL cur = @selector(currentUserNotificationSettings);
  if (![app respondsToSelector:cur]) return XPNotificationAuthStatusAuthorized;  /* iOS < 8 */
  id settings = XPUIKitInvokeObjectGetter(app, cur);
  if (!settings) return XPNotificationAuthStatusNotDetermined;
  NSUInteger types = XPUIKitInvokeUnsignedIntegerGetter(settings, @selector(types));
  return types ? XPNotificationAuthStatusAuthorized : XPNotificationAuthStatusDenied;
}

- (void)postNotificationWithTitle:(NSString *)title body:(NSString *)body
{
  if (!body) return;
  @try {
    [self deliverTitle:(title ? title : @"") body:body];
  } @catch (NSException *e) {
    ENILLog(@"XPUserNotificationCenter.postNotificationWithTitle", @"delivery failed: %@", e);
  }
}

- (void)deliverTitle:(NSString *)title body:(NSString *)body
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* UILocalNotification: iOS 10+ SDK */
#endif
  UILocalNotification *n = [[UILocalNotification alloc] init];
  [n setSoundName:UILocalNotificationDefaultSoundName];
  /* alertTitle is iOS 8.2+; KVC dodges availability. Where it exists keep the
   * title separate; pre-8.2 fold it into the body so the sender still shows. */
  if ([title length] && [n respondsToSelector:@selector(setAlertTitle:)]) {
    [n setAlertBody:body];
    [n setValue:title forKey:@"alertTitle"];
  } else {
    [n setAlertBody:[title length] ? [NSString stringWithFormat:@"%@: %@", title, body] : body];
  }
  [[UIApplication sharedApplication] presentLocalNotificationNow:n];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

/* Set the badge directly on the app, decoupled from any notification. The
 * pragma is house style for these calls — harmless on the 8.4 SDK (not yet
 * deprecated there) and future-proof if the SDK is bumped to iOS 17+, where
 * UNUserNotificationCenter -setBadgeCount: supersedes it. */
- (void)setBadgeCount:(int)count;
{
  UIApplication *app = [UIApplication sharedApplication];
  SEL current = @selector(currentUserNotificationSettings);
  if ([app respondsToSelector:current]) {
    id settings = XPUIKitInvokeObjectGetter(app, current);
    /* Badge=1. Skip writes while permission is pending or badges are disabled,
     * even if the user allowed alerts or sounds. Pre-iOS-8 needs no permission. */
    if (!(XPUIKitInvokeUnsignedIntegerGetter(settings, @selector(types)) & (NSUInteger)1)) return;
  }
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  [app setApplicationIconBadgeNumber:(count > 0) ? count : 0];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

@end
