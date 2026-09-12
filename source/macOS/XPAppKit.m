#import "XPAppKit.h"
#import <objc/runtime.h>
#import <objc/message.h>

/* Every runtime check in this file gates on AICCCurrentTier() rather than
 * -respondsToSelector: / NSClassFromString. The tier is the existence proof;
 * NSInvocation (for struct returns / Tiger-SDK-missing types) or typed
 * objc_msgSend casts (for Tiger-compatible argument types) are just the
 * dispatch mechanism — they assume the selector is present because the tier
 * said so. Mapping policy: APIs whose introduction matches a tier boundary
 * (10.11, 11.0) map directly. APIs whose introduction sits inside a tier
 * (10.5/10.6/10.7/10.10/10.13) are gated at the NEXT tier up — we prefer
 * the older code path on the lower tier rather than risking a missing-
 * selector crash mid-tier. */

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
@implementation XPDrawer
@end
#ifdef __clang__
#pragma clang diagnostic pop
#endif

@implementation NSSplitView (XPAppKit)

/* -setAutosaveName: arrived in 10.5, sub-Legacy. Conservative tier mapping:
 * only call from Middle/Modern, no-op on Legacy (loses divider persistence
 * on 10.4-10.10 but never crashes). */
- (void)XP_setAutosaveName:(NSString *)name {
  if (AICCCurrentTier() < AICCTierMiddle) return;
  ((void (*)(id, SEL, NSString *))objc_msgSend)
    (self, @selector(setAutosaveName:), name);
}

@end

@implementation NSScrollView (XPAppKit)

/* -setAutomaticallyAdjustsContentInsets: is 10.7+ (sub-Legacy → Middle gate). */
- (void)XP_setAutomaticallyAdjustsContentInsets:(BOOL)flag;
{
  if (AICCCurrentTier() < AICCTierMiddle) return;
  ((void (*)(id, SEL, BOOL))objc_msgSend)
    (self, @selector(setAutomaticallyAdjustsContentInsets:), flag);
}

/* -setContentInsets: is 10.10+ (sub-Legacy → Middle gate). NSEdgeInsets is a
 * four-CGFloat struct the Tiger SDK doesn't declare, so build a matching
 * local struct and pass it positionally through NSInvocation (same approach
 * as AIWebViewController's aiDisableLegacyAutoContentInsets). */
- (void)XP_setContentInsetsTop:(CGFloat)top
                          left:(CGFloat)left
                        bottom:(CGFloat)bottom
                         right:(CGFloat)right;
{
  struct { CGFloat top, left, bottom, right; } insets;
  SEL sel = @selector(setContentInsets:);
  NSMethodSignature *sig;
  NSInvocation *inv;
  if (AICCCurrentTier() < AICCTierMiddle) return;
  insets.top = top; insets.left = left; insets.bottom = bottom; insets.right = right;
  sig = [self methodSignatureForSelector:sel];
  if (!sig) return;
  inv = [NSInvocation invocationWithMethodSignature:sig];
  [inv setTarget:self];
  [inv setSelector:sel];
  [inv setArgument:&insets atIndex:2];
  [inv invoke];
}

@end

@implementation NSView (XPAppKitLayer)

/* Each layer shim follows the same two-hop pattern: typed objc_msgSend to
 * grab [self layer], then a second typed objc_msgSend on the layer for the
 * actual setter. We stay header-free of QuartzCore (no -framework
 * QuartzCore in the Makefile) — CALayer is 10.5+ and -[NSColor CGColor] is
 * 10.8+, both universally present on Middle+ which is where these shims
 * apply. The tier gate at the top of each shim short-circuits on Legacy
 * before any of the 10.5+/10.8+ selectors are looked up. */

/* setWantsLayer: also gates on tier so Legacy doesn't see the 10.5+ selector.
 * Per-view CALayers are opt-in in AppKit: every view that wants its own
 * layer must call this on itself — ancestor layer-backing does not
 * propagate. After this call, [self layer] returns a non-nil layer that
 * subsequent XP_setLayer* calls can configure. */
- (void)XP_setWantsLayer:(BOOL)flag;
{
  if (AICCCurrentTier() < AICCTierMiddle) return;
  ((void (*)(id, SEL, BOOL))objc_msgSend)
    (self, @selector(setWantsLayer:), flag);
}

- (void)XP_setLayerCornerRadius:(CGFloat)radius;
{
  id layer;
  if (AICCCurrentTier() < AICCTierMiddle) return;
  layer = ((id (*)(id, SEL))objc_msgSend)(self, @selector(layer));
  if (!layer) return;
  ((void (*)(id, SEL, CGFloat))objc_msgSend)
    (layer, @selector(setCornerRadius:), radius);
}

- (void)XP_setLayerMasksToBounds:(BOOL)flag;
{
  id layer;
  if (AICCCurrentTier() < AICCTierMiddle) return;
  layer = ((id (*)(id, SEL))objc_msgSend)(self, @selector(layer));
  if (!layer) return;
  ((void (*)(id, SEL, BOOL))objc_msgSend)
    (layer, @selector(setMasksToBounds:), flag);
}

- (void)XP_setLayerBorderWidth:(CGFloat)width;
{
  id layer;
  if (AICCCurrentTier() < AICCTierMiddle) return;
  layer = ((id (*)(id, SEL))objc_msgSend)(self, @selector(layer));
  if (!layer) return;
  ((void (*)(id, SEL, CGFloat))objc_msgSend)
    (layer, @selector(setBorderWidth:), width);
}

/* CGColor is a CF type (CGColorRef = struct CGColor *). Typed objc_msgSend
 * carries the pointer cleanly across every arch — no struct-return hazard. */
- (void)XP_setLayerBorderColor:(NSColor *)color;
{
  id layer;
  CGColorRef cg;
  if (AICCCurrentTier() < AICCTierMiddle) return;
  if (!color) return;
  layer = ((id (*)(id, SEL))objc_msgSend)(self, @selector(layer));
  if (!layer) return;
  cg = ((CGColorRef (*)(id, SEL))objc_msgSend)
         (color, @selector(CGColor));
  if (!cg) return;
  ((void (*)(id, SEL, CGColorRef))objc_msgSend)
    (layer, @selector(setBorderColor:), cg);
}

@end

@implementation NSWindow (XPAppKit)

/* -setStyleMask: + NSWindowStyleMaskFullSizeContentView are both inside the
 * Legacy band (10.6 and 10.10). Tier gate: Middle/Modern only. */
- (BOOL)XP_enableModernSidebarAppearance;
{
  NSUInteger fullSize = (NSUInteger)(1 << 15); /* NSWindowStyleMaskFullSizeContentView */
  NSUInteger mask;

  if (AICCCurrentTier() < AICCTierMiddle) return NO;

  mask = [self styleMask] | fullSize;
  ((void (*)(id, SEL, NSUInteger))objc_msgSend)
    (self, @selector(setStyleMask:), mask);
  return YES;
}

/* -contentLayoutRect is 10.10+ (sub-Legacy → Middle gate). NSRect struct
 * return forces NSInvocation regardless of tier. */
- (CGFloat)XP_titlebarHeight;
{
  NSMethodSignature *sig;
  NSInvocation *inv;
  NSRect layoutRect;

  if (AICCCurrentTier() < AICCTierMiddle) return 0.0;
  sig = [self methodSignatureForSelector:@selector(contentLayoutRect)];
  inv = [NSInvocation invocationWithMethodSignature:sig];
  [inv setTarget:self];
  [inv setSelector:@selector(contentLayoutRect)];
  [inv invoke];
  [inv getReturnValue:&layoutRect];
  return NSHeight([[self contentView] bounds]) - NSHeight(layoutRect);
}

/* -setToolbarStyle: is 11.0+ — clean Modern-tier boundary. */
- (void)XP_setToolbarUnifiedStyle;
{
  NSInteger unified = XPWindowToolbarStyleUnified;
  if (AICCCurrentTier() < AICCTierModern) return;
  ((void (*)(id, SEL, NSInteger))objc_msgSend)
    (self, @selector(setToolbarStyle:), unified);
}

/* -backingScaleFactor is 10.7+ (sub-Legacy → Middle gate). Pre-HiDPI Macs
 * are always 1.0. CGFloat return → NSInvocation. */
- (CGFloat)XP_backingScaleFactor;
{
  NSMethodSignature *sig;
  NSInvocation *inv;
  CGFloat scale = 1.0;

  if (AICCCurrentTier() < AICCTierMiddle) return 1.0;
  sig = [self methodSignatureForSelector:@selector(backingScaleFactor)];
  inv = [NSInvocation invocationWithMethodSignature:sig];
  [inv setTarget:self];
  [inv setSelector:@selector(backingScaleFactor)];
  [inv invoke];
  [inv getReturnValue:&scale];
  return (scale > 0.0) ? scale : 1.0;
}

- (void)XP_setTitle:(NSString *)title;
{
  [self setTitle:(title ? title : @"")];
}

/* -setSubtitle: is 11.0+ — clean Modern-tier boundary, mirrors
 * XP_setToolbarUnifiedStyle above. */
- (void)XP_setSubtitle:(NSString *)subtitle;
{
  if (AICCCurrentTier() < AICCTierModern) return;
  ((void (*)(id, SEL, NSString *))objc_msgSend)
    (self, @selector(setSubtitle:), subtitle ? subtitle : @"");
}

- (void)XP_setTitle:(NSString *)title subtitle:(NSString *)subtitle;
{
  NSString *t = title ? title : @"";
  NSString *s = subtitle ? subtitle : @"";
  if (AICCCurrentTier() >= AICCTierModern) {
    [self setTitle:t];
    [self XP_setSubtitle:s];
    return;
  }
  /* No subtitle slot below Modern — fold it into the title. Separator is
   * " <U+2014> " (em dash) written as raw UTF-8 bytes, matching the project's
   * escape style for non-ASCII string literals (see SyncMiniViewController). */
  if ([s length]) {
    t = [NSString stringWithFormat:@"%@%@%@",
         t, [NSString stringWithUTF8String:" \xE2\x80\x94 "], s];
  }
  [self setTitle:t];
}

/* -setRepresentedURL: is 10.5+ — sits mid-Legacy, so dispatch via
 * -respondsToSelector: instead of a tier gate. Tiger (10.4) falls through
 * to -setRepresentedFilename: with the URL's filesystem path. */
- (void)XP_setRepresentedURL:(NSURL *)url;
{
  SEL urlSel = @selector(setRepresentedURL:);
  if ([self respondsToSelector:urlSel]) {
    ((void (*)(id, SEL, NSURL *))objc_msgSend)(self, urlSel, url);
    return;
  }
  [self setRepresentedFilename:(url ? [url path] : @"")];
}

@end

@implementation NSView (XPAppKit)

/* A bar dropped into the sidebar split item (sidebarWithViewController:) is
 * wrapped by AppKit in a vibrant .sidebar NSVisualEffectView, so its subtree
 * inherits a vibrant appearance and semantic colors (separatorColor /
 * windowBackgroundColor) resolve to washed-out variants — the bar's top
 * separator stops matching the same bar drawn in a plain content pane. Pinning
 * to the window's own appearance keeps the bar solid and content-pane-matched,
 * while still following dark mode (effectiveAppearance carries it). Re-applied
 * on every window attach.
 *
 * Modern-only (11.0+). The mismatch needs the prominent vibrant sidebar AND a
 * separatorColor top line — and that line is only compiled into the arm64 slice
 * (the lone min >= 10.14 build; the others draw the old controlHighlightColor).
 * arm64 runs solely on Apple Silicon, which is always Modern, so below Modern
 * there is nothing to fix. The gate also guarantees effectiveAppearance (10.14+)
 * exists, so no name-based Aqua fallback is needed. */
- (void)XP_pinToWindowAppearance;
{
  id appearance;
  NSWindow *win = [self window];

  if (AICCCurrentTier() < AICCTierModern) return;
  if (!win) return;
  if (![self respondsToSelector:@selector(setAppearance:)]) return;

  appearance = ((id (*)(id, SEL))objc_msgSend)
      (win, @selector(effectiveAppearance));
  if (!appearance) return;
  ((void (*)(id, SEL, id))objc_msgSend)
      (self, @selector(setAppearance:), appearance);
}

@end

@implementation XPSplitView

/* -setDividerStyle: is 10.5+ (sub-Legacy → Middle gate). Below Middle we
 * fall through to the manual divider draw the legacy NSSplitView always
 * supported. */
- (id)initWithFrame:(NSRect)frame;
{
  self = [super initWithFrame:frame];
  if (!self)
    return nil;

  if (AICCCurrentTier() >= AICCTierMiddle)
    [self setDividerStyle:NSSplitViewDividerStyleThin];

  return self;
}

- (CGFloat)dividerThickness;
{
  return (AICCCurrentTier() >= AICCTierMiddle)
       ? [super dividerThickness]
       : 1.0;
}

- (void)drawDividerInRect:(NSRect)rect;
{
  if (AICCCurrentTier() >= AICCTierMiddle) {
    [super drawDividerInRect:rect];
    return;
  }
  [XPColorWindowFrame set];
  NSRectFill(rect);
}

@end

@implementation NSAlert (XPAppKit)

- (void)XP_beginSheetModalForWindow:(NSWindow *)window
                      modalDelegate:(id)modalDelegate
                     didEndSelector:(SEL)didEndSelector
                        contextInfo:(void *)contextInfo;
{
  if (!window) return;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  [self beginSheetModalForWindow:window
                   modalDelegate:modalDelegate
                  didEndSelector:didEndSelector
                     contextInfo:contextInfo];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

@end

@implementation NSOpenPanel (XPAppKit)

- (void)XP_beginSheetForWindow:(NSWindow *)docWindow
                         types:(NSArray *)types
                 modalDelegate:(id)modalDelegate
                didEndSelector:(SEL)didEndSelector
                   contextInfo:(void *)contextInfo;
{
  if (!docWindow) return;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  [self beginSheetForDirectory:nil
                          file:nil
                         types:types
                modalForWindow:docWindow
                 modalDelegate:modalDelegate
                didEndSelector:didEndSelector
                   contextInfo:contextInfo];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

@end

@implementation NSTableView (XPAppKit)

/* -setFloatsGroupRows: is 10.7+ (sub-Legacy → Middle gate). */
- (void)XP_setFloatsGroupRows:(BOOL)floats {
  if (AICCCurrentTier() < AICCTierMiddle) return;
  ((void (*)(id, SEL, BOOL))objc_msgSend)
    (self, @selector(setFloatsGroupRows:), floats);
}

/* Two API floors here:
 *   - -setStyle: is 11.0+ — Modern gate, takes XPTableViewStyleSourceList (3)
 *   - -setSelectionHighlightStyle: is 10.5+ — Middle gate, takes
 *     NSTableViewSelectionHighlightStyleSourceList
 * Legacy: no-op (no source-list look on 10.4-10.10). */
- (void)XP_setSourceListStyle {
  AICCTier tier = AICCCurrentTier();
  if (tier == AICCTierModern) {
    NSInteger value = XPTableViewStyleSourceList;
    ((void (*)(id, SEL, NSInteger))objc_msgSend)
      (self, @selector(setStyle:), value);
  } else if (tier == AICCTierMiddle) {
    NSInteger value = NSTableViewSelectionHighlightStyleSourceList;
    ((void (*)(id, SEL, NSInteger))objc_msgSend)
      (self, @selector(setSelectionHighlightStyle:), value);
  }
}

@end

/* Forward-declare the 10.6+ initialiser so GCC on the 10.5 SDK knows the
   method signature. The tier guard below ensures the call is never made on
   Legacy at runtime. */
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1060
@interface NSFileWrapper (XPAppKit_ModernInit)
- (id)initWithURL:(NSURL *)url options:(NSUInteger)options error:(NSError **)outError;
@end
#endif

@implementation NSFileWrapper (XPAppKit)

/* -initWithURL:options:error: is 10.6+ (sub-Legacy → Middle gate). */
+ (NSFileWrapper *)XP_fileWrapperWithURL:(NSURL *)url;
{
  if (!url) return nil;

  if (AICCCurrentTier() >= AICCTierMiddle) {
    return [[[NSFileWrapper alloc] initWithURL:url options:0 error:nil] autorelease];
  }

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  return [[[NSFileWrapper alloc] initWithPath:[url path]] autorelease];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

@end

@implementation NSImage (XPAppKit)

- (void)XP_drawInRect:(NSRect)rect respectingFlippedView:(BOOL)viewIsFlipped;
{
  NSGraphicsContext *ctx = [NSGraphicsContext currentContext];
  NSAffineTransform *xform;

  [ctx saveGraphicsState];
  if (viewIsFlipped) {
    xform = [NSAffineTransform transform];
    [xform translateXBy:0.0 yBy:NSMaxY(rect) + NSMinY(rect)];
    [xform scaleXBy:1.0 yBy:-1.0];
    [xform concat];
  }
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  [self drawInRect:rect
          fromRect:NSZeroRect
         operation:XPCompositingOperationSourceOver
          fraction:1.0];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  [ctx restoreGraphicsState];
}

@end

@implementation NSSegmentedControl (XPAppKit)

/* -setToolTip:forSegment: is 10.13+, which sits inside the Middle band
 * (10.11-10.15). Under tier-only dispatch we'd risk a missing selector on
 * 10.11/10.12, so we gate at Modern. Cost: no per-segment tooltips on
 * Middle (10.11-10.15); the segment's image/label still tells the user
 * what it does. */
- (void)XP_setToolTip:(NSString *)tip forSegment:(XPInteger)segment;
{
  if (AICCCurrentTier() < AICCTierModern) return;
  ((void (*)(id, SEL, NSString *, XPInteger))objc_msgSend)
    (self, @selector(setToolTip:forSegment:), tip, segment);
}

@end

@implementation XPUserNotificationCenter

+ (XPUserNotificationCenter *)defaultCenter;
{
  static XPUserNotificationCenter *instance = nil;
  if (!instance) instance = [[XPUserNotificationCenter alloc] init];
  return instance;
}

/* NSUserNotification(Center) is 10.8+ and deprecated on modern SDKs. Every call
 * here takes 0-1 object args and returns void/id, so plain performSelector:
 * dispatches it — no objc_msgSend cast or NSInvocation needed. Going through
 * NSClassFromString + selectors also lets this compile against the 10.5 SDK
 * without naming the missing classes, and keeps any deprecation warning away
 * from the caller. A nil class (Tiger/Leopard) or nil body no-ops. No delegate
 * is set: the center's default presentation rule then applies — a banner shows
 * only when ENIL is NOT the frontmost app, and is suppressed while it is (a
 * shouldPresentNotification: delegate returning YES would override that to
 * always banner). */
- (void)postNotificationWithTitle:(NSString *)title body:(NSString *)body;
{
  Class centerCls = NSClassFromString(@"NSUserNotificationCenter");
  Class noteCls   = NSClassFromString(@"NSUserNotification");
  id center;
  id note;

  if (!centerCls || !noteCls || !body) return;

  center = [(id)centerCls performSelector:@selector(defaultUserNotificationCenter)];
  if (!center) return;

  note = [[[noteCls alloc] init] autorelease];
  [note performSelector:@selector(setTitle:) withObject:(title ? title : @"")];
  [note performSelector:@selector(setInformativeText:) withObject:body];
#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED >= 1080
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  [note performSelector:@selector(setSoundName:) withObject:NSUserNotificationDefaultSoundName];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#endif
  [center performSelector:@selector(deliverNotification:) withObject:note];
}

/* removeAllDeliveredNotifications is also 10.8+ and deprecated on modern SDKs;
 * it takes no args and returns void, so performSelector: dispatches it cleanly,
 * matching -postNotificationWithTitle:body: above. A nil class (Tiger/Leopard,
 * which never had a notification center) or nil center no-ops. */
- (void)removeAllDelivered;
{
  Class centerCls = NSClassFromString(@"NSUserNotificationCenter");
  id center;

  if (!centerCls) return;

  center = [(id)centerCls performSelector:@selector(defaultUserNotificationCenter)];
  if (!center) return;

  [center performSelector:@selector(removeAllDeliveredNotifications)];
}

/* NSDockTile (10.5+) exists in the 10.5 build SDK but not on the 10.4 runtime,
 * so a respondsToSelector: guard + direct call is the sanctioned pattern: it
 * compiles type-checked and no-ops on Tiger. The badge is a string label, so
 * we format the count; 0 maps to @"" which removes the badge. */
- (void)setBadgeCount:(int)count;
{
  if (![NSApp respondsToSelector:@selector(dockTile)]) return;
  NSString *label = (count > 0) ? [NSString stringWithFormat:@"%d", count] : @"";
  [[NSApp dockTile] setBadgeLabel:label];
}

@end
