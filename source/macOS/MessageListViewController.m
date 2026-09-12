#import "MessageListViewController.h"

@interface MessageListViewController ()
- (void)messageJS:(NSNotification *)note;
- (void)syncDidFinish:(NSNotification *)note;
@end

@implementation MessageListViewController

- (id)initWithEngine:(ENILAccount *)engine;
{
  if (!engine) {
    [self release];
    [NSException raise:NSInvalidArgumentException
                format:@"[MessageListViewController.init] engine is required"];
    return nil;
  }
  /* baseURL is the per-account directory (trailing slash so a relative
   * <img src="chats/foo.png"> inside the rendered HTML doesn't resolve up
   * one level). The base class hands this to WKWebView as the read-access
   * scope for -loadFileURL:allowingReadAccessToURL:, which is what lets
   * the page fetch sticker / media images from sibling subdirectories
   * under enilDir. */
  NSURL *baseURL = [NSURL fileURLWithPath:
    [[engine enilDir] stringByAppendingString:@"/"]];
  if ((self = [super initWithBaseURL:baseURL])) {
    engine_         = [engine retain];
    sendController_ = [[MessageSendViewController alloc] initWithEngine:engine];
    [sendController_ setDelegate:self];
    /* The send bar paints its own background; the WebView sits flush against
     * the bar's top edge via -setAdditionalContentInsetBottom: in
     * -viewDidLayout, so neither view needs to draw under the other. */
    [self setDrawsBackground:NO];
  }
  return self;
}

- (NSString *)chatId; { return chatId_; }

/* Lifecycle:
 *   -loadView       inherited from AIWebViewController — creates the root
 *                   container.
 *   -viewDidLoad    chain super's WebView setup, then add the send bar as
 *                   a sibling child VC; register the chat-content
 *                   notifications and kick off the first render.
 *   -viewDidLayout  size the bar at the bottom of the container and the
 *                   WebView in the remaining area above it. */

- (void)viewDidLoad;
{
  [super viewDidLoad];

  [self AI_addChildViewController:sendController_];
  NSView *bar = [sendController_ view];
  [bar setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
  /* Sibling layout: the WebView (added by super) occupies bounds minus the
   * bar's height; the bar fills the bottom strip. Their frames are pinned
   * each layout pass below. */
  [[self view] addSubview:bar];
  [sendController_ setEnabled:(chatId_ && [chatId_ length])];

  /* Sync-finished re-renders the page (new messages may have landed);
   * ENILMessageJSNotification carries an incremental JS update that we
   * push into the WebView when the notification's chat_id matches ours. */
  [[NSNotificationCenter defaultCenter]
    addObserver:self selector:@selector(syncDidFinish:)
            name:ENILSyncDidFinishNotification object:nil];
  [[NSNotificationCenter defaultCenter]
    addObserver:self selector:@selector(messageJS:)
            name:ENILMessageJSNotification object:nil];
  [self reloadContent];
}

- (void)viewDidLayout;
{
  if (sendController_) {
    NSRect bounds = [[self view] bounds];
    CGFloat w = bounds.size.width;
    CGFloat h = bounds.size.height;
    CGFloat barHeight = [sendController_ preferredHeight];
    CGFloat webHeight = h - barHeight;
    if (webHeight < 0.0) webHeight = 0.0;
    [[sendController_ view] setFrame:NSMakeRect(0, 0, w, barHeight)];
    /* Pin the WebView above the bar. The base class set autoresizing to
     * widthSizable | heightSizable, so this fixed bottom margin survives
     * subsequent container resizes between layout passes. */
    [(NSView *)[self webView] setFrame:NSMakeRect(0, barHeight, w, webHeight)];
  }
  [super viewDidLayout];
}

- (void)reloadWithChatId:(NSString *)chatId displayName:(NSString *)name;
{
  (void)name;
  [chatId_ autorelease];
  chatId_ = [chatId retain];
  [sendController_ setChatId:chatId];
  [sendController_ setEnabled:(chatId_ && [chatId_ length])];
  [self reloadContent];
}

- (void)reloadData;
{
  [self reloadContent];
}

- (BOOL)hasMoreMessages; { return hasMore_; }

- (void)loadMoreMessages;
{
  if (!hasMore_ || ![chatId_ length]) return;
  BOOL more = NO;
  long long newOldest = oldestTime_;
  NSString *js = [engine_ prependMessagesJSForChatId:chatId_
                                          beforeTime:oldestTime_
                                             hasMore:&more
                                       newOldestTime:&newOldest];
  if (![js length]) { hasMore_ = NO; return; }
  [self pushJavaScript:js];
  oldestTime_ = newOldest;
  hasMore_    = more;
  [[[[self view] window] toolbar] validateVisibleItems];
}

#pragma mark - AIWebViewController overrides

+ (NSArray *)handledURLSchemes;
{
  return [NSArray arrayWithObjects:@"enil-media", @"enil-action", nil];
}

/* Reset the pagination cursor and ask ENILAccount to render the current
 * chat (or the empty placeholder) into <enilDir>/chats/. The base class
 * calls this from -reloadContent and feeds the returned URL to
 * -loadFileURL:allowingReadAccessToURL:. */
- (NSURL *)contentURL;
{
  oldestTime_ = 0;
  hasMore_    = NO;
  long long oldest = 0;
  BOOL more = NO;
  NSURL *url = [engine_ writeChatPageHTMLForChatId:chatId_
                                          outOldest:&oldest
                                         outHasMore:&more];
  oldestTime_ = oldest;
  hasMore_    = more;
  [[[[self view] window] toolbar] validateVisibleItems];
  return url;
}

/* enil-action://load-more   — paginate older messages.
 * enil-media://<rel>        — open the media file at <enilDir>/<rel> in
 *                             the user's default viewer (Preview, etc.). */
- (void)handleActionURL:(NSURL *)url;
{
  NSString *scheme = [url scheme];
  if ([scheme isEqualToString:@"enil-action"]) {
    if ([[url host] isEqualToString:@"load-more"]) [self loadMoreMessages];
    return;
  }
  if ([scheme isEqualToString:@"enil-media"]) {
    NSString *rel = [[url host] stringByAppendingString:[url path]];
    if (![rel length]) return;
    NSString *full = [[engine_ enilDir] stringByAppendingPathComponent:rel];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:full]];
    return;
  }
}

#pragma mark - Send passthrough

- (void)insertSticonWithPackageId:(NSString *)packageId
                         sticonId:(NSString *)sticonId
                          altText:(NSString *)altText;
{
  [sendController_ insertSticonWithPackageId:packageId
                                    sticonId:sticonId
                                     altText:altText];
}

- (BOOL)canSendCurrentMessage;
{
  return [sendController_ canSendCurrentMessage];
}

- (void)sendCurrentMessage;
{
  [sendController_ performSend:self];
}

- (void)presentImagePicker;
{
  [sendController_ presentImagePicker];
}

#pragma mark - Notifications

- (void)messageJS:(NSNotification *)note;
{
  NSDictionary *info = [note userInfo];
  NSString *chatId = [info objectForKey:@"chat_id"];
  NSString *js     = [info objectForKey:@"js"];
  if (![chatId isEqualToString:chatId_] || ![js length]) return;
  [self pushJavaScript:js];
}

- (void)syncDidFinish:(NSNotification *)note;
{
  (void)note;
  [self reloadContent];
}

#pragma mark - MessageSendDelegate

/* The send VC's expansion changes its preferred height — re-run the same
 * layout pass that -viewDidLayout owns, since the geometry math is identical.
 * Calling -viewDidLayout directly (rather than -setNeedsLayout:, which is
 * 10.10+) keeps this synchronous and Tiger-callable. */
- (void)messageSendViewControllerDidChangeHeight:
    (MessageSendViewController *)vc;
{
  (void)vc;
  if (![[sendController_ view] superview]) return;
  [self viewDidLayout];
  [[sendController_ view] setNeedsDisplay:YES];
  [[self view] setNeedsDisplay:YES];
}

- (void)dealloc;
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [chatId_ release];
  [engine_ release];
  [sendController_ release];
  [super dealloc];
}

@end
