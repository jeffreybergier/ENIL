//
//  MessageSendViewController.m
//  ENIL
//

#import "MessageSendViewController.h"
#import "AIFontAwesome.h"
#import <QuartzCore/QuartzCore.h>  /* CALayer corner/border on the input field */

static const CGFloat kCtrlGap         = 4.0;   /* uniform gap between every bar control and the
                                                * bar edges / its neighbours. Exception: the
                                                * dismiss button sits flush (0) against the left
                                                * edge and the field — its tap area extends past
                                                * the visible chevron, so the contact is invisible. */
static const CGFloat kSendDiameter    = 36.0;  /* prominent circular Send button */
static const CGFloat kAttachGroupWidth = 64.0; /* emoji + photo, joined as one pill;
                                                * 32pt slots → 6pt flank per glyph.
                                                * Doubles as the pill's *height* once
                                                * rotated to portrait in the expanded
                                                * bar (see kBarExpandedDelta). */
static const CGFloat kDismissWidth    = 22.0;  /* slim left-anchored keyboard-dismiss button */
static const CGFloat kAttachGlyphPt   = 20.0;  /* emoji + photo glyph point size */
static const CGFloat kSendIconPt      = 20.0;  /* paper-plane ink; centered by the rasterizer
                                                * in a kSendDiameter canvas (single source of
                                                * padding — the image fills the disc exactly) */
static const CGFloat kDismissGlyphPt  = 14.0;  /* smaller chevron glyph */

/* Height model (binary, mirrors macOS kBarHeightCollapsed/kBarHeightExpanded):
 * a single line sits in the collapsed height; once the text wraps past one
 * line the bar grows by kBarExpandedDelta and the field/web view follow. */
static const CGFloat kBarCollapsedHeight = 44.0;
/* Expanded delta makes room for the right-hand column — send disc on top, the
 * attach group rotated to portrait beneath it (portraitHeight == kAttachGroupWidth):
 *   inset + send + gap + portraitHeight + inset
 *   = 4 + 36 + 4 + 64 + 4 = 112  →  delta = 112 - 44 = 68. */
static const CGFloat kBarExpandedDelta   = 68.0;

static const CGFloat kFieldInsetV  = 4.0;   /* field top/bottom inset; equals kCtrlGap so every
                                             * control's top/bottom gap matches its horizontal one */
static const CGFloat kButtonHeight = 36.0;  /* collapsed dismiss + attach-group height; equals the
                                             * field and send height (kBarCollapsedHeight -
                                             * 2*kFieldInsetV = 36) and the send diameter, so every
                                             * control matches height and sits vertically centered
                                             * with equal top/bottom gaps. Also the attach group's
                                             * *width* once rotated to portrait in the expanded bar. */
static const CGFloat kFieldFontPt  = 16.0;
static const CGFloat kFieldRadius  = 8.0;   /* shared by the field's layer + the inner-shadow overlay path */

/* Visible, single-UTF-16-unit placeholder standing in for one inline sticon in
 * the plain-text field. Identity lives in self.sticonRuns, aligned to these
 * markers by ordinal (the k-th marker <-> sticonRuns[k]). U+25C6 BLACK DIAMOND
 * renders in the system font on the 4.3 floor and is effectively never typed by
 * hand — no attributed text, no Font Awesome, no per-keystroke font fixups. */
static const unichar kSticonMarker = 0x25C6;

/* Immutable identity for one inline sticon — the iOS analog of the macOS compose
 * bar's SticonAttachment. It is deliberately NOT a text attribute: the field
 * stays plain text (one kSticonMarker per sticon) and MessageSendViewController
 * holds an ordered array of these, aligned to the markers by ordinal. Private to
 * this file; the controller is the only thing that ever needs it. */
@interface SticonModel : NSObject
@property (nonatomic, copy, readonly) NSString *packageId;
@property (nonatomic, copy, readonly) NSString *sticonId;
@property (nonatomic, copy, readonly) NSString *altText;   /* may be nil */

/* packageId and sticonId are required; altText is optional. Raises
 * NSInvalidArgumentException if either required field is missing. */
- (instancetype)initWithPackageId:(NSString *)packageId
                         sticonId:(NSString *)sticonId
                          altText:(NSString *)altText;
@end

@implementation SticonModel

- (instancetype)initWithPackageId:(NSString *)packageId
                         sticonId:(NSString *)sticonId
                          altText:(NSString *)altText
{
  if (![packageId length] || ![sticonId length]) {
    [NSException raise:NSInvalidArgumentException
                format:@"[SticonModel.init] packageId and sticonId are required"];
  }
  if ((self = [super init])) {
    _packageId = [packageId copy];
    _sticonId  = [sticonId copy];
    _altText   = [altText copy];
  }
  return self;
}

@end

/* Recessed inner shadow for the input field, reproducing the inset look that
 * UITextField's UITextBorderStyleRoundedRect drew for itself (CALayer has no
 * inner-shadow property, and a sublayer on the scrolling UITextView would
 * scroll away). A non-scrolling, touch-transparent overlay sits over the field
 * and draws the shadow in -drawRect:. */
@interface _FieldInnerShadowView : UIView
@end

@implementation _FieldInnerShadowView

- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    [self setOpaque:NO];
    [self setBackgroundColor:[UIColor clearColor]];
    [self setUserInteractionEnabled:NO];       /* touches fall through to the field */
    [self setContentMode:UIViewContentModeRedraw];  /* re-draw on resize, not stretch */
  }
  return self;
}

/* Inner-shadow recipe that works on the 4.3 floor (no bezierPathByReversingPath,
 * which is 6.0): clip to the rounded field, then fill the ring *outside* the
 * rounded path with even-odd winding while a shadow is set. The fill lands
 * outside the clip so it never paints — only its shadow projects inward,
 * leaving a clean recessed edge with no visible band. */
- (void)drawRect:(CGRect)rect
{
  (void)rect;
  CGContextRef ctx = UIGraphicsGetCurrentContext();
  if (ctx == NULL) return;
  CGRect b = [self bounds];
  UIBezierPath *rounded =
    [UIBezierPath bezierPathWithRoundedRect:b cornerRadius:kFieldRadius];

  CGContextSaveGState(ctx);
  [rounded addClip];
  CGContextSetShadowWithColor(ctx, CGSizeMake(0.0f, 1.0f), 3.0f,
    [[UIColor colorWithWhite:0.0f alpha:0.22f] CGColor]);
  CGMutablePathRef ring = CGPathCreateMutable();
  CGPathAddRect(ring, NULL, CGRectInset(b, -(kFieldRadius * 2.0f + 8.0f),
                                           -(kFieldRadius * 2.0f + 8.0f)));
  CGPathAddPath(ring, NULL, [rounded CGPath]);
  CGContextAddPath(ctx, ring);
  CGContextSetFillColorWithColor(ctx, [[UIColor blackColor] CGColor]);
  CGContextEOFillPath(ctx);
  CGPathRelease(ring);
  CGContextRestoreGState(ctx);
}

@end

/* Prominent circular Send button: a filled blue disc with a recessed inner
 * shadow, carrying a white paper-plane glyph. The disc + shadow are painted in
 * -drawRect: (which renders into the button's own layer, *below* the imageView
 * that holds the white glyph), reusing the clip-to-shape + even-odd ring shadow
 * recipe from _FieldInnerShadowView. The fill flattens toward grey when
 * disabled and darkens while pressed; UIButton does not redraw a custom
 * -drawRect: on state changes, so -setEnabled:/-setHighlighted: force it. */
@interface _SendButton : UIButton
@end

@implementation _SendButton

- (void)setEnabled:(BOOL)enabled
{
  [super setEnabled:enabled];
  [self setNeedsDisplay];
}

- (void)setHighlighted:(BOOL)highlighted
{
  [super setHighlighted:highlighted];
  [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect
{
  (void)rect;
  CGContextRef ctx = UIGraphicsGetCurrentContext();
  if (ctx == NULL) return;
  CGRect b = [self bounds];

  UIColor *fill;
  if (![self isEnabled]) {
    fill = [UIColor colorWithWhite:0.74f alpha:1.0f];
  } else if ([self isHighlighted]) {
    fill = [UIColor colorWithRed:0.0f green:0.40f blue:0.80f alpha:1.0f];
  } else {
    fill = [UIColor colorWithRed:0.0f green:0.52f blue:1.0f alpha:1.0f];
  }
  UIBezierPath *disc = [UIBezierPath bezierPathWithOvalInRect:b];
  [fill setFill];
  [disc fill];

  /* Inner shadow: clip to the disc, then fill the ring *outside* it with
   * even-odd winding while a shadow is set, so only the projected shadow lands
   * inside — a clean recessed edge with no visible band (the field uses the
   * same trick). */
  CGContextSaveGState(ctx);
  [disc addClip];
  CGContextSetShadowWithColor(ctx, CGSizeMake(0.0f, 1.0f), 2.5f,
    [[UIColor colorWithWhite:0.0f alpha:0.35f] CGColor]);
  CGMutablePathRef ring = CGPathCreateMutable();
  CGPathAddRect(ring, NULL, CGRectInset(b, -b.size.width, -b.size.height));
  CGPathAddEllipseInRect(ring, NULL, b);
  CGContextAddPath(ctx, ring);
  CGContextSetFillColorWithColor(ctx, [[UIColor blackColor] CGColor]);
  CGContextEOFillPath(ctx);
  CGPathRelease(ring);
  CGContextRestoreGState(ctx);
}

@end

/* Plain icon button with a *visible* pressed state. UIButtonTypeCustom's only
 * built-in highlight is adjustsImageWhenHighlighted, which merely dims the glyph
 * image — imperceptible on these already-dark AIFontAwesome marks over a light
 * bar. So, like _SendButton, this repaints its own background on highlight:
 * nothing at rest, a subtle inset rounded fill behind the glyph while pressed.
 * UIButton draws -drawRect: below its image/title subviews, so the glyph stays
 * crisp on top. Used by the dismiss chevron and both attach-group buttons. */
@interface _PressableIconButton : UIButton
@end

@implementation _PressableIconButton

- (void)setHighlighted:(BOOL)highlighted
{
  [super setHighlighted:highlighted];
  [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect
{
  (void)rect;
  if (![self isHighlighted]) return;
  /* Inset so the fill reads as a key behind the glyph rather than flooding the
   * whole tap target (the dismiss button's tap area runs to the screen edge),
   * and so inside the attach pill it sits clear of the rounded ends + divider. */
  UIBezierPath *bg =
    [UIBezierPath bezierPathWithRoundedRect:CGRectInset([self bounds], 2.0f, 2.0f)
                               cornerRadius:5.0f];
  [[UIColor colorWithWhite:0.0f alpha:0.12f] setFill];
  [bg fill];
}

@end

/* Styled button group joining the Emoji/sticker and Photo buttons into one
 * rounded pill — the iOS counterpart to the macOS compose bar's segmented
 * control. Built custom rather than with UISegmentedControl: the 4.3 floor
 * can't tint per-segment glyph images and its segment styling is unreliable,
 * whereas a pill hosting two plain icon buttons gives full control and stays
 * warning-clean. Touches go straight to the child buttons; this view only
 * paints the shared chrome (fill, border, divider) in -drawRect:. */
@interface _AttachmentGroupView : UIView
@property (nonatomic, strong) UIButton *emojiButton;
@property (nonatomic, strong) UIButton *imageButton;
@end

@implementation _AttachmentGroupView

- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    [self setOpaque:NO];
    [self setBackgroundColor:[UIColor clearColor]];
    [self setContentMode:UIViewContentModeRedraw];  /* re-draw on resize */
    _emojiButton = [[_PressableIconButton alloc] initWithFrame:CGRectZero];
    _imageButton = [[_PressableIconButton alloc] initWithFrame:CGRectZero];
    /* Center the FA glyph at its natural size — never let UIKit scale it. */
    [[_emojiButton imageView] setContentMode:UIViewContentModeCenter];
    [[_imageButton imageView] setContentMode:UIViewContentModeCenter];
    [self addSubview:_emojiButton];
    [self addSubview:_imageButton];
  }
  return self;
}

- (CGFloat)hairline
{
  CGFloat scale = [[UIScreen mainScreen] scale];
  if (scale <= 0.0f) scale = 1.0f;
  return 1.0f / scale;
}

/* Orientation is read straight from the bounds aspect, so the group adapts
 * automatically as its frame animates between the collapsed landscape size and
 * the expanded portrait size — no explicit state to set or keep in sync. */
- (BOOL)isPortrait
{
  return [self bounds].size.height > [self bounds].size.width;
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  CGRect b = [self bounds];
  if ([self isPortrait]) {
    /* Portrait (expanded): emoji on top, photo on the bottom. */
    CGFloat mid = (CGFloat)floorf((float)(b.size.height / 2.0f));
    [[self emojiButton] setFrame:CGRectMake(0.0f, 0.0f, b.size.width, mid)];
    [[self imageButton] setFrame:CGRectMake(0.0f, mid, b.size.width, b.size.height - mid)];
  } else {
    /* Landscape (collapsed): emoji on the left, photo on the right. */
    CGFloat mid = (CGFloat)floorf((float)(b.size.width / 2.0f));
    [[self emojiButton] setFrame:CGRectMake(0.0f, 0.0f, mid, b.size.height)];
    [[self imageButton] setFrame:CGRectMake(mid, 0.0f, b.size.width - mid, b.size.height)];
  }
}

- (void)drawRect:(CGRect)rect
{
  (void)rect;
  CGFloat hair = [self hairline];
  /* Inset by half the hairline so the stroke sits fully inside the bounds. */
  UIBezierPath *pill = [UIBezierPath
    bezierPathWithRoundedRect:CGRectInset([self bounds], hair * 0.5f, hair * 0.5f)
                 cornerRadius:6.0f];
  [[UIColor colorWithWhite:1.0f alpha:1.0f] setFill];
  [pill fill];
  [pill setLineWidth:hair];
  [[UIColor colorWithWhite:0.78f alpha:1.0f] setStroke];
  [pill stroke];

  /* Divider between the two buttons: horizontal when portrait (stacked),
   * vertical when landscape (side by side). */
  UIBezierPath *divider = [UIBezierPath bezierPath];
  if ([self isPortrait]) {
    CGFloat midY = (CGFloat)floorf((float)([self bounds].size.height / 2.0f));
    [divider moveToPoint:CGPointMake(5.0f, midY)];
    [divider addLineToPoint:CGPointMake([self bounds].size.width - 5.0f, midY)];
  } else {
    CGFloat midX = (CGFloat)floorf((float)([self bounds].size.width / 2.0f));
    [divider moveToPoint:CGPointMake(midX, 5.0f)];
    [divider addLineToPoint:CGPointMake(midX, [self bounds].size.height - 5.0f)];
  }
  [divider setLineWidth:hair];
  [[UIColor colorWithWhite:0.82f alpha:1.0f] setStroke];
  [divider stroke];
}

@end

@interface MessageSendViewController () <UITextViewDelegate>
/* Top bevel: a dark hairline + a light highlight underneath it give the bar a
 * sense of depth over the (white) web view it borders. */
@property (nonatomic, strong) UIView *topSeparator;
@property (nonatomic, strong) UIView *topHighlight;
@property (nonatomic, strong) UIButton *dismissButton;
@property (nonatomic, strong) _AttachmentGroupView *attachGroup;  /* emoji + photo pill */
@property (nonatomic, strong) UITextView *field;
@property (nonatomic, strong) _FieldInnerShadowView *fieldShadow;  /* recessed inset look */
@property (nonatomic, strong) UILabel *placeholderLabel;  /* UITextView has no native placeholder */
@property (nonatomic, strong) _SendButton *sendButton;
@property (nonatomic, assign) BOOL composing;
@property (nonatomic, assign) BOOL expanded;
/* Authoritative, ordered identity model for the inline sticons currently in the
 * field — sticonRuns[k] is the k-th kSticonMarker in document order. The field
 * stays plain text: each sticon is a single marker character, and its identity
 * (packageId/sticonId/altText) is held here and realigned to the markers by
 * counting marker codepoints. No NSAttributedString, so none of the iOS 6
 * attribute-dropping / font-inheritance problems apply. */
@property (nonatomic, strong) NSMutableArray *sticonRuns;
@end

@implementation MessageSendViewController

- (instancetype)initWithFrame:(CGRect)frame
{
  if ((self = [super initWithFrame:frame])) {
    [self setBackgroundColor:[UIColor colorWithWhite:0.95f alpha:1.0f]];
    /* Flexible width + top margin keeps the bar full-width and bottom-pinned
     * as the host resizes it; -layoutSubviews handles the internal split. No
     * Auto Layout on the 4.3 floor. */
    [self setAutoresizingMask:UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin];
    _sticonRuns = [[NSMutableArray alloc] init];
    [self buildSubviews];
  }
  return self;
}

/* Shared glyph factory for the compose-bar buttons. Renders at the device
 * scale so the icons stay crisp on Retina; the dark template glyph matches how
 * the macOS toolbar tints the same icons. pointSize varies per button (the
 * dismiss chevron is smaller than the sticker/send glyphs). */
- (UIImage *)iconImageForIcon:(AIFontAwesomeIcon)icon
                        style:(AIFontAwesomeStyle)style
                    pointSize:(CGFloat)pointSize
{
  CGFloat scale = [[UIScreen mainScreen] scale];
  if (scale <= 0.0f) scale = 1.0f;
  return [AIFontAwesome imageForIcon:icon
                            style:style
                        pointSize:pointSize
                            scale:scale];
}

/* White glyph for the Send/attach buttons. These paint the glyph over a custom
 * blue disc / custom-drawn face, which UIKit does NOT auto-tint, so the white
 * must be baked into the image — AIFontAwesome rasterises directly in white. The
 * iconSize/canvasSize split pads + centers the glyph so a wide glyph's
 * antialiased edges don't clip at the bitmap boundary. */
- (UIImage *)whiteIconImageForIcon:(AIFontAwesomeIcon)icon
                             style:(AIFontAwesomeStyle)style
                          iconSize:(CGFloat)iconSize
                        canvasSize:(CGFloat)canvasSize
{
  return [AIFontAwesome imageForIcon:icon
                             style:style
                          iconSize:iconSize
                        canvasSize:canvasSize
                             color:[UIColor whiteColor]
                             scale:0.0f];  /* 0 -> [UIScreen mainScreen].scale */
}

/* A crisp 1px line in screen terms (0.5pt on Retina), for the top bevel. */
- (CGFloat)hairline
{
  CGFloat scale = [[UIScreen mainScreen] scale];
  if (scale <= 0.0f) scale = 1.0f;
  return 1.0f / scale;
}

- (void)buildSubviews
{
  /* Top bevel hairlines. Frames are set in -layoutSubviews; both stretch the
   * full width there. */
  UIView *sep = [[UIView alloc] initWithFrame:CGRectZero];
  [sep setBackgroundColor:[UIColor colorWithWhite:0.72f alpha:1.0f]];
  [sep setUserInteractionEnabled:NO];
  [self addSubview:sep];
  [self setTopSeparator:sep];

  UIView *hi = [[UIView alloc] initWithFrame:CGRectZero];
  [hi setBackgroundColor:[UIColor colorWithWhite:1.0f alpha:1.0f]];
  [hi setUserInteractionEnabled:NO];
  [self addSubview:hi];
  [self setTopHighlight:hi];

  /* Keyboard-dismiss (fa-solid fa-chevron-down). Top-anchored, flush top-left;
   * hidden at rest (alpha 0). The compose state drives its alpha, applied in
   * -layoutSubviews so it animates with the keyboard. */
  _PressableIconButton *dis = [[_PressableIconButton alloc] initWithFrame:CGRectZero];
  [dis setAlpha:0.0f];
  [dis setImage:[self iconImageForIcon:AIFAChevronDown
                                style:AIFontAwesomeStyleSolid
                            pointSize:kDismissGlyphPt]
       forState:UIControlStateNormal];
  [[dis imageView] setContentMode:UIViewContentModeCenter];  /* natural size, no scaling */
  [dis addTarget:self
          action:@selector(dismissTapped)
        forControlEvents:UIControlEventTouchUpInside];
  [self addSubview:dis];
  [self setDismissButton:dis];

  /* UITextView (multi-line) replacing the old single-line UITextField. The
   * rounded-rect bubble that UITextBorderStyleRoundedRect used to draw is
   * recreated on the layer here. clipsToBounds keeps the text inside the
   * rounded corners. */
  UITextView *f = [[UITextView alloc] initWithFrame:CGRectZero];
  [f setDelegate:self];
  [f setFont:[UIFont systemFontOfSize:kFieldFontPt]];
  [f setBackgroundColor:[UIColor whiteColor]];
  [f setScrollEnabled:YES];  /* once the text exceeds the expanded height, scroll in place */
  [f setClipsToBounds:YES];
  [[f layer] setCornerRadius:kFieldRadius];
  [[f layer] setBorderWidth:[self hairline]];
  [[f layer] setBorderColor:[[UIColor colorWithWhite:0.8f alpha:1.0f] CGColor]];
  [self addSubview:f];
  [self setField:f];

  /* Inner-shadow overlay: above the field, below the placeholder, so the
   * recessed edge reads but the placeholder/text stay crisp. */
  _FieldInnerShadowView *shadow =
    [[_FieldInnerShadowView alloc] initWithFrame:CGRectZero];
  [self addSubview:shadow];
  [self setFieldShadow:shadow];

  /* Placeholder overlay — UITextView has no native placeholder. Positioned
   * over the field's first text line in -layoutSubviews; hidden once the user
   * types. */
  UILabel *ph = [[UILabel alloc] initWithFrame:CGRectZero];
  [ph setText:NSLocalizedString(@"Message", nil)];
  [ph setFont:[f font]];
  [ph setTextColor:[UIColor colorWithWhite:0.6f alpha:1.0f]];
  [ph setBackgroundColor:[UIColor clearColor]];
  [ph setUserInteractionEnabled:NO];
  [self addSubview:ph];
  [self setPlaceholderLabel:ph];

  /* Attachment group + Send on the right. The group (emoji + photo, joined as a
   * pill) is visible at all times — including with the keyboard dismissed — so
   * the field always stops short of its slot (see -fieldFrame). Collapsed it's a
   * landscape row beside send; expanded (composing + multi-line) it rotates to
   * portrait and tucks under send (see -layoutSubviews). */
  _AttachmentGroupView *grp =
    [[_AttachmentGroupView alloc] initWithFrame:CGRectZero];
  [grp setAlpha:1.0f];
  /* AIFAFaceGrin (regular) — same glyph as the macOS picker's Emoji segment. */
  [[grp emojiButton] setImage:[self iconImageForIcon:AIFAFaceGrin
                                            style:AIFontAwesomeStyleRegular
                                        pointSize:kAttachGlyphPt]
                   forState:UIControlStateNormal];
  [[grp emojiButton] addTarget:self
                      action:@selector(stickersTapped)
                forControlEvents:UIControlEventTouchUpInside];
  /* AIFAImage (regular) — the macOS toolbar's Photo segment icon. */
  [[grp imageButton] setImage:[self iconImageForIcon:AIFAImage
                                            style:AIFontAwesomeStyleRegular
                                        pointSize:kAttachGlyphPt]
                   forState:UIControlStateNormal];
  [[grp imageButton] addTarget:self
                      action:@selector(imageTapped)
                forControlEvents:UIControlEventTouchUpInside];
  [self addSubview:grp];
  [self setAttachGroup:grp];

  /* Send — a prominent blue disc carrying a white paper-plane glyph (see
   * _SendButton). Plain alloc/init yields a UIButtonTypeCustom instance of the
   * subclass, warning-clean on the 4.3 floor (RoundedRect is deprecated, System
   * is 7.0). Disabled until there is sendable text. */
  _SendButton *btn = [[_SendButton alloc] initWithFrame:CGRectZero];
  [btn setImage:[self whiteIconImageForIcon:AIFAPaperPlane
                                     style:AIFontAwesomeStyleRegular
                                  iconSize:kSendIconPt
                                canvasSize:kSendDiameter]
       forState:UIControlStateNormal];
  [[btn imageView] setContentMode:UIViewContentModeCenter];  /* natural size, no scaling */
  [btn setEnabled:NO];
  [btn addTarget:self
          action:@selector(sendTapped)
        forControlEvents:UIControlEventTouchUpInside];
  [self addSubview:btn];
  [self setSendButton:btn];

  [self updatePlaceholderVisibility];
}

#pragma mark - Layout

/* The field's frame for the current composing state. Composing: the dismiss
 * button is flush at the left edge and the field butts directly against it
 * (x = kDismissWidth, no gap — the chevron's tap edges are invisible), then the
 * field yields kCtrlGap before the attach group (so it gives up width for two
 * kCtrlGaps + the group + send on the right). Resting (composing == NO): dismiss
 * + group are hidden, the field spans from a kCtrlGap left margin over their
 * slots up to kCtrlGap before send (three kCtrlGaps: outer-left, field|send,
 * outer-right). Vertically it fills the bar inset by kFieldInsetV, so it grows
 * with -preferredHeight. Computed from the bounds so it survives rotation +
 * expansion. */
- (CGRect)fieldFrame
{
  CGRect b = [self bounds];
  CGFloat x = [self composing] ? kDismissWidth : kCtrlGap;
  /* The field's right edge sits kCtrlGap left of the right-hand cluster.
   * Expanded (composing + multi-line): the cluster is a single narrow column
   * (send on top, attach group portrait beneath), so the field only yields the
   * send-column width — it gets much wider. Otherwise the attach group sits in
   * its landscape slot beside send (collapsed compose AND now the resting state,
   * since the group stays visible), so the field yields both. Keyed off
   * -showsExpanded, which is exactly "is the group in its portrait column?". */
  CGFloat rightEdge = [self showsExpanded]
    ? (b.size.width - kSendDiameter - kCtrlGap * 2)
    : (b.size.width - kSendDiameter - kAttachGroupWidth - kCtrlGap * 3);
  CGFloat w = rightEdge - x;
  if (w < 0.0f) w = 0.0f;
  return CGRectMake(x, kFieldInsetV, w, b.size.height - kFieldInsetV * 2);
}

- (void)layoutSubviews
{
  [super layoutSubviews];
  CGFloat w = [self bounds].size.width;
  CGFloat hair = [self hairline];

  [[self topSeparator] setFrame:CGRectMake(0.0f, 0.0f, w, hair)];
  [[self topHighlight] setFrame:CGRectMake(0.0f, hair, w, hair)];

  /* The dismiss chevron is visible only while composing — it has nothing to
   * dismiss once the keyboard is down. Fading it here (rather than in
   * -setComposing:) lets the alpha ride the same animation as the frames when
   * the host calls -layoutIfNeeded inside its keyboard block. The attach group
   * (emoji + photo) stays visible at all times so stickers/images can be opened
   * with the keyboard dismissed. */
  [[self dismissButton] setAlpha:_composing ? 1.0f : 0.0f];
  [[self attachGroup] setAlpha:1.0f];

  /* Dismiss + send are top-anchored and never move between collapsed and
   * expanded: dismiss flush top-left (its tap edges are invisible), send flush
   * top-right. In the collapsed bar the top inset equals the bottom inset so
   * they read as vertically centered; when the bar grows downward for the
   * expanded layout they stay pinned at the top — which is exactly "send moves
   * to the top-right corner". */
  [[self dismissButton] setFrame:CGRectMake(0.0f, kFieldInsetV, kDismissWidth, kButtonHeight)];
  [[self sendButton] setFrame:CGRectMake(w - kSendDiameter - kCtrlGap, kFieldInsetV,
               kSendDiameter, kSendDiameter)];

  if ([self showsExpanded]) {
    /* Expanded: the attach group rotates to portrait — a 90° turn of its
     * collapsed size (kButtonHeight wide x kAttachGroupWidth tall) — and tucks
     * directly under the send disc, right-aligned with it (both kSendDiameter
     * wide, since kButtonHeight == kSendDiameter). */
    [[self attachGroup] setFrame:CGRectMake(w - kSendDiameter - kCtrlGap,
                 kFieldInsetV + kSendDiameter + kCtrlGap,
                 kButtonHeight, kAttachGroupWidth)];
  } else {
    /* Collapsed: landscape, in the top row just left of the send disc. */
    [[self attachGroup] setFrame:CGRectMake(w - kSendDiameter - kAttachGroupWidth - kCtrlGap * 2,
                 kFieldInsetV, kAttachGroupWidth, kButtonHeight)];
  }

  CGRect ff = [self fieldFrame];
  [[self field] setFrame:ff];
  [[self fieldShadow] setFrame:ff];  /* overlay tracks the field exactly */

  /* Align the placeholder with the first text line. Pre-iOS 7 UITextView lays
   * text out at roughly an (8, 8) content inset; match that. */
  CGFloat lineH = (CGFloat)ceilf((float)[[[self field] font] lineHeight]);
  [[self placeholderLabel] setFrame:CGRectMake(ff.origin.x + 8.0f, ff.origin.y + 8.0f,
               ff.size.width - 16.0f, lineH)];
}

- (void)setComposing:(BOOL)composing
{
  /* State only. The dismiss/attach-group fade and the field/button frames are
   * applied in -layoutSubviews, which the host runs via -layoutIfNeeded inside
   * its keyboard begin/commit block so the whole change animates together.
   * Recording the state here (before the host reads -preferredHeight) is what
   * lets the bar collapse the instant composing ends — see -showsExpanded. */
  _composing = composing ? YES : NO;
  [self setNeedsLayout];
}

#pragma mark - Height

/* The expanded (tall) layout applies only while composing. On losing first
 * responder the bar drops to the short collapsed layout even if the draft still
 * wraps to several lines; `expanded` stays the text-driven truth, so the tall
 * layout snaps back when the keyboard (and first responder) return. */
- (BOOL)showsExpanded
{
  return ([self expanded] && [self composing]) ? YES : NO;
}

- (CGFloat)preferredHeight
{
  return kBarCollapsedHeight + ([self showsExpanded] ? kBarExpandedDelta : 0.0f);
}

/* Detect "more than one line" by asking the text view how tall it needs to be
 * at a *fixed* width, compared against a single-line height (font line height
 * plus UITextView's ~16pt top+bottom text inset). Binary, like macOS.
 *
 * Crucially the measurement width is the *collapsed* field width, not the live
 * one. The expanded layout makes the field much wider, so measuring at the
 * current width would let a 2-line wrap shrink back to one line the instant we
 * expand — then re-wrap once collapsed — flip-flopping the bar on every
 * keystroke. Anchoring to the stable collapsed width breaks that feedback loop.
 * -sizeThatFits: (not the deprecated -sizeWithFont:) keeps it warning-clean. */
- (void)updateExpansion
{
  CGFloat lineH = (CGFloat)ceilf((float)[[[self field] font] lineHeight]);
  if (lineH <= 0.0f) lineH = 20.0f;
  CGFloat oneLine = lineH + 16.0f;
  CGFloat collapsedW = [self bounds].size.width - kDismissWidth
    - kAttachGroupWidth - kSendDiameter - kCtrlGap * 3;
  if (collapsedW < 1.0f) collapsedW = 1.0f;
  CGFloat needed =
    [[self field] sizeThatFits:CGSizeMake(collapsedW, CGFLOAT_MAX)].height;
  BOOL nowExpanded = (needed > oneLine + lineH * 0.5f);
  if (nowExpanded == [self expanded]) return;
  [self setExpanded:nowExpanded];
  [[self delegate] messageSendDidChangeHeight:self];
}

#pragma mark - Text + actions

/* Single source of truth for "is there something sendable?" — mirrors macOS
 * -canSendCurrentMessage. Drives both the Send button and the placeholder. */
- (NSString *)trimmedText
{
  return [[[self field] text] stringByTrimmingCharactersInSet:
            [NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

- (void)updatePlaceholderVisibility
{
  [[self placeholderLabel] setHidden:([[[self field] text] length] > 0)];
}

- (void)dismissTapped
{
  [[self field] resignFirstResponder];
}

- (void)stickersTapped
{
  [[self delegate] messageSendDidTapStickers:self];
}

- (void)imageTapped
{
  [[self delegate] messageSendDidTapImagePicker:self];
}

- (void)sendTapped
{
  NSMutableArray *resources = [NSMutableArray array];
  NSString *text = [[self wireTextIntoResources:resources] stringByTrimmingCharactersInSet:
                      [NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if ([text length] == 0) return;
  /* Clear optimistically before handing off so the field is ready for the next
   * message even if the send path takes a moment. Clearing also collapses the
   * bar back to a single line via -updateExpansion. */
  [[self field] setText:@""];
  [[self sticonRuns] removeAllObjects];
  [[self sendButton] setEnabled:NO];
  [self updatePlaceholderVisibility];
  [self updateExpansion];
  [[self delegate] messageSend:self didSubmitText:text sticonResources:resources];
}

#pragma mark - Inline sticons

/* Insert an inline sticon at the caret: register its identity in self.sticonRuns
 * at the ordinal matching the caret, and splice one kSticonMarker into the plain
 * text at the same spot. No attributed text and no fonts — the marker is one
 * ordinary character, and identity is realigned to the markers by ordinal (see
 * -sticonCountInString:range:). Works on every floor (iOS 4.3+). Any runs whose
 * markers sat inside the replaced selection are dropped first, then the new run
 * is inserted at the count of markers preceding the caret, keeping
 * sticonRuns[k] aligned to the k-th marker in document order. */
- (void)insertSticonWithPackageId:(NSString *)packageId
                         sticonId:(NSString *)sticonId
                          altText:(NSString *)altText
{
  SticonModel *run = [[SticonModel alloc] initWithPackageId:packageId
                                                   sticonId:sticonId
                                                    altText:altText];
  NSString *cur = [[self field] text] ? [[self field] text] : @"";
  NSRange sel = [self clampedSelectionForLength:[cur length]];

  NSUInteger ordinal  = [self sticonCountInString:cur range:NSMakeRange(0, sel.location)];
  NSUInteger replaced = [self sticonCountInString:cur range:sel];
  NSUInteger k;
  for (k = 0; k < replaced && ordinal < [[self sticonRuns] count]; k++)
    [[self sticonRuns] removeObjectAtIndex:ordinal];
  if (ordinal <= [[self sticonRuns] count])
    [[self sticonRuns] insertObject:run atIndex:ordinal];
  else
    [[self sticonRuns] addObject:run];

  unichar m = kSticonMarker;
  NSString *marker = [NSString stringWithCharacters:&m length:1];
  [[self field] setText:[cur stringByReplacingCharactersInRange:sel withString:marker]];
  [[self field] setSelectedRange:NSMakeRange(sel.location + 1, 0)];
  /* Setting .text programmatically does NOT fire the delegate, so refresh the
   * placeholder / send-enabled / expansion state by hand. */
  [self textViewDidChange:[self field]];
}

- (NSRange)clampedSelectionForLength:(NSUInteger)length
{
  NSRange sel = [[self field] selectedRange];
  if (sel.location > length) return NSMakeRange(length, 0);
  if (NSMaxRange(sel) > length) return NSMakeRange(sel.location, length - sel.location);
  return sel;
}

/* Count sticon markers in a substring range — the bridge between the visible
 * plain text (which keeps the marker character) and the side identity model. */
- (NSUInteger)sticonCountInString:(NSString *)s range:(NSRange)r
{
  unichar marker = kSticonMarker;
  NSUInteger i, count = 0;
  for (i = r.location; i < NSMaxRange(r); i++)
    if ([s characterAtIndex:i] == marker) count++;
  return count;
}

/* Walk the compose text into LINE wire form: each marker becomes its
 * sticon's "(altText)" form plus an ordered {package_id, sticon_id, alt_text}
 * resource dict (consumed in lockstep from self.sticonRuns); every other
 * character passes through literally. Mirrors the macOS -performSend: walk,
 * but the identity source is the side model — never the lossy text storage, so
 * the raw marker codepoint never escapes onto the wire. */
- (NSString *)wireTextIntoResources:(NSMutableArray *)resources
{
  NSString *str = [[self field] text];
  NSMutableString *out = [NSMutableString stringWithCapacity:[str length]];
  unichar marker = kSticonMarker;
  NSUInteger i, n = [str length], ordinal = 0;
  for (i = 0; i < n; i++) {
    unichar c = [str characterAtIndex:i];
    if (c == marker) {
      if (ordinal < [[self sticonRuns] count])
        [self appendMarkerForRun:[[self sticonRuns] objectAtIndex:ordinal]
                          toText:out resources:resources];
      ordinal++;  /* consume in lockstep; an unmodelled glyph is dropped, not emitted raw */
    } else {
      [out appendString:[NSString stringWithCharacters:&c length:1]];
    }
  }
  return out;
}

- (void)appendMarkerForRun:(SticonModel *)run
                    toText:(NSMutableString *)out
                 resources:(NSMutableArray *)resources
{
  NSString *alt = ([[run altText] length] ? [run altText] : [run sticonId]);
  [out appendFormat:@"(%@)", alt];
  [resources addObject:[NSDictionary dictionaryWithObjectsAndKeys:
    [run packageId], @"package_id",
    [run sticonId],  @"sticon_id",
    alt,           @"alt_text", nil]];
}

#pragma mark - UITextViewDelegate

/* Keep the side identity model aligned with keyboard edits. Programmatic
 * sticon inserts go through -insertSticonWithPackageId:… (this delegate does
 * NOT fire for them); here we handle the inverse — a user edit that deletes or
 * replaces a range containing one or more markers drops the matching runs, so
 * the k-th marker in the text always maps to sticonRuns[k]. Replacement text
 * from the keyboard never contains a marker codepoint, so nothing is added.
 * Always returns YES — Return still inserts a newline (multi-line compose);
 * sending stays on the Send button, the standard iOS multi-line pattern. */
- (BOOL)textView:(UITextView *)textView
    shouldChangeTextInRange:(NSRange)range
            replacementText:(NSString *)text
{
  (void)text;
  NSString *s = [textView text];
  NSUInteger ordinal = [self sticonCountInString:s range:NSMakeRange(0, range.location)];
  NSUInteger deleted = [self sticonCountInString:s range:range];
  NSUInteger k;
  for (k = 0; k < deleted && ordinal < [[self sticonRuns] count]; k++)
    [[self sticonRuns] removeObjectAtIndex:ordinal];
  return YES;
}

- (void)textViewDidChange:(UITextView *)textView
{
  (void)textView;
  [self updatePlaceholderVisibility];
  [[self sendButton] setEnabled:([[self trimmedText] length] > 0)];
  [self updateExpansion];
}

@end
