//
//  XPUIKit.h
//  ENIL
//

#import <UIKit/UIKit.h>

/* Cross-platform UIKit shims — the iOS counterpart to source/macOS/XPAppKit.
 * Each XP_* wrapper hides an availability pragma so caller code stays clean of
 * -Wunguarded-availability against the iOS 8.4 SDK / 4.3 deployment floor, the
 * same way XPAppKit's XP_* category methods hide -Wdeprecated-declarations. */

@interface UIViewController (XPUIKit)
/* Keep fixed-frame content below navigation/status bars on iOS 7+.
 * Earlier iOS versions already lay out this way; leave them unchanged. */
- (void)XP_layoutBelowBars;
/* Informational alert; dismissing it leaves the presenting controller open.
 * Uses UIAlertController on iOS 8+ and UIAlertView on earlier versions. */
- (void)XP_showAlertWithTitle:(NSString *)title
                      message:(NSString *)message
                 dismissTitle:(NSString *)dismissTitle;
@end

@interface UIDevice (XPUIKit)
/* Runtime version check using APIs available at the iOS 4.3 floor. */
- (BOOL)XP_isOperatingSystemAtLeastMajorVersion:(NSInteger)majorVersion;
@end

@interface UIColor (XPUIKit)
/* The shared chat-surface background: RGB(220, 226, 236), a soft blue-grey.
 * Used behind the (now transparent) message and sticker-picker web views so the
 * overscroll bounce reveals this colour rather than UIWebView's white. */
+ (UIColor *)messagesBackgroundColor;
@end

@interface UIView (XPUIKit)
/* Composite a view's background through to whatever sits behind it. UIWebView
 * ships opaque + white (unlike the macOS WKWebView's drawsBackground); both the
 * opaque flag and a clear backgroundColor are required before the host view
 * shows through. Generic for any view — the web views are the only callers. */
- (void)XP_setBackgroundTransparent;

/* Hide a UIWebView scroll view's overscroll drop-shadow without disturbing its
 * scroll indicators. UIWebView layers its scroll view as: gradient shadow
 * UIImageViews (behind) → the UIWebBrowserView content → scroll-indicator
 * UIImageViews (in front). The shadows are exactly the image views preceding the
 * content view, so this hides leading image-view subviews and stops at the first
 * non-image subview — leaving the indicators alone. There is no UIScrollView
 * property for this; hiding the views is the only lever. Call on the scroll view
 * (via XP_scrollView) after each load; the views are rebuilt per content load.
 * Sending this to nil (the 4.3 floor's absent scroll view) is a safe no-op. */
- (void)XP_removeShadow;
@end

@interface UIWebView (XPUIKit)
/* -[UIWebView scrollView] is iOS 5.0+; naming it against the 4.3 floor trips
 * -Wunguarded-availability. Mirrors the original getter and runtime-gates: on
 * the floor the selector is absent and this returns nil,
 * so `[[web XP_scrollView] setDecelerationRate:UIScrollViewDecelerationRateNormal]`
 * is a silent no-op there (a message to nil does nothing). Used to give the scroller the
 * same coast as a UITableView — WebKit shipped UIWebView's scroller with the
 * high-friction "fast" rate (0.99) through iOS 10, so an identical flick stops
 * ~5x sooner than a native table (which uses "normal", 0.998): the source of
 * the "web views scroll slow" feel on iOS 6. */
- (UIScrollView *)XP_scrollView;
@end

@interface UIApplication (XPUIKit)
/* Wraps the deprecated -cancelAllLocalNotifications (iOS 10+ SDK deprecates it;
 * UNUserNotificationCenter is the modern path we intentionally don't link) so
 * callers stay pragma-free, per the XP_<verb> convention. This is also ENIL's
 * best-effort "clear on foreground" lever. HONEST CAVEAT: it cancels only
 * *scheduled* local notifications — it does NOT remove already-*delivered* ones
 * from Notification Center, and ENIL delivers via -presentLocalNotificationNow:
 * (never scheduling), so today it's effectively a no-op. Reliably clearing the
 * delivered tray is a documented follow-up (the badge-to-zero trick). */
- (void)XP_cancelAllLocalNotifications;
@end

/* XPUserNotificationCenter — the iOS counterpart to source/macOS/XPAppKit's
 * class of the same name. Instant local notifications only (a title + body that
 * fires now), built on UILocalNotification. There is deliberately no
 * UNUserNotificationCenter path: the abstraction needed to reach the modern API
 * was not worth it. iOS 8+ gates delivery behind a permission prompt
 * (UIUserNotificationSettings), reached by runtime dispatch so the iOS 4.3 floor
 * stays warning-clean; iOS < 8 has no prompt and is always authorized. */
typedef enum {
  XPNotificationAuthStatusNotDetermined = 0,
  XPNotificationAuthStatusDenied        = 1,
  XPNotificationAuthStatusAuthorized    = 2
} XPNotificationAuthStatus;

@interface XPUserNotificationCenter : NSObject
+ (XPUserNotificationCenter *)defaultCenter;
/* Call each launch; iOS prompts on the first request and remembers the answer. */
- (void)requestAuthorization;
/* On iOS 8+, empty settings include both unrequested and denied permission;
 * this status must not be used to decide whether to register at launch. */
- (XPNotificationAuthStatus)authorizationStatus;
- (void)postNotificationWithTitle:(NSString *)title body:(NSString *)body;
/* Set the app-icon badge to `count` (0 clears it). Separate from the
 * notification post: on iOS the badge is an app-level property
 * (-[UIApplication applicationIconBadgeNumber]), not something the
 * UILocalNotification has to carry — and the clear-on-foreground path needs it
 * with no notification in hand anyway. On iOS 8+ writes require badge permission. */
- (void)setBadgeCount:(int)count;
@end
