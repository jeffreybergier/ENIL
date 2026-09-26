//
//  StickerViewController.m
//  ENIL
//

#import "StickerViewController.h"
#import "ENILAccount.h"
#import "ENILUserDefaults.h"
#import "XPFoundation.h"
#import "AIFontAwesome.h"
#import "XPUIKit.h"  /* XP_scrollView / XP_setBackgroundTransparent / XP_removeShadow */

/* Picker grid geometry — matches the macOS inspector. Stickers use the 108 px
 * cell / 100 px thumb pair; sticons the compact 48/42 pair. */
static const NSInteger kStickerCell  = 108;
static const NSInteger kStickerThumb = 100;
static const NSInteger kSticonCell   = 48;
static const NSInteger kSticonThumb  = 42;

/* Tab-bar glyph: ink sized near Apple's ~25-30pt tab icons, centered in a
 * slightly larger square so the rasteriser's antialiased edges don't clip. */
static const CGFloat kTabIconPt   = 24.0;
static const CGFloat kTabCanvasPt = 30.0;

#pragma mark - StickerWebViewController (private leaf)

@class StickerWebViewController;

/* One tab's web view. A single UIWebView that renders exactly ONE mode
 * (stickers OR sticons), loads once, and stays resident — so the container
 * (StickerViewController, below) switches tabs by show/hide with no reload and
 * no lost scroll. Reports taps + Done up to webDelegate; the container re-emits
 * to the host's StickerPickerDelegate.
 *
 * Private to this file: only StickerViewController instantiates it, and the
 * host never sees it. webDelegate is `assign` (unsafe_unretained under ARC) per
 * the 4.3-floor rule — zeroing __weak is unavailable; the container nils it. */
@protocol StickerWebDelegate <NSObject>
- (void)stickerWeb:(StickerWebViewController *)web
  didPickPackageId:(NSString *)packageId
         stickerId:(NSString *)stickerId
           altText:(NSString *)altText
          isSticon:(BOOL)isSticon;
- (void)stickerWebDidTapDone:(StickerWebViewController *)web;
@end

@interface StickerWebViewController : UIViewController
- (instancetype)initWithEngine:(ENILAccount *)engine sticonMode:(BOOL)sticonMode;
@property (nonatomic, assign) id<StickerWebDelegate> webDelegate;
@end

@interface StickerWebViewController () <UIWebViewDelegate>
@property (nonatomic, strong) ENILAccount *engine;
@property (nonatomic, strong) UIWebView *webView;
@property (nonatomic, assign) BOOL sticonMode;  /* fixed for this page's lifetime */
/* Re-render this page's HTML and reload its web view. Called once from
 * viewDidLoad, and again by the container when the catalog changes. */
- (void)reloadContent;
@end

@implementation StickerWebViewController

- (instancetype)initWithEngine:(ENILAccount *)engine sticonMode:(BOOL)sticonMode
{
  if (engine == nil) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException
                                  reason:@"engine is required"
                                userInfo:nil];
  }
  if ((self = [super initWithNibName:nil bundle:nil])) {
    _engine     = engine;
    _sticonMode = sticonMode;
    /* Title drives the nav bar; the explicit tabBarItem drives the tab. Icons
     * mirror the macOS inspector's picker-mode segments: a certificate ribbon
     * for stickers, a grinning face for sticons (macOS's "Emoji" tab). */
    NSString *title = sticonMode ? NSLocalizedString(@"Emoji", nil)
                                 : NSLocalizedString(@"Stickers", nil);
    [self setTitle:title];
    AIFontAwesomeIcon icon = sticonMode ? AIFAFaceGrin : AIFACertificate;
    AIFontAwesomeStyle style = sticonMode
      ? AIFontAwesomeStyleRegular : AIFontAwesomeStyleSolid;
    [self setTabBarItem:[[UITabBarItem alloc]
      initWithTitle:title
              image:[self tabImageForIcon:icon style:style]
                tag:(sticonMode ? 1 : 0)]];
  }
  return self;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  [[self view] setBackgroundColor:[UIColor messagesBackgroundColor]];

  /* UIBarButtonSystemItemDone is 2.0-era — safe on the 4.3 floor. There is
   * nothing to discard, so it routes back through the cancel delegate. */
  [[self navigationItem] setRightBarButtonItem:[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                  target:self
                                                  action:@selector(doneTapped)]];

  [self buildWebView];
  [self reloadContent];  /* once: viewDidLoad fires the first time this tab is shown */
}

/* Web view fills the page's view — the nav bar and tab bar belong to the
 * container controllers, which size self.view to the gap between them.
 * UIWebView grants the file:// picker page a real file origin, so its <img src>
 * paths under media/purchases resolve directly. */
- (void)buildWebView
{
  UIWebView *web = [[UIWebView alloc] initWithFrame:[[self view] bounds]];
  [web setAutoresizingMask:UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight];
  [web setDelegate:self];
  /* Coast like a UITableView (iOS 5+; no-op on the 4.3 floor — nil scrollView). */
  [[web XP_scrollView] setDecelerationRate:UIScrollViewDecelerationRateNormal];
  /* Composite through to the host view (drawsBackground equivalent) so the
   * overscroll bounce shows the picker's background, not UIWebView's white. */
  [web XP_setBackgroundTransparent];
  [[self view] addSubview:web];
  [self setWebView:web];
}

/* Black-on-transparent FA glyph at the tab-bar size; UIKit tints it for the
 * selected state. scale 0 auto-resolves to the screen scale. */
- (UIImage *)tabImageForIcon:(AIFontAwesomeIcon)icon
                       style:(AIFontAwesomeStyle)style
{
  return [AIFontAwesome imageForIcon:icon
                            style:style
                         iconSize:kTabIconPt
                       canvasSize:kTabCanvasPt
                            scale:0.0];
}

/* Pull model, same as macOS -contentURL: the engine renders this page's HTML,
 * writes it under enilDir, and returns the file:// URL. Renders once for the
 * page's fixed mode — switching tabs never re-enters this. */
- (void)reloadContent
{
  @try {
    NSInteger cell  = [self sticonMode] ? kSticonCell  : kStickerCell;
    NSInteger thumb = [self sticonMode] ? kSticonThumb : kStickerThumb;
    NSString *css = [ENILUserDefaults pickerCSSWithItemSize:cell];
    NSURL *url = [[self engine] writeStickerPickerHTMLForSticonMode:[self sticonMode]
                                                              css:css
                                                        thumbSize:(int)thumb];
    if (url == nil) {
      ENILLog(@"StickerWebViewController.reloadContent", @"nil picker URL");
      return;
    }
    [[self webView] loadRequest:[NSURLRequest requestWithURL:url]];
  } @catch (NSException *exception) {
    ENILLog(@"StickerWebViewController.reloadContent", @"exception: %@", exception);
  }
}

- (void)doneTapped
{
  [[self webDelegate] stickerWebDidTapDone:self];
}

/* Parse package_id / sticker_id / alt_text out of an enil-sticker:// or
 * enil-sticon:// query, mirroring the macOS parseQuery. Both schemes carry the
 * id under `sticker_id`; only sticons add `alt_text`.
 * XP_stringByRemovingPercentEncoding is the 4.3-safe shim
 * (stringByRemovingPercentEncoding is iOS 7.0). */
static void parseQuery(NSString *query, NSString **packageId, NSString **stickerId,
                       NSString **altText)
{
  NSArray *pairs = [query componentsSeparatedByString:@"&"];
  NSUInteger i;
  for (i = 0; i < [pairs count]; i++) {
    NSString *pair = [pairs objectAtIndex:i];
    NSRange r = [pair rangeOfString:@"="];
    if (r.location == NSNotFound) continue;
    NSString *key = [pair substringToIndex:r.location];
    NSString *val = [[pair substringFromIndex:r.location + 1]
      XP_stringByRemovingPercentEncoding];
    if ([key isEqualToString:@"package_id"]) *packageId = val;
    else if ([key isEqualToString:@"sticker_id"]) *stickerId = val;
    else if ([key isEqualToString:@"alt_text"]) *altText = val;
  }
}

/* Report a tapped cell to the delegate. Empty identity is dropped silently. */
- (void)dispatchPickURL:(NSURL *)url sticon:(BOOL)isSticon
{
  NSString *packageId = nil, *stickerId = nil, *altText = nil;
  NSString *query = [url query];
  if (query) parseQuery(query, &packageId, &stickerId, &altText);
  if (![packageId length] || ![stickerId length]) return;
  [[self webDelegate] stickerWeb:self
              didPickPackageId:packageId
                     stickerId:stickerId
                       altText:altText
                      isSticon:isSticon];
}

#pragma mark UIWebViewDelegate

/* Intercept the picker's enil-sticker:// / enil-sticon:// cell taps; let every
 * other navigation through. */
- (BOOL)webView:(UIWebView *)webView
    shouldStartLoadWithRequest:(NSURLRequest *)request
                navigationType:(UIWebViewNavigationType)navigationType
{
  (void)webView; (void)navigationType;
  NSString *scheme = [[request URL] scheme];
  if ([scheme isEqualToString:@"enil-sticker"]) {
    [self dispatchPickURL:[request URL] sticon:NO];
    return NO;
  }
  if ([scheme isEqualToString:@"enil-sticon"]) {
    [self dispatchPickURL:[request URL] sticon:YES];
    return NO;
  }
  return YES;
}

- (void)webViewDidFinishLoad:(UIWebView *)webView
{
  (void)webView;
  [[[self webView] XP_scrollView] XP_removeShadow];
}

- (void)dealloc
{
  [_webView setDelegate:nil];
}

@end

#pragma mark - StickerViewController (public container)

@interface StickerViewController () <StickerWebDelegate>
@property (nonatomic, strong) ENILAccount *engine;
@property (nonatomic, strong) StickerWebViewController *stickersLeaf;
@property (nonatomic, strong) StickerWebViewController *sticonsLeaf;
@property (nonatomic, assign) BOOL needsReload;  /* catalog changed while off screen */
@end

@implementation StickerViewController

- (instancetype)initWithEngine:(ENILAccount *)engine
{
  if (engine == nil) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException
                                  reason:@"engine is required"
                                userInfo:nil];
  }
  if ((self = [super initWithNibName:nil bundle:nil])) {
    _engine = engine;
    [self buildTabs];
  }
  return self;
}

/* Two resident tabs: Stickers (left) | Emoji/sticons (right). Each leaf owns
 * its own web view and is wrapped in a UINavigationController for the title +
 * Done button. The tab bar reads each tab from the navigation controller's
 * tabBarItem, so copy the leaf's item (FA glyph + title) onto its wrapper
 * rather than relying on child forwarding. */
- (void)buildTabs
{
  StickerWebViewController *stickers =
    [[StickerWebViewController alloc] initWithEngine:[self engine] sticonMode:NO];
  [stickers setWebDelegate:self];
  StickerWebViewController *sticons =
    [[StickerWebViewController alloc] initWithEngine:[self engine] sticonMode:YES];
  [sticons setWebDelegate:self];
  /* Held so -setNeedsReload can re-render them; the tab bar's own retain via
   * viewControllers is on the wrapping nav controllers, not the leaves. */
  [self setStickersLeaf:stickers];
  [self setSticonsLeaf:sticons];

  UINavigationController *stickersNav =
    [[UINavigationController alloc] initWithRootViewController:stickers];
  [stickersNav setTabBarItem:[stickers tabBarItem]];
  UINavigationController *sticonsNav =
    [[UINavigationController alloc] initWithRootViewController:sticons];
  [sticonsNav setTabBarItem:[sticons tabBarItem]];

  [self setViewControllers:[NSArray arrayWithObjects:stickersNav, sticonsNav, nil]];
}

#pragma mark Reload

/* Re-render only the tabs whose web view already exists. A never-selected tab's
 * view is unloaded (UITabBarController loads tab views lazily); it renders fresh
 * from its own viewDidLoad on first appearance, so reloading it here would be
 * wasted work — and would message a nil web view. */
- (void)reloadLoadedLeaves
{
  if ([[self stickersLeaf] isViewLoaded]) [[self stickersLeaf] reloadContent];
  if ([[self sticonsLeaf]  isViewLoaded]) [[self sticonsLeaf]  reloadContent];
}

- (void)setNeedsReload
{
  if ([self isViewLoaded] && [[self view] window]) [self reloadLoadedLeaves];
  else [self setNeedsReload:YES];
}

- (void)viewWillAppear:(BOOL)animated
{
  [super viewWillAppear:animated];
  if ([self needsReload]) {
    [self setNeedsReload:NO];
    [self reloadLoadedLeaves];
  }
}

#pragma mark StickerWebDelegate

/* Re-emit a leaf's pick to the host, passing self as the picker so the host's
 * StickerPickerDelegate contract is unchanged. Stickers send one-shot; sticons
 * inline into the compose draft. */
- (void)stickerWeb:(StickerWebViewController *)web
  didPickPackageId:(NSString *)packageId
         stickerId:(NSString *)stickerId
           altText:(NSString *)altText
          isSticon:(BOOL)isSticon
{
  (void)web;
  if (isSticon)
    [[self pickerDelegate] stickerPicker:self
                didPickSticonPackageId:packageId
                              sticonId:stickerId
                               altText:altText];
  else
    [[self pickerDelegate] stickerPicker:self
                      didPickPackageId:packageId
                             stickerId:stickerId];
}

- (void)stickerWebDidTapDone:(StickerWebViewController *)web
{
  (void)web;
  [[self pickerDelegate] stickerPickerDidCancel:self];
}

@end
