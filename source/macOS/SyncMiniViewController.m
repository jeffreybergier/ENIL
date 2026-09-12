#import "SyncMiniViewController.h"
#import "ENILResponderActions.h"
#import "XPAppKit.h"
#import "AIFontAwesome.h"
#import "ENILBottomToolbarView.h"

static const CGFloat kMiniH          = 32.0;
static const CGFloat kMiniPad        = 4.0;
static const CGFloat kMiniButtonWH   = 24.0;
static const CGFloat kMiniLabelH     = 12.0;
static const CGFloat kMiniProgressH  = 8.0;
static const NSTimeInterval kShineDuration = 0.60;
static const NSTimeInterval kShineFPS      = 1.0 / 30.0;

static NSColor *SyncMiniTextColor(void) { return [NSColor controlTextColor]; }
static NSColor *SyncMiniTextHighlightColor(void) { return XPColorControlHighlight; }

/* ------------------------------------------------------------------------- */

@interface SyncMiniStatusTextView : NSView {
 @private
  NSString *text_;
  double    shineProgress_;
  BOOL      shineActive_;
  BOOL      isError_;
}
- (void)setText:(NSString *)text;
- (void)setShineProgress:(double)progress active:(BOOL)active;
- (void)setError:(BOOL)isError;
@end

@implementation SyncMiniStatusTextView

- (void)dealloc;
{
  [text_ release];
  [super dealloc];
}

- (void)setText:(NSString *)text;
{
  if (text_ == text) return;
  [text_ release];
  text_ = [text copy];
  [self setNeedsDisplay:YES];
}

- (void)setShineProgress:(double)progress active:(BOOL)active;
{
  if (progress < 0.0) progress = 0.0;
  if (progress > 1.0) progress = 1.0;
  shineProgress_ = progress;
  shineActive_ = active;
  [self setNeedsDisplay:YES];
}

- (void)setError:(BOOL)isError;
{
  if (isError_ == isError) return;
  isError_ = isError;
  [self setNeedsDisplay:YES];
}

/* Solid red used when isError_ is set. Mirrors the system "destructive" UI
 * tone — vivid enough to be unmissable in the chrome of the bottom bar without
 * being neon. Static so the same instance is reused on every redraw. */
static NSColor *SyncMiniErrorColor(void) {
  static NSColor *c = nil;
  if (!c) c = [[NSColor colorWithDeviceRed:0.80 green:0.13 blue:0.13
                                     alpha:1.0] retain];
  return c;
}

- (void)drawRect:(NSRect)dirtyRect;
{
  NSString *text = text_ ? text_ : @"";
  NSFont *font = XPFontTextStyleBoldSmall;
  NSMutableDictionary *attrs = [NSMutableDictionary dictionary];
  NSColor *mainColor = isError_ ? SyncMiniErrorColor() : SyncMiniTextColor();
  NSSize size;
  NSRect rect;
  NSRect shineRect;
  CGFloat shineW;
  (void)dirtyRect;

  [attrs setObject:font forKey:NSFontAttributeName];
  [attrs setObject:mainColor forKey:NSForegroundColorAttributeName];

  size = [text sizeWithAttributes:attrs];
  rect = NSMakeRect(floor(([self bounds].size.width - size.width) * 0.5),
                    floor(([self bounds].size.height - size.height) * 0.5),
                    ceil(size.width), ceil(size.height));

  /* The 1pt "highlight" emboss under regular labels reads as decorative — we
   * suppress it for error rows so the red is unambiguous and not softened by a
   * contrasting underlay. */
  if (!isError_) {
    [attrs setObject:SyncMiniTextHighlightColor()
              forKey:NSForegroundColorAttributeName];
    [text drawInRect:NSOffsetRect(rect, 0.0, -1.0) withAttributes:attrs];
  }

  [attrs setObject:mainColor forKey:NSForegroundColorAttributeName];
  [text drawInRect:rect withAttributes:attrs];

  /* Error rows never animate the "shine" wipe — that's a steady-state "alive
   * and syncing" affordance, not an error one. */
  if (!shineActive_ || isError_) return;

  shineW = MAX(8.0, rect.size.width * 0.20);
  shineRect = NSMakeRect(rect.origin.x - shineW +
                           (rect.size.width + shineW * 2.0) * shineProgress_,
                         rect.origin.y,
                         shineW,
                         rect.size.height);
  [NSGraphicsContext saveGraphicsState];
  NSRectClip(shineRect);
  [attrs setObject:SyncMiniTextHighlightColor()
            forKey:NSForegroundColorAttributeName];
  [text drawInRect:rect withAttributes:attrs];
  [NSGraphicsContext restoreGraphicsState];
}

@end

/* ------------------------------------------------------------------------- */

@interface SyncMiniViewController ()
- (void)redraw;
- (void)renderBarMode;
- (void)renderSteadyState;
@end

@implementation SyncMiniViewController

static void configureProgress(NSProgressIndicator *progress)
{
  [progress setStyle:XPProgressIndicatorStyleBar];
  [progress setIndeterminate:NO];
  [progress setMinValue:0.0];
  [progress setMaxValue:100.0];
  [progress setDoubleValue:0.0];
}

/* All four subviews share width through siblings (the button anchors the right
 * edge; statusView/label/progress fill the rest), so geometry math lives in
 * -viewDidLayout — one place to read for "where does each subview go for
 * current bounds?". Autoresize masks set in -viewDidLoad keep the layout
 * coherent between layout passes. */
- (void)viewDidLayout;
{
  [super viewDidLayout];
  if (!syncButton_) return;
  NSRect bounds = [[self view] bounds];
  /* Buttons anchor both edges (sync right, link left); the content column
   * (status / label / progress) fills the gap between them. */
  CGFloat rightButtonX = bounds.size.width - kMiniPad - kMiniButtonWH;
  CGFloat contentX = kMiniPad + kMiniButtonWH + kMiniPad;
  CGFloat contentW = rightButtonX - kMiniPad - contentX;
  NSRect content;
  CGFloat buttonY = (bounds.size.height - kMiniButtonWH) * 0.5;
  NSRect rightButton = NSMakeRect(rightButtonX, buttonY,
                                  kMiniButtonWH, kMiniButtonWH);
  NSRect leftButton = NSMakeRect(kMiniPad, buttonY,
                                 kMiniButtonWH, kMiniButtonWH);
  NSRect label;
  NSRect progress;
  if (contentW < 0.0) contentW = 0.0;
  content = NSMakeRect(contentX, kMiniPad, contentW,
                       bounds.size.height - kMiniPad * 2.0);
  label = NSMakeRect(content.origin.x,
                     content.origin.y + kMiniProgressH + kMiniPad,
                     content.size.width,
                     kMiniLabelH);
  progress = NSMakeRect(content.origin.x,
                        content.origin.y,
                        content.size.width,
                        kMiniProgressH);
  [syncButton_ setFrame:rightButton];
  [linkButton_ setFrame:leftButton];
  [statusView_ setFrame:content];
  [syncingLabel_ setFrame:label];
  [progress_ setFrame:progress];
}

- (void)configureSyncButton;
{
  [syncButton_ setBezelStyle:XPBezelStyleTexturedSquare];
  [syncButton_ setButtonType:XPButtonTypeMomentaryLight];
  [syncButton_ setImagePosition:NSImageOnly];
  [syncButton_ setToolTip:NSLocalizedString(@"Sync", nil)];
}

/* Called from view_ once it has a window, so the backing scale is known and
 * correct (also re-runs on display change). */
- (void)rebuildSyncButtonIcon;
{
  if (!syncButton_) return;
  CGFloat scale = [[[self view] window] XP_backingScaleFactor];
  if (scale < 1.0) scale = 1.0;
  NSImage *image = [AIFontAwesome imageForIcon:AIFAArrowRotateRight
                                       style:AIFontAwesomeStyleSolid
                                    iconSize:13.0
                                  canvasSize:16.0
                                       scale:scale];
  if (image) {
    [syncButton_ setImagePosition:NSImageOnly];
    [syncButton_ setImage:image];
    [syncButton_ setTitle:@""];
  } else {
    [syncButton_ setImage:nil];
    [syncButton_ setImagePosition:NSNoImage];
    [syncButton_ setTitle:[NSString stringWithUTF8String:"\xE2\x86\x93"]];
    [syncButton_ setFont:XPFontTextStyleBoldBody];
  }
}

- (void)configureLinkButton;
{
  [linkButton_ setBezelStyle:XPBezelStyleTexturedSquare];
  [linkButton_ setButtonType:XPButtonTypeMomentaryLight];
  [linkButton_ setImagePosition:NSImageOnly];
  /* Tooltip is state-dependent and set in -rebuildLinkButtonIcon (which runs on
   * window attach, before the button is hoverable) — mirrors iOS's link button
   * accessibility label. */
}

/* link when SSE is enabled, link-slash when the user has turned it off. The
 * Offline state is set ONLY by the toggle, so syncState is the source of truth;
 * a transient Error/Syncing phase still draws "link" (enabled, not connected
 * now). Caches the drawn state in linkShowsOff_ so -redraw skips redundant
 * re-rasterisation on every progress tick. */
- (void)rebuildLinkButtonIcon;
{
  AIFontAwesomeIcon glyph;
  CGFloat scale;
  NSImage *image;
  if (!linkButton_) return;
  linkShowsOff_ = ([queue_ syncState] == ENILSyncStateOffline);
  glyph = linkShowsOff_ ? AIFALinkSlash : AIFALink;
  /* Name the action the click performs, matching the iOS link button's
   * accessibility label: off -> "Connect", on -> "Disconnect". */
  [linkButton_ setToolTip:linkShowsOff_ ? NSLocalizedString(@"Connect", nil)
                                        : NSLocalizedString(@"Disconnect", nil)];
  scale = [[[self view] window] XP_backingScaleFactor];
  if (scale < 1.0) scale = 1.0;
  image = [AIFontAwesome imageForIcon:glyph
                              style:AIFontAwesomeStyleSolid
                           iconSize:13.0
                         canvasSize:16.0
                              scale:scale];
  if (image) {
    [linkButton_ setImagePosition:NSImageOnly];
    [linkButton_ setImage:image];
    [linkButton_ setTitle:@""];
  } else {
    [linkButton_ setImage:nil];
    [linkButton_ setImagePosition:NSNoImage];
    [linkButton_ setTitle:linkShowsOff_ ? @"x" : @"o"];
    [linkButton_ setFont:XPFontTextStyleBoldBody];
  }
}

/* Both glyphs are scale-dependent, so re-rasterise both whenever the backing
 * scale may have changed (initial window attach + display change). */
- (void)rebuildBarIcons;
{
  [self rebuildSyncButtonIcon];
  [self rebuildLinkButtonIcon];
}

- (void)stopShine;
{
  [shineTimer_ invalidate];
  [shineTimer_ release];
  shineTimer_ = nil;
  [statusView_ setShineProgress:0.0 active:NO];
}

- (void)shineTick:(NSTimer *)timer;
{
  NSTimeInterval elapsed = [NSDate timeIntervalSinceReferenceDate] - shineStart_;
  double progress = elapsed / kShineDuration;
  (void)timer;
  if (progress >= 1.0) {
    [self stopShine];
    return;
  }
  [statusView_ setShineProgress:progress active:YES];
}

/* One-shot 0.6s highlight wipe over the steady-state text. The queue has
 * already gated this (it only fires -syncStatusQueueRequestsShimmer: when
 * idle-live with nothing transient up), so there are no state checks here. */
- (void)startShine;
{
  [self stopShine];
  shineStart_ = [NSDate timeIntervalSinceReferenceDate];
  [statusView_ setShineProgress:0.0 active:YES];
  shineTimer_ = [[NSTimer scheduledTimerWithTimeInterval:kShineFPS
                                                  target:self
                                                selector:@selector(shineTick:)
                                                userInfo:nil
                                                 repeats:YES] retain];
}

/* Lifecycle split: -loadView creates only the root window-aware bar (and hooks
 * its window-change callback). -viewDidLoad creates the four subviews and the
 * queue. -viewDidLayout owns geometry. */

- (id)initWithAccount:(id)account;
{
  if ((self = [super init])) {
    account_ = account;  /* unsafe_unretained; only forwarded to the queue */
  }
  return self;
}

- (void)loadView;
{
  ENILBottomToolbarView *bar = [[ENILBottomToolbarView alloc]
      initWithFrame:NSMakeRect(0, 0, 240.0, kMiniH)];
  [bar setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  /* On every window change (initial attach + display-scale changes) pin the bar
   * to the window's appearance, then re-rasterise both glyphs for the backing
   * scale. */
  [bar setWindowChangeTarget:self
                      action:@selector(barDidMoveToWindow)];
  [self setView:bar];
  [bar release];
}

/* This bar lives inside the vibrant .sidebar split pane (StatusViewController),
 * so by default its semantic colors resolve to washed-out vibrant variants and
 * its top separator won't match MessageSendViewController's content-pane bar.
 * Pinning THIS view (the one that draws) to the window's non-vibrant appearance
 * is what fixes it — pinning the StatusViewController wrapper and relying on
 * inheritance doesn't reach this view through the child-VC adapter. The shim
 * self-gates to Modern (11.0+), the only tier where the symptom exists. */
- (void)barDidMoveToWindow;
{
  [[self view] XP_pinToWindowAppearance];
  [self rebuildBarIcons];
}

- (void)viewDidLoad;
{
  [super viewDidLoad];
  NSView *bar = [self view];

  statusView_ = [[SyncMiniStatusTextView alloc] initWithFrame:NSZeroRect];
  [statusView_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [bar addSubview:statusView_];

  syncingLabel_ = [[NSTextField alloc] initWithFrame:NSZeroRect];
  [syncingLabel_ setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
  [syncingLabel_ setBezeled:NO];
  [syncingLabel_ setDrawsBackground:NO];
  [syncingLabel_ setEditable:NO];
  [syncingLabel_ setSelectable:NO];
  [syncingLabel_ setAlignment:XPTextAlignmentCenter];
  [syncingLabel_ setFont:XPFontTextStyleSmallLabel];
  [bar addSubview:syncingLabel_];

  progress_ = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
  [progress_ setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
  configureProgress(progress_);
  [bar addSubview:progress_];

  syncButton_ = [[NSButton alloc] initWithFrame:NSZeroRect];
  [syncButton_ setAutoresizingMask:NSViewMinXMargin | NSViewMinYMargin];
  [self configureSyncButton];
  [syncButton_ setTarget:self];
  [syncButton_ setAction:@selector(syncTapped:)];
  [bar addSubview:syncButton_];

  /* SSE on/off toggle, anchored to the LEFT edge (NSViewMaxXMargin). */
  linkButton_ = [[NSButton alloc] initWithFrame:NSZeroRect];
  [linkButton_ setAutoresizingMask:NSViewMaxXMargin | NSViewMinYMargin];
  [self configureLinkButton];
  [linkButton_ setTarget:self];
  [linkButton_ setAction:@selector(linkTapped:)];
  [bar addSubview:linkButton_];

  /* The portable engine self-wires to the sync notifications and applies the
   * initial NEVER_SYNCED state the moment we become its delegate. Scoped to
   * account_ so this bar only reflects its own account's state. */
  queue_ = [[ENILSyncStatusQueue alloc] initWithAccount:account_];
  [queue_ setDelegate:self];
}

/* The single drawing entry point. Dispatches to one of two layout helpers by
 * the queue's display mode — bar-mode whenever a sync is in flight (stable
 * layout across every phase, including the gap between phases), centered
 * statusView otherwise. */
- (void)redraw;
{
  if ([queue_ displayMode] == ENILSyncDisplayModeBar) [self renderBarMode];
  else                                                [self renderSteadyState];
  [syncButton_ setEnabled:[queue_ syncButtonEnabled]];
  /* Repaint the link glyph only when the Offline state actually flipped, so a
   * burst of progress redraws during a sync doesn't re-rasterise it each tick. */
  if (([queue_ syncState] == ENILSyncStateOffline) != linkShowsOff_)
    [self rebuildLinkButtonIcon];
}

/* Bar layout: used for the entire Syncing window. With a determinate item the
 * bar tracks its progress; indeterminate spins; with no item it sits empty at 0
 * under the "Syncing" label, which doubles as a phase-boundary cue. */
- (void)renderBarMode;
{
  BOOL determinate = [queue_ displayDeterminate];
  BOOL isError     = [queue_ displayIsError];
  NSString *label  = [queue_ displayLabel];

  [statusView_   setHidden:YES];
  [progress_     setHidden:NO];
  [syncingLabel_ setHidden:NO];
  [self stopShine];

  if (determinate) {
    if ([progress_ isIndeterminate]) {
      [progress_ stopAnimation:nil];
      [progress_ setIndeterminate:NO];
    }
    [progress_ setDoubleValue:[queue_ displayPercent]];
  } else if (![progress_ isIndeterminate]) {
    [progress_ setIndeterminate:YES];
    [progress_ startAnimation:nil];
  }

  [syncingLabel_ setStringValue:label ? label : @""];
  /* NSTextField caches its text color across redraws, so we always set it
   * explicitly here (otherwise an error → non-error transition would leave the
   * label red). */
  [syncingLabel_ setTextColor:isError ? SyncMiniErrorColor()
                                      : [NSColor controlTextColor]];
}

/* Centered statusView layout: steady-state text, or a transient queue item
 * shown without the bar (e.g. an SSE-driven notification while Live). */
- (void)renderSteadyState;
{
  [progress_     setHidden:YES];
  [syncingLabel_ setHidden:YES];
  [statusView_   setHidden:NO];
  if ([progress_ isIndeterminate]) {
    [progress_ stopAnimation:nil];
    [progress_ setIndeterminate:NO];
  }
  [statusView_ setText:[queue_ displayLabel]];
  [statusView_ setError:[queue_ displayIsError]];
}

/* ----------------------- ENILSyncStatusQueueDelegate --------------------- */

- (void)syncStatusQueueDidChange:(ENILSyncStatusQueue *)queue;
{
  (void)queue;
  [self redraw];
}

- (void)syncStatusQueueRequestsShimmer:(ENILSyncStatusQueue *)queue;
{
  (void)queue;
  [self startShine];
}

/* ------------------------------------------------------------------------- */

- (void)syncTapped:(id)sender;
{
  ENILSyncState state = [queue_ syncState];
  (void)sender;
  if (state == ENILSyncStateSyncing) return;
  /* When the token is dead, the only useful action is reauth — repurpose the
     same button (its label reads "Reauthenticate" in this state). Both
     selectors resolve up the responder chain to ChatWindowController. */
  if (state == ENILSyncStateReauthNeeded) {
    [[self nextResponder] tryToPerform:@selector(enilReauthenticate:)
                                  with:self];
    return;
  }
  [[self nextResponder] tryToPerform:@selector(enilStartSync:) with:self];
}

/* The link toggle resolves up the responder chain to ChatWindowController,
 * which owns the engine and flips its persisted SSE preference. */
- (void)linkTapped:(id)sender;
{
  (void)sender;
  [[self nextResponder] tryToPerform:@selector(enilToggleSSE:) with:self];
}

- (void)dealloc;
{
  [self stopShine];
  [queue_ setDelegate:nil];
  [queue_ release];
  /* view_ is owned by AIViewController base; do not release here. */
  [progress_ release];
  [syncingLabel_ release];
  [statusView_ release];
  [syncButton_ release];
  [linkButton_ release];
  [super dealloc];
}

@end
