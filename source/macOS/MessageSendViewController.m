#import "MessageSendViewController.h"
#import "ENILResponderActions.h"
#import "ENILAccount.h"
#import "ENILBottomToolbarView.h"
#import "AIFontAwesome.h"
#import "XPAppKit.h"
#import <objc/message.h>
#import "XPFoundation.h"

static const CGFloat kBarHeightCollapsed = 32.0;
static const CGFloat kBarHeightExpanded  = 108.0;
static const CGFloat kPad                = 4.0;

/* Actions segmented control — three momentary segments at the right edge of
 * the bar. Send carries both an icon and the "Send" label; the others are
 * icon-only with tooltips. */
enum {
  kActionsSegClose    = 0,
  kActionsSegMarkSeen = 1,
  kActionsSegSend     = 2
};

static NSColor *InputBezelBackgroundColor(void) { return [NSColor controlBackgroundColor]; }
static NSColor *InputBezelBorderColor(void) { return [NSColor gridColor]; }
static NSColor *InputBezelHighlightColor(void) { return XPColorControlHighlight; }

@interface SticonAttachmentCell : NSTextAttachmentCell {
 @private
  CGFloat baselineOffset_;
}
- (id)initWithBaselineOffset:(CGFloat)offset;
@end

@implementation SticonAttachmentCell

- (id)initWithBaselineOffset:(CGFloat)offset;
{
  if ((self = [super init])) {
    baselineOffset_ = offset;
  }
  return self;
}

- (NSPoint)cellBaselineOffset;
{
  NSPoint offset = [super cellBaselineOffset];
  offset.y += baselineOffset_;
  return offset;
}

@end

/* Inline sticon attachment — stores identity for use at send time. */
@interface SticonAttachment : NSTextAttachment {
 @private
  NSString *packageId_;
  NSString *sticonId_;
  NSString *altText_;
}
- (id)initWithPackageId:(NSString *)pkgId
              sticonId:(NSString *)sid
               altText:(NSString *)alt
                   url:(NSURL *)url;
- (NSString *)packageId;
- (NSString *)sticonId;
- (NSString *)altText;
@end

@implementation SticonAttachment

static const CGFloat kSticonAttachmentHeight = 18.0;
static const CGFloat kSticonBaselineOffsetY = -2.0;

static void SticonAttachmentResizeCellImage(NSTextAttachment *attachment) {
  id cell = [attachment attachmentCell];
  if (![cell respondsToSelector:@selector(image)] ||
      ![cell respondsToSelector:@selector(setImage:)]) {
    ENILLog(@"SticonAttachment.resize", @"cell does not respond to selector image=%d setImage=%d",
          [cell respondsToSelector:@selector(image)],
          [cell respondsToSelector:@selector(setImage:)]);
    return;
  }

  NSImage *image = [cell image];
  NSSize size = [image size];
  if (size.width <= 0.0 || size.height <= 0.0) {
    ENILLog(@"SticonAttachment.resize", @"image size invalid width=%f height=%f",
          size.width, size.height);
    return;
  }

  CGFloat scale = kSticonAttachmentHeight / size.height;
  [image setSize:NSMakeSize(floor(size.width * scale),
                            kSticonAttachmentHeight)];
  [cell setImage:image];
}

static void SticonAttachmentSetBaselineCell(NSTextAttachment *attachment) {
  id oldCell = [attachment attachmentCell];
  if (![oldCell respondsToSelector:@selector(image)]) {
    ENILLog(@"SticonAttachment.baseline", @"cell does not respond to selector image=%d",
          [oldCell respondsToSelector:@selector(image)]);
    return;
  }

  SticonAttachmentCell *cell = [[SticonAttachmentCell alloc]
      initWithBaselineOffset:kSticonBaselineOffsetY];
  [cell setImage:[oldCell image]];
  [attachment setAttachmentCell:cell];
  [cell release];
}

- (id)initWithPackageId:(NSString *)pkgId
               sticonId:(NSString *)sid
                altText:(NSString *)alt
                    url:(NSURL *)url;
{
  NSFileWrapper *wrapper = [NSFileWrapper XP_fileWrapperWithURL:url];
  if ((self = [super initWithFileWrapper:wrapper])) {
    SticonAttachmentResizeCellImage(self);
    SticonAttachmentSetBaselineCell(self);
    packageId_ = [pkgId copy];
    sticonId_  = [sid   copy];
    altText_   = [alt   copy];
  }
  return self;
}

- (NSString *)packageId { return packageId_; }
- (NSString *)sticonId  { return sticonId_;  }
- (NSString *)altText   { return altText_;   }

- (void)dealloc;
{
  [packageId_ release];
  [sticonId_  release];
  [altText_   release];
  [super dealloc];
}

@end

/* ── Custom view that draws a recessed bezel around the text input ──
 * Two visual paths: Middle/Modern gets rounded corners + a border painted
 * by this view's own CALayer, materialised by XP_setWantsLayer:YES and
 * configured via XP_setLayer*: in MessageSendViewController.viewDidLoad.
 * masksToBounds on the bezel clips the inner scrollView/clipView/textView
 * to the rounded shape. Legacy has no layer, so we keep the original
 * rectangular fill + frame + highlight cue in -drawRect:. */
@interface _TextInputBezelView : NSView
@end

@implementation _TextInputBezelView

- (void)drawRect:(NSRect)dirtyRect;
{
  NSRect bounds = [self bounds];
  (void)dirtyRect;

  [InputBezelBackgroundColor() set];
  NSRectFill(bounds);

  /* Layer-backed: the layer's borderColor/borderWidth paints the rounded
   * stroke, and masksToBounds clips this rectangular fill to the rounded
   * shape at composite time. Painting our own rectangular frame here would
   * stack a square border behind the rounded one and look wrong at the
   * corners. */
  if (AICCCurrentTier() >= AICCTierMiddle) return;

  [InputBezelBorderColor() set];
  NSFrameRect(bounds);
  [InputBezelHighlightColor() set];
  NSRectFill(NSMakeRect(bounds.origin.x + 1.0,
                        bounds.origin.y + bounds.size.height - 2.0,
                        bounds.size.width - 2.0, 1.0));
}

@end

/* ── MessageSendViewController ── */

@interface MessageSendViewController ()
- (void)updateExpansion;
- (void)presentImagePicker;
- (void)rebuildActionsSegmentIcons;
- (void)updateMarkAsSeenEnabled;
- (void)barDidMoveToWindow;
- (void)externalUnseenStateChanged:(NSNotification *)note;
- (void)openPanelDidEnd:(NSOpenPanel *)panel
             returnCode:(int)code
            contextInfo:(void *)ctx;
@end

@implementation MessageSendViewController

- (id)initWithEngine:(ENILAccount *)engine;
{
  if ((self = [super init])) {
    engine_ = [engine retain];
    enabled_ = NO;
  }
  return self;
}

/* Single source of truth for "is there something sendable right now?" —
   shared by the Send button, the Cmd+Return menu item's validateMenuItem:,
   and the send path itself. */
- (BOOL)canSendCurrentMessage;
{
  if (!enabled_ || !inputView_) return NO;
  NSString *trimmed = [[[inputView_ textStorage] string]
      stringByTrimmingCharactersInSet:
        [NSCharacterSet whitespaceAndNewlineCharacterSet]];
  return [trimmed length] > 0 ? YES : NO;
}

- (void)updateSendSegmentEnabled;
{
  if (!actionsSegmented_) return;
  [actionsSegmented_ setEnabled:[self canSendCurrentMessage]
                     forSegment:kActionsSegSend];
}

/* Visual-only state application. NEVER walks the responder chain — that
 * stays in -updateMarkAsSeenEnabled and is only invoked from genuine state-
 * change entry points (-setChatId:, the unseen-state notifications, and
 * -viewWillAppear for the initial pass). Decoupling here is what lets
 * MessageListViewController.viewDidLoad call [sendController_ setEnabled:]
 * without tripping the chain walk before AppKit's AIVCAdapter has rewired
 * barView_.nextResponder on the Modern path. */
- (void)applyEnabledState;
{
  if (inputView_) {
    [inputView_ setEditable:enabled_];
    [inputView_ setSelectable:enabled_];
    [inputView_ setDrawsBackground:YES];
    [inputView_ setBackgroundColor:enabled_
        ? [NSColor controlBackgroundColor]
        : [NSColor disabledControlTextColor]];
  }
  if (actionsSegmented_) {
    /* Disabling the whole control implicitly disables every segment, so the
     * Mark-as-Seen and Send per-segment states (set by
     * -updateMarkAsSeenEnabled and -updateSendSegmentEnabled) stay correct
     * and visually identical while enabled_ is NO. When enabled_ flips back
     * to YES, those per-segment refreshes are driven by the next
     * setChatId: / unseen-state / text-change notification. */
    [actionsSegmented_ setEnabled:enabled_];
    if (enabled_) [self updateSendSegmentEnabled];
  }
}

/* AIViewController lifecycle split:
 *   -loadView      creates only the root NSView (so [self view] returns the
 *                  base container reliably before any subview work).
 *   -viewDidLoad   instantiates all subviews, wires delegates / notifications,
 *                  and applies the initial visual state. No responder-chain
 *                  walks here — on Modern the adapter has not yet rewired
 *                  the chain, so any walk would self-cycle.
 *   -viewDidLayout positions the subviews from the bar's current bounds.
 *                  Idempotent: same bounds → same frames. The autoresize
 *                  masks set in viewDidLoad keep the layout coherent between
 *                  layout passes.
 *   -viewWillAppear performs the first responder-chain walk, by which point
 *                  AppKit's AIVCAdapter has run adp_loadView on Modern and
 *                  AICookieCutter has fired the appear pair on Legacy. */

- (void)loadView;
{
  NSRect barFrame = NSMakeRect(0, 0, 400.0, kBarHeightCollapsed);
  ENILBottomToolbarView *bar = [[ENILBottomToolbarView alloc]
      initWithFrame:barFrame];
  barView_ = bar;
  [barView_ setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
  /* Rasterise the actions-segment icons whenever the bar attaches to a
   * window (initial attach + display-scale changes), so the icons match the
   * window's backing scale. */
  [bar setWindowChangeTarget:self action:@selector(barDidMoveToWindow)];
  /* Hand the bar to the base, then drop our +1 — the base now owns the
   * retain via view_; barView_ remains as a non-owning typed alias. */
  [self setView:barView_];
  [barView_ release];
}

- (void)viewDidLoad;
{
  [super viewDidLoad];

  /* Actions segmented control — three momentary segments at the right edge:
   * Close Chat (icon), Mark as Seen (icon), Send (icon + "Send" label).
   * Momentary tracking gives button-like behaviour (no persistent selection).
   * The cell's setTrackingMode: + NSSegmentSwitchTrackingMomentary date to
   * 10.3 so they're callable on every architecture target (oldest is Tiger
   * 10.4). NSSegmentedControl gained its own forwarding -trackingMode only
   * in 10.10.3 — the 10.5 SDK doesn't declare it on the control, so we go
   * through the cell to stay warning-clean on the ppc/x86 builds.
   * setSegmentStyle: is 10.5+ and lives on the cell on those vintages —
   * runtime-probed via respondsToSelector: so Tiger 10.4 falls through to
   * the default aqua look without crashing.
   * Icons (and their tooltips) are rasterised by -rebuildActionsSegmentIcons
   * once the bar has a window — only then is the backing scale known.
   * Setting the "Send" label here (before -rebuildActionsSegmentIcons) means
   * the -sizeToFit at the end of that method accounts for both image + label
   * slots. Cmd+Return is owned by the Chat
   * menu's "Send Message" item routed via the responder chain — no duplicate
   * key equivalent on the segment. */
  actionsSegmented_ = [[NSSegmentedControl alloc] initWithFrame:NSZeroRect];
  [actionsSegmented_ setSegmentCount:3];
  [[actionsSegmented_ cell] setTrackingMode:NSSegmentSwitchTrackingMomentary];
  if ([[actionsSegmented_ cell]
        respondsToSelector:@selector(setSegmentStyle:)]) {
    [[actionsSegmented_ cell] setSegmentStyle:NSSegmentStyleTexturedRounded];
  }
  [actionsSegmented_ setLabel:NSLocalizedString(@"Send", nil) forSegment:kActionsSegSend];
  [actionsSegmented_ setTarget:self];
  [actionsSegmented_ setAction:@selector(actionsSegmentClicked:)];
  [self rebuildActionsSegmentIcons];
  /* NSViewMinXMargin anchors the right edge as the bar widens; NSViewMaxYMargin
   * pins the bottom margin when the bar grows into the expanded state. */
  [actionsSegmented_ setAutoresizingMask:NSViewMinXMargin | NSViewMaxYMargin];
  [barView_ addSubview:actionsSegmented_];

  /* Bezel + scroll view + text view. Frames are NSZeroRect here — the real
   * geometry comes from -viewDidLayout. On Middle/Modern the bezel owns the
   * rounded chrome via its own layer: cornerRadius + masksToBounds clip the
   * inner scrollView/clipView/textView to the rounded shape at composite
   * time, and borderWidth/borderColor paint the visible stroke at the bezel
   * edge — the rectangle the user actually sees. Putting these on the text
   * view doesn't work: NSScrollView always interposes a square NSClipView,
   * and a vertically-resizable text view sizes to its content so its
   * "bottom corner" drifts with the text. Radius 8pt matches .enil-bubble
   * in ENILUserDefaults.m. Shims no-op on Legacy where there is no layer;
   * the bezel's -drawRect: handles that tier with the rectangular fallback. */
  _TextInputBezelView *bezel = [[_TextInputBezelView alloc]
      initWithFrame:NSZeroRect];
  bezelView_ = bezel;
  [bezel setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [barView_ addSubview:bezel];
  [bezel release];
  [bezelView_ XP_setWantsLayer:YES];
  [bezelView_ XP_setLayerCornerRadius:8.0];
  [bezelView_ XP_setLayerMasksToBounds:YES];
  [bezelView_ XP_setLayerBorderWidth:1.0];
  [bezelView_ XP_setLayerBorderColor:InputBezelBorderColor()];

  NSScrollView *scrollView = [[[NSScrollView alloc]
      initWithFrame:NSZeroRect] autorelease];
  [scrollView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [scrollView setBorderType:NSNoBorder];
  [scrollView setHasVerticalScroller:YES];
  if ([scrollView respondsToSelector:@selector(setAutohidesScrollers:)])
    [scrollView setAutohidesScrollers:YES];
  [bezelView_ addSubview:scrollView];

  inputView_ = [[NSTextView alloc] initWithFrame:NSZeroRect];
  [inputView_ setMinSize:NSMakeSize(0, 0)];
  [inputView_ setMaxSize:NSMakeSize(FLT_MAX, FLT_MAX)];
  [inputView_ setVerticallyResizable:YES];
  [inputView_ setHorizontallyResizable:NO];
  [inputView_ setAutoresizingMask:NSViewWidthSizable];
  [[inputView_ textContainer] setWidthTracksTextView:YES];
  [inputView_ setFont:XPFontTextStyleBody];
  [inputView_ setDrawsBackground:YES];
  [inputView_ setBackgroundColor:[NSColor controlBackgroundColor]];
  [inputView_ setTextContainerInset:NSMakeSize(2, 2)];
  [inputView_ setDelegate:self];
  [scrollView setDocumentView:inputView_];

  [barView_ setPostsFrameChangedNotifications:YES];
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(barViewFrameDidChange:)
             name:NSViewFrameDidChangeNotification
           object:barView_];

  /* External unseen-state changes — refresh the Mark-as-Seen segment.
   * SSE is scoped to engine_ for multi-account safety; sync-finished is
   * global because the C progress bridge can't pass an engine handle
   * (same compromise ChatWindowController accepts). */
  [[NSNotificationCenter defaultCenter] addObserver:self
    selector:@selector(externalUnseenStateChanged:)
        name:ENILSyncDidFinishNotification
      object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
    selector:@selector(externalUnseenStateChanged:)
        name:ENILSSEEventNotification
      object:engine_];

  /* Visual state only — the responder-chain walk is deferred to
   * -viewWillAppear / -setChatId: / the unseen-state notifications. */
  [self applyEnabledState];
}

- (void)viewDidLayout;
{
  [super viewDidLayout];
  if (!barView_) return;

  NSRect bounds = [barView_ bounds];
  CGFloat w = bounds.size.width;
  CGFloat h = bounds.size.height;
  CGFloat segW = [actionsSegmented_ frame].size.width;
  CGFloat segH = [actionsSegmented_ frame].size.height;
  /* Actions segmented: right-aligned with kPad margin, natural size from
   * sizeToFit, bottom-aligned so the placement agrees with its
   * NSViewMinXMargin|NSViewMaxYMargin autoresizing mask on every tier.
   * Centering here would fight that mask, and which one wins is tier-
   * dependent: Legacy re-runs -viewDidLayout from the bar's frame-change
   * notification (so centering stuck), while Middle/Modern never re-run it on
   * a plain resize, so the mask's bottom-pin stuck. Matching the mask makes
   * all three tiers render identically. */
  CGFloat segY = kPad;
  [actionsSegmented_ setFrame:NSMakeRect(w - segW - kPad, segY, segW, segH)];
  /* Bezel: kPad inset on all sides, grows to fill the space the segmented
   * control leaves behind. */
  CGFloat bezelW = w - segW - kPad * 3;
  if (bezelW < 0.0) bezelW = 0.0;
  [bezelView_ setFrame:NSMakeRect(kPad, kPad, bezelW, h - kPad * 2)];
}

- (void)viewWillAppear;
{
  [super viewWillAppear];
  /* First chain-walk: on Modern, AppKit's AIVCAdapter has now run
   * adp_loadView and rewired barView_.nextResponder to the adapter, so
   * [self nextResponder] reaches the parent's responder chain. On Legacy,
   * AI_addChildViewController set setNextResponder: directly at attach
   * time, so the walk has always been safe — this call just keeps the
   * mark-as-seen segment in sync once the window is about to appear. */
  [self updateMarkAsSeenEnabled];
}

- (void)barViewFrameDidChange:(NSNotification *)note;
{
  (void)note;
  [self updateExpansion];
}

/* Window-attach handler — see -loadView's setWindowChangeTarget call. Runs
 * after the bar's window pointer is non-nil, when the backing scale is known. */
- (void)barDidMoveToWindow;
{
  [self rebuildActionsSegmentIcons];
}

- (CGFloat)preferredHeight;
{
  return expanded_ ? kBarHeightExpanded : kBarHeightCollapsed;
}

- (void)updateExpansion;
{
  if (!inputView_) return;
  NSLayoutManager *lm = [inputView_ layoutManager];
  NSTextContainer *tc = [inputView_ textContainer];
  NSRange glyphRange = [lm glyphRangeForTextContainer:tc];
  NSUInteger lineCount = 0;
  NSUInteger idx = glyphRange.location;

  while (idx < NSMaxRange(glyphRange)) {
    NSRange lineRange;
    (void)[lm lineFragmentUsedRectForGlyphAtIndex:idx effectiveRange:&lineRange];
    if (lineRange.length == 0) break;
    lineCount++;
    idx = NSMaxRange(lineRange);
  }

  BOOL nowExpanded = (lineCount > 1);

  if (nowExpanded == expanded_) return;
  expanded_ = nowExpanded;
  if (delegate_)
    [delegate_ messageSendViewControllerDidChangeHeight:self];
}

- (void)setEnabled:(BOOL)enabled;
{
  enabled_ = enabled ? YES : NO;
  [self applyEnabledState];
}

- (void)setChatId:(NSString *)chatId;
{
  if (chatId == chatId_) return;
  [chatId_ autorelease];
  chatId_ = [chatId copy];
  /* Clear any pending mark-as-seen state — it's scoped to the previous
   * chat. */
  [pendingMarkSeenChatId_ release];
  pendingMarkSeenChatId_ = nil;
  /* Drop any in-progress draft on chat switch (and on close, where
   * chatId is nil) — same clear sequence -performSend: runs after a
   * successful send. Guarded on inputView_ so a setChatId: that lands
   * before -viewDidLoad is a safe no-op. */
  if (inputView_) {
    [inputView_ setString:@""];
    [self updateSendSegmentEnabled];
    [self updateExpansion];
  }
  /* The Mark-as-Seen availability is chat-scoped — recompute now even
   * though the matching setEnabled: call usually arrives right after,
   * so a no-op chat swap (same enabled, new id) still updates. */
  [self updateMarkAsSeenEnabled];
}

- (NSString *)chatId;
{
  return chatId_;
}

- (void)setDelegate:(id<MessageSendDelegate>)delegate;
{
  delegate_ = delegate;
}

- (id<MessageSendDelegate>)delegate;
{
  return delegate_;
}

- (void)insertSticonWithPackageId:(NSString *)packageId
                         sticonId:(NSString *)sticonId
                          altText:(NSString *)altText;
{
  if (!enabled_) return;
  if (!inputView_) return;
  NSURL *imgURL = [engine_ fileURLForPurchasesPackage:packageId
                                               asset:[sticonId stringByAppendingString:@".png"]];
  ENILLog(@"MessageSendViewController.insertSticon", @"pkg=%@ id=%@ url=%@",
        packageId, sticonId, imgURL);

  SticonAttachment *att = [[[SticonAttachment alloc]
                             initWithPackageId:packageId
                                      sticonId:sticonId
                                       altText:altText
                                           url:imgURL] autorelease];
  NSAttributedString *attStr =
      [NSAttributedString attributedStringWithAttachment:att];
  NSRange sel = [inputView_ selectedRange];
  [[inputView_ textStorage] replaceCharactersInRange:sel
                                withAttributedString:attStr];
  [inputView_ setSelectedRange:NSMakeRange(sel.location + 1, 0)];
  [inputView_ scrollRangeToVisible:[inputView_ selectedRange]];
  [[inputView_ window] makeFirstResponder:inputView_];
  [self updateSendSegmentEnabled];
  [self updateExpansion];
}

- (void)presentImagePicker;
{
  /* Bail paths still notify the responder chain so callers (toolbar Photo
   * segment) can restore their UI state — otherwise the segment stays stuck
   * on "Photo" when no chat is active. */
  if (!chatId_ || ![chatId_ length]) {
    [[self nextResponder] tryToPerform:@selector(enilImagePickerSheetDidEnd:)
                                  with:self];
    return;
  }
  NSWindow *parent = [barView_ window];
  if (!parent) {
    [[self nextResponder] tryToPerform:@selector(enilImagePickerSheetDidEnd:)
                                  with:self];
    return;
  }
  NSOpenPanel *panel = [NSOpenPanel openPanel];
  [panel setCanChooseFiles:YES];
  [panel setCanChooseDirectories:NO];
  [panel setAllowsMultipleSelection:NO];
  /* Keep bare extensions for files without UTI tagging. */
  NSArray *types = [NSArray arrayWithObjects:
    (NSString *)kUTTypeJPEG, (NSString *)kUTTypePNG,
    @"jpg", @"jpeg", @"png", nil];
  [panel XP_beginSheetForWindow:parent
                          types:types
                  modalDelegate:self
                 didEndSelector:@selector(openPanelDidEnd:returnCode:contextInfo:)
                    contextInfo:NULL];
}

- (void)openPanelDidEnd:(NSOpenPanel *)panel
             returnCode:(int)code
            contextInfo:(void *)ctx;
{
  (void)ctx;
  /* Always notify the responder chain that the sheet has ended — the toolbar
   * Photo segment relies on this to restore its previous selection regardless
   * of whether the user cancelled or picked a file. */
  [[self nextResponder] tryToPerform:@selector(enilImagePickerSheetDidEnd:)
                                with:self];
  if (code != XPModalResponseOK) return;
  NSArray *urls = [panel URLs];
  if (![urls count]) return;
  NSURL *url = [urls objectAtIndex:0];
  NSString *path = [url isFileURL] ? [url path] : nil;
  if (![path length] || !chatId_) return;
  NSDictionary *payload = [NSDictionary dictionaryWithObjectsAndKeys:
    chatId_, ENILResponderPayloadChatIdKey,
    path,    ENILResponderPayloadImagePathKey,
    nil];
  [[self nextResponder] tryToPerform:@selector(enilSendImage:) with:payload];
}

#pragma mark - Actions Segment

/* Called from -loadView (pre-window) and again from the bar's
 * setWindowChangeTarget callback once attached. Uses the window's actual
 * backing scale when available so HiDPI screens get sharp rasters; pre-
 * window calls produce a 1x raster with the correct canvas size, so any
 * size-fitting done at loadView stays valid. XP_setToolTip:forSegment: is
 * a no-op on pre-10.13; setImage:forSegment: works back to 10.3. */
- (void)rebuildActionsSegmentIcons;
{
  if (!actionsSegmented_) return;
  CGFloat scale = [[barView_ window] XP_backingScaleFactor];
  AIFontAwesomeStyle regular = AIFontAwesomeStyleRegular;
  if (scale < 1.0) scale = 1.0;
  [actionsSegmented_ setImage:[AIFontAwesome imageForIcon:AIFACircleXmark
                                                  style:regular
                                               iconSize:14.0
                                             canvasSize:20.0
                                                  scale:scale]
                   forSegment:kActionsSegClose];
  [actionsSegmented_ setImage:[AIFontAwesome imageForIcon:AIFACircleCheck
                                                  style:regular
                                               iconSize:14.0
                                             canvasSize:20.0
                                                  scale:scale]
                   forSegment:kActionsSegMarkSeen];
  [actionsSegmented_ setImage:[AIFontAwesome imageForIcon:AIFAPaperPlane
                                                  style:regular
                                               iconSize:14.0
                                             canvasSize:20.0
                                                  scale:scale]
                   forSegment:kActionsSegSend];
  [actionsSegmented_ XP_setToolTip:NSLocalizedString(@"Close Chat",   nil) forSegment:kActionsSegClose];
  [actionsSegmented_ XP_setToolTip:NSLocalizedString(@"Mark as Seen", nil) forSegment:kActionsSegMarkSeen];
  [actionsSegmented_ XP_setToolTip:NSLocalizedString(@"Send Message", nil) forSegment:kActionsSegSend];
  /* Re-fit after the icon raster so the cached frame size reflects current
   * content (images + the "Send" label). -viewDidLayout reads this size to
   * place the control, so fitting here — the one place content changes — keeps
   * that read fresh without re-fitting on every layout pass. */
  [actionsSegmented_ sizeToFit];
}

/* Per-segment enable: Mark-as-Seen greys out when the active chat has
 * nothing unread. Reuses ChatWindowController's -enil_canMarkActiveChatSeen
 * via a manual responder-chain walk so the bar doesn't duplicate the
 * predicate. tryToPerform:with: only carries void actions, so a BOOL query
 * has to walk the chain by hand — respondsToSelector + typed objc_msgSend
 * cast per CLAUDE.md's "prefer the runtime cast over NSInvocation" rule.
 * Close stays enabled whenever the bar itself is enabled — that gate is
 * already correct. setEnabled:forSegment: lives on NSSegmentedControl since
 * the 10.5 SDK, so we don't need the [cell] hop here that setTrackingMode:
 * required. */
- (void)updateMarkAsSeenEnabled;
{
  if (!actionsSegmented_) return;
  /* The chain walk is reachable only from -setChatId:, the unseen-state
   * notifications, and -viewWillAppear. All three fire after AppKit's
   * AIVCAdapter has run adp_loadView on Modern (which rewires
   * barView_.nextResponder away from the self-cycling fallback that
   * AIViewController.setView: installed) and after AI_addChildViewController:
   * wired setNextResponder: directly on Legacy. So [self nextResponder]
   * always reaches a real parent here and the walk terminates. */
  SEL sel = @selector(enil_canMarkActiveChatSeen);
  BOOL canMarkSeen = NO;
  id r = [self nextResponder];
  while (r) {
    if ([r respondsToSelector:sel]) {
      canMarkSeen = ((BOOL (*)(id, SEL))objc_msgSend)(r, sel);
      break;
    }
    r = [r nextResponder];
  }
  /* If the DB now says nothing is unread, the SSE confirmation has landed —
   * clear our in-memory pending flag so the next chat switch doesn't carry a
   * stale disable. */
  if (!canMarkSeen) {
    [pendingMarkSeenChatId_ release];
    pendingMarkSeenChatId_ = nil;
  }
  /* In-memory pending flag: keep the button disabled even if the DB still
   * reports unread messages, because we sent the RPC and are waiting for the
   * SSE push to confirm the seen state. */
  if (pendingMarkSeenChatId_ && chatId_ &&
      [pendingMarkSeenChatId_ isEqualToString:chatId_]) {
    [actionsSegmented_ setEnabled:NO
                       forSegment:kActionsSegMarkSeen];
    return;
  }
  [actionsSegmented_ setEnabled:canMarkSeen
                     forSegment:kActionsSegMarkSeen];
}

/* Notification trampoline — both SSE message arrivals and sync completion
 * can flip unseen state without the bar's setters being touched. */
- (void)externalUnseenStateChanged:(NSNotification *)note;
{
  (void)note;
  [self updateMarkAsSeenEnabled];
}

/* Routes to enilCloseChat: / enilMarkChatSeen: through the responder chain
 * — ChatWindowController owns those actions and shares them with the Chat
 * menu, so this segmented control just becomes another caller. */
- (void)actionsSegmentClicked:(id)sender;
{
  NSInteger which = [(NSSegmentedControl *)sender selectedSegment];
  if (which == kActionsSegClose) {
    [[self nextResponder] tryToPerform:@selector(enilCloseChat:) with:self];
    /* Close path runs setChatId:nil + setEnabled:NO via the parent's
     * reload, so Mark-as-Seen recomputes through applyEnabledState. */
    return;
  }
  if (which == kActionsSegMarkSeen) {
    [[self nextResponder] tryToPerform:@selector(enilMarkChatSeen:) with:self];
    /* Disable the button in memory until the SSE push confirms the seen
     * state.  We no longer write to the DB optimistically in
     * -[ENILAccount markChatSeen:messageId:] — that created a split
     * source of truth where the button read fresh DB (unreadCount == 0)
     * but the chat list cached rows_ still showed the blue dot.  Now
     * only the SSE path (enil_sync_process_sse_event) writes to the DB;
     * this in-memory flag keeps the button greyed until that arrives. */
    [pendingMarkSeenChatId_ autorelease];
    pendingMarkSeenChatId_ = [chatId_ copy];
    [self updateMarkAsSeenEnabled];
    return;
  }
  if (which == kActionsSegSend) {
    [self performSend:sender];
  }
}

#pragma mark - Send

/* Public send action. Reached three ways — Send segment, Cmd+Return menu
   item (via the responder chain), and any future caller — all gated by the
   same -canSendCurrentMessage predicate. */
- (void)performSend:(id)sender;
{
  (void)sender;
  if (![self canSendCurrentMessage]) return;
  if (!chatId_ || ![chatId_ length]) return;

  NSTextStorage *storage = [inputView_ textStorage];
  NSString *raw = [storage string];
  NSMutableString *lineText = [NSMutableString stringWithCapacity:[raw length]];
  NSMutableArray *resources = [NSMutableArray array];

  NSUInteger i;
  for (i = 0; i < [raw length]; i++) {
    unichar c = [raw characterAtIndex:i];
    if (c == NSAttachmentCharacter) {
      id att = [storage attribute:NSAttachmentAttributeName
                          atIndex:i
                   effectiveRange:NULL];
      if ([att isKindOfClass:[SticonAttachment class]]) {
        NSString *rawAlt = [att altText];
        NSString *pkgId  = [att packageId];
        NSString *sid    = [att sticonId];
        NSString *alt = (rawAlt && [rawAlt length]) ? rawAlt : sid;
        NSString *marker = [NSString stringWithFormat:@"(%@)", alt];
        [lineText appendString:marker];
        [resources addObject:[NSDictionary dictionaryWithObjectsAndKeys:
          pkgId, @"package_id",
          sid,   @"sticon_id",
          alt,   @"alt_text",
          nil]];
      }
    } else {
      CFStringAppendCharacters((CFMutableStringRef)lineText, &c, 1);
    }
  }

  NSString *trimmed = [lineText stringByTrimmingCharactersInSet:
      [NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if (![trimmed length]) return;
  [inputView_ setString:@""];
  [self updateSendSegmentEnabled];
  [self updateExpansion];

  NSDictionary *payload = [NSDictionary dictionaryWithObjectsAndKeys:
    chatId_, ENILResponderPayloadChatIdKey,
    trimmed, ENILResponderPayloadTextKey,
    resources, ENILResponderPayloadResourcesKey,
    nil];
  [[self nextResponder] tryToPerform:@selector(enilSendMessage:) with:payload];
}

- (void)textDidChange:(NSNotification *)note;
{
  (void)note;
  NSFont *font = [inputView_ font] ?: XPFontTextStyleBody;
  NSTextStorage *storage = [inputView_ textStorage];
  NSRange all = NSMakeRange(0, [storage length]);
  if (all.length > 0)
    [storage addAttribute:NSFontAttributeName value:font range:all];
  [self updateSendSegmentEnabled];
  [self updateExpansion];
}


#pragma mark - Cleanup

- (void)dealloc;
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [chatId_ release];
  [pendingMarkSeenChatId_ release];
  [engine_ release];
  /* barView_ is a non-owning alias — released by AIViewController's base via
   * view_. */
  [inputView_ release];
  [actionsSegmented_ release];
  [super dealloc];
}

@end
