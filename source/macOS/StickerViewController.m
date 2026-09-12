#import "StickerViewController.h"
#import "ENILResponderActions.h"
#import "ENILUserDefaults.h"
#import "XPFoundation.h"

@implementation StickerViewController

- (id)initWithEngine:(ENILAccount *)engine;
{
  if (!engine) {
    [self release];
    [NSException raise:NSInvalidArgumentException
                format:@"[StickerViewController.init] engine is required"];
    return nil;
  }
  /* baseURL is needed at init time (AIWebViewController stores it for
   * later loadHTMLString:baseURL: calls). The trailing slash matters so
   * relative file:// paths inside the picker HTML — sticker images under
   * Application Support/ENIL/<accountId>/media/ — resolve against the
   * account directory and not its parent. */
  NSURL *baseURL = [NSURL fileURLWithPath:
    [[engine enilDir] stringByAppendingString:@"/"]];
  if ((self = [super initWithBaseURL:baseURL])) {
    engine_        = [engine retain];
    mode_          = StickerPickerModeNone;  /* never-loaded until a tab is picked */
    dirty_         = YES;
    /* Inspector pane — let whatever the host window puts behind us show
     * through (vibrancy on Modern, sheet/sidebar tint on Middle). */
    [self setDrawsBackground:NO];
  }
  return self;
}

+ (NSArray *)handledURLSchemes;
{
  return [NSArray arrayWithObjects:@"enil-sticker", @"enil-sticon", nil];
}

/* Pull model: the base class calls this from -reloadContent. The engine
 * renders the active tab's HTML and writes it to a stable file inside
 * enilDir (sticker-picker.html or sticon-picker.html); we return the
 * file:// URL. Sticons use a 48 px cell + 42 px thumb compact grid;
 * stickers derive their cell size to keep sticker_footprint = 2 ×
 * sticon_footprint so the column rhythm survives a tab switch. */
- (NSURL *)contentURL;
{
  /* Never-loaded state: render nothing so no default tab can flash through the
   * inspector when it is first revealed. reloadContent bails on a nil URL,
   * leaving the WebView blank until the user picks Stickers or Emoji. */
  if (mode_ == StickerPickerModeNone) return nil;

  {
    BOOL sticon = (mode_ == StickerPickerModeSticon);
    /* Per-cell rendered footprint = cellSize + 4 (border) + 8 (margin LR);
     * see ENILUserDefaults.m pickerCSSWithItemSize:. To enforce 2× ratio:
     *   stickerCell + 12 = 2 × (sticonCell + 12)  =>  stickerCell = 2·sticonCell + 12 */
    NSInteger sticonCell   = 48;
    NSInteger stickerCell  = 2 * sticonCell + 12;   /* 108 */
    NSInteger sticonThumb  = 42;
    NSInteger stickerThumb = stickerCell - 8;       /* 100 */
    NSInteger cellSize  = sticon ? sticonCell  : stickerCell;
    NSInteger thumbSize = sticon ? sticonThumb : stickerThumb;
    NSString *css = [ENILUserDefaults pickerCSSWithItemSize:cellSize];
    NSURL *url = [engine_ writeStickerPickerHTMLForSticonMode:sticon
                                                          css:css
                                                    thumbSize:(int)thumbSize];
    ENILLog(@"StickerViewController.contentURL", @"%@ -> %@",
          sticon ? @"sticon" : @"sticker", [url path]);
    return url;
  }
}

- (void)viewWillAppear;
{
  [super viewWillAppear];
  [self reloadActiveTabIfDirty];
}

- (void)reloadActiveTabIfDirty;
{
  if (!dirty_) return;
  [self reloadContent];
  dirty_ = NO;
}

- (void)reloadData;
{
  dirty_ = YES;
}

- (BOOL)isSticonMode;
{
  return mode_ == StickerPickerModeSticon;
}

- (BOOL)hasSelectedMode;
{
  return mode_ != StickerPickerModeNone;
}

- (void)setSticonMode:(BOOL)sticon;
{
  StickerPickerMode want = sticon ? StickerPickerModeSticon
                                  : StickerPickerModeSticker;
  if (mode_ == want) return;
  mode_  = want;
  dirty_ = YES;
  [self reloadActiveTabIfDirty];
}

- (void)setDelegate:(id<StickerViewControllerDelegate>)delegate;
{
  delegate_ = delegate;
}

static void parseQuery(NSString *query, NSString **packageId, NSString **stickerId,
                       NSString **altText) {
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

- (void)handleActionURL:(NSURL *)url;
{
  NSString *scheme    = [url scheme];
  NSString *query     = [url query];
  NSString *packageId = nil, *stickerId = nil, *altText = nil;
  BOOL isSticon = [scheme isEqualToString:@"enil-sticon"];

  if (query) parseQuery(query, &packageId, &stickerId, &altText);
  if (!packageId || !stickerId) return;

  NSMutableDictionary *payload = [NSMutableDictionary dictionary];
  [payload setObject:packageId forKey:ENILResponderPayloadPackageIdKey];
  [payload setObject:stickerId forKey:ENILResponderPayloadStickerIdKey];
  [payload setObject:[NSNumber numberWithBool:isSticon]
              forKey:ENILResponderPayloadIsSticonKey];
  if (altText && [altText length])
    [payload setObject:altText forKey:ENILResponderPayloadAltTextKey];
  if ([[self nextResponder] tryToPerform:@selector(enilPickSticker:)
                                    with:payload]) return;
  if (delegate_)
    [delegate_ stickerViewController:self
                      didSelectPackageId:packageId
                               stickerId:stickerId
                                isSticon:isSticon];
}

- (void)dealloc;
{
  [engine_ release];
  [super dealloc];
}

@end
