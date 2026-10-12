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
