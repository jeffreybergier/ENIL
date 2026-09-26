//
//  SyncMiniBarView.m
//  ENIL
//

#import "SyncMiniBarView.h"
#import <QuartzCore/QuartzCore.h>   /* CAGradientLayer / CABasicAnimation ping shimmer */
#import "ENILSyncStatusQueue.h"
#import "XPUIKit.h"

/* The NSTextAlignment *type* is iOS 6.0+, so we never name it; the value is
 * ABI-stable at 1 and the .textAlignment setter converts the NSInteger
 * implicitly. Same trap, same fix as QRLoginViewController. */
static const NSInteger kENILTextAlignCenter = 1;

static const CGFloat kBarFontPt    = 13.0;
static const CGFloat kSteadyFontBump = 2.0;   /* steady-state text is a touch larger */
static const CGFloat kBarHeight    = 30.0;
static const CGFloat kStackGap    = 2.0;    /* label bottom -> progress bar top */
static const CGFloat kProgressW   = 100.0;  /* fixed bar width (always shown in bar mode) */
static const float   kIndetFill   = 0.5f;   /* indeterminate phases peg here */
static const NSTimeInterval kShineDuration = 0.60;  /* ping shimmer sweep */
/* Upward nudge (x font.descender) so single-line text doesn't read low. Split
 * per layout mode so each can be tuned independently: bar mode is dialed in,
 * steady (text-only) mode is the one being tweaked. */
static const CGFloat kInkLiftFactorSteady = 0.3f;  /* text label alone, no progress bar */
static const CGFloat kInkLiftFactorBar    = 0.8f;  /* label stacked above the progress bar */

@interface SyncMiniBarView () <ENILSyncStatusQueueDelegate>
@property (nonatomic, strong) ENILSyncStatusQueue *queue;
@property (nonatomic, strong) UILabel             *label;
@property (nonatomic, strong) UILabel             *shineLabel;
@property (nonatomic, strong) UIProgressView      *progress;
@end

@implementation SyncMiniBarView

/* Solid red mirroring the macOS SyncMiniErrorColor — tints the label text for
 * a sticky error so a torn-down sync is unmissable on the toolbar. */
+ (UIColor *)errorColor
{
  static UIColor *c = nil;
  if (c == nil)
    c = [UIColor colorWithRed:0.80f green:0.13f blue:0.13f alpha:1.0f];
  return c;
}

- (instancetype)initWithFrame:(CGRect)frame
{
  return [self initWithFrame:frame account:nil];
}

- (instancetype)initWithFrame:(CGRect)frame account:(id)account
{
  if ((self = [super initWithFrame:frame])) {
    /* Paint nothing of our own — the hosting UIToolbar's chrome shows through. */
    [self setBackgroundColor:[UIColor clearColor]];
    [self buildSubviews];
    /* The shared engine self-wires to the sync notifications and paints the
     * initial state the moment we become its delegate. Scoped to `account` so
     * the bar only reflects its own account's state. */
    _queue = [[ENILSyncStatusQueue alloc] initWithAccount:account];
    [_queue setDelegate:self];
  }
  return self;
}

- (void)buildSubviews
{
  _label = [[UILabel alloc] initWithFrame:CGRectZero];
  [_label setBackgroundColor:[UIColor clearColor]];
  [_label setTextAlignment:kENILTextAlignCenter];
  [_label setFont:[UIFont boldSystemFontOfSize:kBarFontPt]];
  [self addSubview:_label];

  /* A light-gray copy of the label, painted on top and revealed only under the
   * moving gradient mask — the visible ping shimmer. Gray contrasts with both
   * the legacy white and modern dark base text. Hidden between sweeps. */
  _shineLabel = [[UILabel alloc] initWithFrame:CGRectZero];
  [_shineLabel setBackgroundColor:[UIColor clearColor]];
  [_shineLabel setTextAlignment:kENILTextAlignCenter];
  [_shineLabel setFont:[UIFont boldSystemFontOfSize:kBarFontPt]];
  [_shineLabel setTextColor:[UIColor lightGrayColor]];
  [_shineLabel setHidden:YES];
  [self addSubview:_shineLabel];

  /* ALWAYS a determinate bar. iOS has no constant-footprint indeterminate bar
   * (the indeterminate widget is a differently-shaped spinner), so unlike the
   * macOS NSProgressIndicator we never switch styles: indeterminate phases just
   * peg the determinate bar at kIndetFill, keeping size/shape/position fixed. */
  _progress = [[UIProgressView alloc]
    initWithProgressViewStyle:UIProgressViewStyleDefault];
  [_progress setHidden:YES];
  [self addSubview:_progress];
  [self refreshAppearance];
}

/* Match StrappyPreferencesStatusToolbarView: flat dark text on iOS 7+,
 * engraved white text on the older toolbar chrome. Reapply on every render
 * so leaving an error restores the correct color for the running OS. */
- (void)refreshAppearance
{
  BOOL usesIOS7Appearance = [[UIDevice currentDevice]
    XP_isOperatingSystemAtLeastMajorVersion:7];
  UIColor *textColor = usesIOS7Appearance
    ? [UIColor darkTextColor] : [UIColor whiteColor];
  [[self label] setTextColor:[[self queue] displayIsError]
    ? [SyncMiniBarView errorColor] : textColor];
  [[self label] setShadowColor:usesIOS7Appearance
    ? nil : [UIColor colorWithWhite:0.0f alpha:0.5f]];
  [[self label] setShadowOffset:usesIOS7Appearance
    ? CGSizeZero : CGSizeMake(0.0f, -1.0f)];
}

/* Pull the queue's display snapshot onto the widgets — the iOS analogue of the
 * macOS -redraw. Bar mode (a sync in flight) shows the label + a determinate
 * progress bar; steady mode is the centered label alone, the real toolbar
 * carrying the platform gloss while the SSE ping drives the shimmer sweep. */
- (void)render
{
  BOOL active = ([[self queue] displayMode] == ENILSyncDisplayModeBar);

  [[self label] setText:[[self queue] displayLabel] ? [[self queue] displayLabel] : @""];
  /* Steady-state text (idle / "Syncing Live") rides a touch larger than the
   * in-flight phase labels. */
  [[self label] setFont:[UIFont boldSystemFontOfSize:
    active ? kBarFontPt : (kBarFontPt + kSteadyFontBump)]];
  [self refreshAppearance];

  [[self progress] setHidden:!active];
  if (active) {
    /* Plain property setter, not -setProgress:animated: (iOS 5.0+). */
    [[self progress] setProgress:[[self queue] displayDeterminate]
      ? (float)([[self queue] displayPercent] / 100.0) : kIndetFill];
  }

  [self resizeToContent];
}

/* Size to the wider of the label or the bar (they stack vertically, so width is
 * the max, not the sum) and ask the toolbar to re-measure so the flexible-space
 * sandwich re-centers us. */
- (void)resizeToContent
{
  [[self label] sizeToFit];
  CGFloat w = [[self label] bounds].size.width;
  if (![[self progress] isHidden] && kProgressW > w) w = kProgressW;
  CGRect f = [self frame];
  f.size = CGSizeMake(w, kBarHeight);
  [self setFrame:f];
  [self setNeedsLayout];
  [self refreshHostingToolbar];
}

/* A bar-button custom view's width is only measured when the toolbar lays out
 * its items, and the toolbar does not observe our frame changes. After our
 * width changes, re-set the toolbar's own items to force a re-measure (a no-op
 * when we're off-screen and not hosted in any toolbar). */
- (void)refreshHostingToolbar
{
  UIView *v = [self superview];
  while (v != nil && ![v isKindOfClass:[UIToolbar class]]) v = [v superview];
  if (v == nil) return;
  UIToolbar *toolbar = (UIToolbar *)v;
  [toolbar setItems:[toolbar items] animated:NO];
}

/* Steady mode: the label alone, centered. Bar mode: the label STACKED on top of
 * the progress bar, the whole group centered vertically and each element
 * centered horizontally (the label's centered alignment fills the full width;
 * the fixed-width bar is centered by inset). */
- (void)layoutSubviews
{
  [super layoutSubviews];
  CGFloat w  = [self bounds].size.width;
  CGFloat h  = [self bounds].size.height;
  CGFloat lh = [[self label] bounds].size.height;
  /* Geometric centering leaves single-line text looking low: the font's line
   * box reserves descender room the word's ink only partly fills, so the ink
   * sits below the box center. Nudge up by a multiple of the descender to
   * optically center the ink (see kInkLiftFactorSteady / kInkLiftFactorBar).
   * descender is negative, so this raises the label, and it scales with the
   * current font (13pt bar phase vs 15pt steady). The factor is per mode so the
   * text-only and stacked layouts tune independently. */
  CGFloat descender = [[[self label] font] descender];

  if ([[self progress] isHidden]) {
    CGFloat optShift = descender * kInkLiftFactorSteady;
    [[self label] setFrame:CGRectMake(0.0f, (h - lh) * 0.5f + optShift, w, lh)];
    [[self shineLabel] setFrame:[[self label] frame]];  /* overlay for the ping shimmer */
    return;
  }

  CGFloat optShift = descender * kInkLiftFactorBar;
  CGFloat ph = [[self progress] bounds].size.height;  /* the control's natural track height */
  CGFloat top = (h - (lh + kStackGap + ph)) * 0.5f;
  [[self label] setFrame:CGRectMake(0.0f, top + optShift, w, lh)];
  [[self progress] setFrame:CGRectMake((w - kProgressW) * 0.5f, top + lh + kStackGap,
                                   kProgressW, ph)];
}

#pragma mark - ENILSyncStatusQueueDelegate

- (void)syncStatusQueueDidChange:(ENILSyncStatusQueue *)queue
{
  (void)queue;
  [self render];
}

- (void)syncStatusQueueRequestsShimmer:(ENILSyncStatusQueue *)queue
{
  (void)queue;
  [self playShimmer];
}

/* One-shot light-gray sweep over the steady-state text, fired on each SSE ping
 * (the queue gates it to idle-live with nothing transient up). A clear->white->
 * clear gradient masks the gray shine copy; animating the mask's `locations`
 * slides the opaque band across, so a soft gray highlight travels over the
 * base text. Runs on the render server, so it survives chat-list scrolling. */
- (void)playShimmer
{
  UILabel *base = [self label];
  if ([base bounds].size.width < 1.0 || [[base text] length] == 0) return;

  UILabel *shine = [self shineLabel];
  [shine setFrame:[base frame]];
  [shine setText:[base text]];
  [shine setFont:[base font]];
  [shine setHidden:NO];

  CAGradientLayer *mask = [CAGradientLayer layer];
  [mask setFrame:[shine bounds]];
  [mask setStartPoint:CGPointMake(0.0, 0.5)];
  [mask setEndPoint:CGPointMake(1.0, 0.5)];
  [mask setColors:[NSArray arrayWithObjects:
    (__bridge id)[[UIColor clearColor] CGColor],
    (__bridge id)[[UIColor whiteColor] CGColor],
    (__bridge id)[[UIColor clearColor] CGColor], nil]];
  [mask setLocations:[NSArray arrayWithObjects:
    [NSNumber numberWithFloat:0.0f],
    [NSNumber numberWithFloat:0.5f],
    [NSNumber numberWithFloat:1.0f], nil]];
  [[shine layer] setMask:mask];

  CABasicAnimation *anim = [CABasicAnimation animationWithKeyPath:@"locations"];
  [anim setDuration:kShineDuration];
  [anim setFromValue:[NSArray arrayWithObjects:
    [NSNumber numberWithFloat:-0.3f],
    [NSNumber numberWithFloat:-0.15f],
    [NSNumber numberWithFloat:0.0f], nil]];
  [anim setToValue:[NSArray arrayWithObjects:
    [NSNumber numberWithFloat:1.0f],
    [NSNumber numberWithFloat:1.15f],
    [NSNumber numberWithFloat:1.3f], nil]];
  [anim setRemovedOnCompletion:YES];
  [anim setDelegate:self];   /* to tear the mask down on completion */
  [mask addAnimation:anim forKey:@"shine"];
}

/* CAAnimation delegate (informal) — hide the shine copy and drop the mask once
 * the sweep finishes so the base label is fully opaque again. */
- (void)animationDidStop:(CAAnimation *)anim finished:(BOOL)flag
{
  (void)anim; (void)flag;
  [[self shineLabel] setHidden:YES];
  [[[self shineLabel] layer] setMask:nil];
}

- (void)dealloc
{
  /* Clear the assign back-pointer before ARC releases the queue, so a late
   * main-thread notification can never message a half-torn-down view. */
  [_queue setDelegate:nil];
}

@end
