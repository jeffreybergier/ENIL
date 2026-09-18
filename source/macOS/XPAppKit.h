#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import "XPFoundation.h"
/* Brings in AICCTier{Legacy,Middle,Modern} + AICCCurrentTier(). XPAppKit's
 * runtime-dispatch wrappers gate on the tier instead of -respondsToSelector:.
 * The cookie cutter header is the single home for the tier definition; both
 * XPAppKit's call sites and the cookie cutter itself read from it. */
#import "AICookieCutterWindowController.h"

/* --- Cross-Platform Compatibility Macros --- */

/* 10.6 Snow Leopard (Formal Protocols) */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1060
  #define XPApplicationDelegate   NSApplicationDelegate
  #define XPTableViewDataSource   NSTableViewDataSource
  #define XPTableViewDelegate     NSTableViewDelegate
  #define XPTextViewDelegate      NSTextViewDelegate
  #define XPTabViewDelegate       NSTabViewDelegate
#else
  @protocol XPApplicationDelegate @end
  @protocol XPTableViewDataSource @end
  @protocol XPTableViewDelegate   @end
  @protocol XPTextViewDelegate    @end
  @protocol XPTabViewDelegate     @end
#endif

/* 10.12 Sierra (Window Masks, Events, Text Alignment) */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 101200
  #define XPWindowStyleMask               NSWindowStyleMask
  #define XPWindowStyleMaskTitled         NSWindowStyleMaskTitled
  #define XPWindowStyleMaskClosable       NSWindowStyleMaskClosable
  #define XPWindowStyleMaskMiniaturizable NSWindowStyleMaskMiniaturizable
  #define XPEventModifierFlagCommand      NSEventModifierFlagCommand
  #define XPEventModifierFlagOption       NSEventModifierFlagOption
  #define XPEventModifierFlagControl      NSEventModifierFlagControl
#else
  #define XPWindowStyleMask               NSUInteger
  #define XPWindowStyleMaskTitled         NSTitledWindowMask
  #define XPWindowStyleMaskClosable       NSClosableWindowMask
  #define XPWindowStyleMaskMiniaturizable NSMiniaturizableWindowMask
  #define XPEventModifierFlagCommand      NSCommandKeyMask
  #define XPEventModifierFlagOption       NSAlternateKeyMask
  #define XPEventModifierFlagControl      NSControlKeyMask
#endif

/* 10.12 Sierra (Text Alignment) */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 101200
  #define XPTextAlignmentCenter NSTextAlignmentCenter
  #define XPCompositingOperationSourceOver NSCompositingOperationSourceOver
#else
  #define XPTextAlignmentCenter NSCenterTextAlignment
  #define XPCompositingOperationSourceOver NSCompositeSourceOver
#endif

/* 10.10 Yosemite (NSImageView scaling — NSScaleToFit deprecated) */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 101000
  #define XPImageScaleAxesIndependently NSImageScaleAxesIndependently
#else
  #define XPImageScaleAxesIndependently NSScaleToFit
#endif

/* 10.14 Mojave (Bezel, Progress Styles, Button Types) */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 101400
  #define XPBezelStyleRounded              NSBezelStyleRounded
  #define XPBezelStyleTexturedSquare       NSBezelStyleTexturedSquare
  #define XPProgressIndicatorStyleBar      NSProgressIndicatorStyleBar
  #define XPButtonTypeMomentaryLight       NSButtonTypeMomentaryLight
  #define XPButtonTypeRadio                NSButtonTypeRadio
#else
  #define XPBezelStyleRounded              NSRoundedBezelStyle
  #define XPBezelStyleTexturedSquare       NSTexturedSquareBezelStyle
  #define XPProgressIndicatorStyleBar      NSProgressIndicatorBarStyle
  #define XPButtonTypeMomentaryLight       NSMomentaryLightButton
  #define XPButtonTypeRadio                NSRadioButton
#endif

/* 10.13 High Sierra (control state names). */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 101300
  #define XPControlStateOn  NSControlStateValueOn
  #define XPControlStateOff NSControlStateValueOff
#else
  #define XPControlStateOn  NSOnState
  #define XPControlStateOff NSOffState
#endif

/* Suppress deprecated-declarations for APIs that are deprecated on modern SDKs
   but are the correct choice for our Tiger/Leopard target. */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
typedef NSDrawerState XPDrawerState;
static const XPDrawerState XPDrawerOpeningState = NSDrawerOpeningState;
static const XPDrawerState XPDrawerOpenState = NSDrawerOpenState;
@interface XPDrawer : NSDrawer
@end
#ifdef __clang__
#pragma clang diagnostic pop
#endif

/* 10.6 Snow Leopard (Formal Protocols) */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1060
  #define XPToolbarDelegate    NSToolbarDelegate
#else
  @protocol XPToolbarDelegate   @end
#endif

/* Runtime OS dispatch is done through AICCCurrentTier() from
 * AICookieCutterWindowController.h. The three-bucket tier system (Legacy /
 * Middle / Modern) replaces the older XP_appKitIsAtLeast() free-form
 * literal compares; callers tier-check once and pick an API path. */

/* 11.0 Big Sur (Toolbar Style) */
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 110000
  #define XPWindowToolbarStyleUnified        NSWindowToolbarStyleUnified
  #define XPWindowToolbarStyleUnifiedCompact NSWindowToolbarStyleUnifiedCompact
#else
  #define XPWindowToolbarStyleUnified        3
  #define XPWindowToolbarStyleUnifiedCompact 4
#endif

/* 11.0 Big Sur (Deprecated named colors) */
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 101400
  #define XPColorControlHighlight [NSColor separatorColor]
#else
  #define XPColorControlHighlight [NSColor controlHighlightColor]
#endif

#if MAC_OS_X_VERSION_MIN_REQUIRED >= 110000
  #define XPColorWindowFrame [NSColor windowBackgroundColor]
#else
  #define XPColorWindowFrame [NSColor windowFrameColor]
#endif

@interface NSSplitView (XPAppKit)
- (void)XP_setAutosaveName:(NSString *)name;
@end

@interface NSScrollView (XPAppKit)
- (void)XP_setAutomaticallyAdjustsContentInsets:(BOOL)flag;
/* Manual content insets (NSEdgeInsets) — pair with
 * XP_setAutomaticallyAdjustsContentInsets:NO, otherwise AppKit overwrites
 * them. Lets an edge-to-edge scroll view reserve a top strip for the toolbar
 * and a bottom strip for an overlapping bar. No-op on Legacy. */
- (void)XP_setContentInsetsTop:(CGFloat)top
                          left:(CGFloat)left
                        bottom:(CGFloat)bottom
                         right:(CGFloat)right;
@end

/* Layer-backed chrome shims. AppKit's per-view CALayer is always opt-in:
 * a view only has its own [self layer] after setWantsLayer:YES is called
 * ON THAT VIEW. Ancestor opt-in (e.g. AICookieCutterWindowController's
 * contentView call) enables uniform layer compositing for the subtree but
 * does NOT give descendants their own layers — that opt-in is a separate
 * concern (WebView + native control Z-order / hit-testing). So any view
 * that wants cornerRadius / masksToBounds / borderColor / etc. must first
 * call XP_setWantsLayer:YES on itself; only then will the XP_setLayer*
 * setters find a non-nil layer to configure.
 * On Legacy the tier gate inside each shim short-circuits before the call,
 * so callers can invoke these unconditionally and provide their own
 * non-layer fallback in -drawRect:.
 * Each shim hides the [self layer] hop and any CALayer/CGColor imports from
 * callers — we never link QuartzCore directly, so all dispatch goes through
 * typed objc_msgSend on selectors that AppKit forwards to the layer's class. */
@interface NSView (XPAppKitLayer)
/* Materialises a CALayer for this view immediately. Must be called before
 * any XP_setLayer* shim on the same view — otherwise [self layer] is nil
 * and the setters silently no-op. */
- (void)XP_setWantsLayer:(BOOL)flag;
- (void)XP_setLayerCornerRadius:(CGFloat)radius;
- (void)XP_setLayerMasksToBounds:(BOOL)flag;
- (void)XP_setLayerBorderWidth:(CGFloat)width;
- (void)XP_setLayerBorderColor:(NSColor *)color;
@end

@interface NSWindow (XPAppKit)
- (BOOL)XP_enableModernSidebarAppearance;
- (void)XP_setToolbarUnifiedStyle;
- (CGFloat)XP_titlebarHeight;
- (CGFloat)XP_backingScaleFactor;
/* nil-safe title setter — modern AppKit asserts on nil titles, Tiger
 * tolerates them. Coerces nil to @"" so callers never have to guard. */
- (void)XP_setTitle:(NSString *)title;
/* NSWindow.subtitle is 11.0+ (AICCTierModern). No-op on Legacy/Middle. */
- (void)XP_setSubtitle:(NSString *)subtitle;
/* Title + subtitle in one call. On Modern this is the native two-line
 * titlebar (setTitle: + setSubtitle:); on Legacy/Middle — where there is no
 * subtitle slot — the subtitle is folded into the title as "Title — Subtitle"
 * instead of being dropped. An empty/nil subtitle yields the bare title on
 * every tier. */
- (void)XP_setTitle:(NSString *)title subtitle:(NSString *)subtitle;
/* setRepresentedURL: is 10.5+ — that boundary sits inside AICCTierLegacy
 * (which spans 10.4-10.10), so the dispatch uses -respondsToSelector:
 * rather than the tier gate. Pre-10.5 falls back to setRepresentedFilename:
 * with the URL's path. */
- (void)XP_setRepresentedURL:(NSURL *)url;
@end

@interface NSView (XPAppKit)
/* Pin this view's subtree to the window's non-vibrant appearance. Needed for a
 * bar that lives inside the modern sidebar split item, which AppKit wraps in a
 * vibrant .sidebar NSVisualEffectView — see the implementation note. */
- (void)XP_pinToWindowAppearance;
@end

/* -[NSSegmentedControl setToolTip:forSegment:] is 10.13+. Wrap the runtime
 * dispatch so callers don't sprout @available checks or NSInvocation. No-op
 * when the selector is missing (Tiger / pre-10.13), so callers can pair the
 * tooltip with an image-vs-label fallback at the same time. */
@interface NSSegmentedControl (XPAppKit)
- (void)XP_setToolTip:(NSString *)tip forSegment:(XPInteger)segment;
@end

/* NSToolbarSeparatorItemIdentifier is deprecated in 11.0 and ignored on 10.7+,
 * but we still need it for Tiger/Leopard. Wrap so callers don't need pragmas. */
static inline NSString *_XP_NSToolbarSeparatorItemIdentifier(void) {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  return NSToolbarSeparatorItemIdentifier;
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}
#define XPToolbarSeparatorItemIdentifier _XP_NSToolbarSeparatorItemIdentifier()

@interface XPSplitView : NSSplitView
@end

/* 10.13 High Sierra (modal response constants).
 * Both names map to the same integer value; the modern name is only chosen
 * when the SDK declares it. */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 101300
  #define XPModalResponseOK NSModalResponseOK
#else
  #define XPModalResponseOK NSOKButton
#endif

@interface NSAlert (XPAppKit)
- (void)XP_beginSheetModalForWindow:(NSWindow *)window
                      modalDelegate:(id)modalDelegate
                     didEndSelector:(SEL)didEndSelector
                        contextInfo:(void *)contextInfo;
@end

@interface NSOpenPanel (XPAppKit)
- (void)XP_beginSheetForWindow:(NSWindow *)docWindow
                         types:(NSArray *)types
                 modalDelegate:(id)modalDelegate
                didEndSelector:(SEL)didEndSelector
                   contextInfo:(void *)contextInfo;
@end

/* 11.0 Big Sur (Font Text Styles) */
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 110000
  #define XPFontTextStyleBody       [NSFont preferredFontForTextStyle:NSFontTextStyleBody options:@{}]
  #define XPFontTextStyleBoldBody   [NSFont preferredFontForTextStyle:NSFontTextStyleHeadline options:@{}]
  #define XPFontTextStyleChatName   [NSFont preferredFontForTextStyle:NSFontTextStyleCallout options:@{}]
  #define XPFontTextStyleSmallLabel [NSFont preferredFontForTextStyle:NSFontTextStyleCaption2 options:@{}]
  #define XPFontTextStyleBoldSmall  [NSFont preferredFontForTextStyle:NSFontTextStyleFootnote options:@{}]
#else
  #define XPFontTextStyleBody       [NSFont systemFontOfSize:13.0]
  #define XPFontTextStyleBoldBody   [NSFont boldSystemFontOfSize:13.0]
  #define XPFontTextStyleChatName   [NSFont systemFontOfSize:14.0]
  #define XPFontTextStyleSmallLabel [NSFont systemFontOfSize:10.0]
  #define XPFontTextStyleBoldSmall  [NSFont boldSystemFontOfSize:11.0]
#endif

/* 11.0 Big Sur (Table View Style) — runtime path uses @available in XP_setSourceListStyle */
#define XPTableViewStyleSourceList 3

@interface NSTableView (XPAppKit)
- (void)XP_setFloatsGroupRows:(BOOL)floats;
- (void)XP_setSourceListStyle;
@end

@interface NSFileWrapper (XPAppKit)
+ (NSFileWrapper *)XP_fileWrapperWithURL:(NSURL *)url;
@end

@interface NSImage (XPAppKit)
- (void)XP_drawInRect:(NSRect)rect respectingFlippedView:(BOOL)viewIsFlipped;
@end

/* Instant local notifications via NSUserNotification (macOS 10.8+), the legacy
 * API. UNUserNotificationCenter is intentionally NOT used: on modern macOS it
 * silently refuses to deliver without a trusted code signature, which the
 * OSXCross build lacks. Every deprecated NSUserNotification* call is dispatched
 * through typed objc_msgSend inside the .m, so no deprecation warning reaches a
 * call site and the file still compiles against the 10.5 SDK (ppc/x86 slices),
 * where the classes are absent — there it no-ops at runtime (NSClassFromString
 * is nil on Tiger/Leopard, which never had notifications). NSUserNotification
 * has no permission prompt, so there is nothing to request up front. */
@interface XPUserNotificationCenter : NSObject
+ (XPUserNotificationCenter *)defaultCenter;
- (void)postNotificationWithTitle:(NSString *)title body:(NSString *)body;
/* Clear every banner this app has placed in Notification Center. Called when
 * ENIL comes to the front, since banners only accrue while it isn't frontmost. */
- (void)removeAllDelivered;
/* Set the Dock-tile badge to `count` (0 clears it). Separate from the
 * notification API: NSUserNotification has no badge concept on macOS — the
 * badge is the Dock tile's string label (NSDockTile, 10.5+). No-ops on Tiger
 * (10.4), which predates NSDockTile; pre-10.5 apps badged by compositing onto
 * -[NSApplication setApplicationIconImage:] instead. */
- (void)setBadgeCount:(int)count;
@end
