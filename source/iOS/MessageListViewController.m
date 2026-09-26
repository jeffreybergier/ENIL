//
//  MessageListViewController.m
//  ENIL
//

#import "MessageListViewController.h"
#import "MediaViewerViewController.h"
#import "MessageSendViewController.h"
#import "StickerViewController.h"
#import "ENILAccount.h"
#import "XPFoundation.h"
#import "AIFontAwesome.h"
#import "UIViewController+ENILModal.h"  /* 4.3-floor present/dismiss shim */
#import "XPUIKit.h"                      /* XP_setScrollDecelerationNormal */

static const CGFloat kComposeBarHeight = 44.0;

@interface MessageListViewController ()
    <UIWebViewDelegate, MessageSendDelegate, StickerPickerDelegate,
     UIImagePickerControllerDelegate, UINavigationControllerDelegate>
@property (nonatomic, strong) ENILAccount *account;
@property (nonatomic, copy)   NSString *chatId;
@property (nonatomic, copy)   NSString *displayName;
@property (nonatomic, strong) UIWebView *webView;
@property (nonatomic, strong) MessageSendViewController *sendBar;
@property (nonatomic, strong) StickerViewController *stickerPicker; /* account-shared */
@property (nonatomic, strong) UIBarButtonItem *markSeenButton;
@property (nonatomic, copy)   NSString *pendingMarkSeenChatId; /* in-memory
                                            disabled state while awaiting
                                            SSE confirmation */
@property (nonatomic, assign) long long oldestTime;  /* pagination cursor */
@property (nonatomic, assign) BOOL hasMore;
/* Compose-bar docking state. composeBarBottomY is the bar's pinned bottom edge
 * (the keyboard top while composing, else the view bottom); composing tracks
 * the keyboard-up state. Both feed -relayoutComposeBar… so a height change from
 * the bar can re-dock it without re-deriving the keyboard frame. */
@property (nonatomic, assign) CGFloat composeBarBottomY;
@property (nonatomic, assign) BOOL composing;
@end

@implementation MessageListViewController

- (instancetype)initWithAccount:(ENILAccount *)account
                         chatId:(NSString *)chatId
                    displayName:(NSString *)displayName
                  stickerPicker:(StickerViewController *)stickerPicker
{
  if (account == nil || [chatId length] == 0) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException
                                  reason:@"account nil or chatId empty"
                                userInfo:nil];
  }
  if ((self = [super initWithNibName:nil bundle:nil])) {
    _account = account;
    _chatId = [chatId copy];
    _displayName = [displayName copy];
    _stickerPicker = stickerPicker;  /* shared; owned by the coordinator */
  }
  return self;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  [[self navigationItem] setTitle:[[self displayName] length] ? [self displayName] : [self chatId]];
  [[self navigationItem] setRightBarButtonItem:[self makeMarkSeenBarButton]];
  [[self view] setBackgroundColor:[UIColor messagesBackgroundColor]];

  /* Springs-and-struts split: the web view fills everything above the bar; the
   * bar is a bottom-docked strip. Both frames are recomputed on keyboard
   * show/hide and on bar height changes (see -relayoutComposeBarAnimated:…). */
  CGRect b = [[self view] bounds];
  CGFloat barTop = b.size.height - kComposeBarHeight;

  UIWebView *web =
    [[UIWebView alloc] initWithFrame:CGRectMake(0, 0, b.size.width, barTop)];
  [web setAutoresizingMask:UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight];
  [web setDelegate:self];
  /* Coast like a UITableView (iOS 5+; no-op on the 4.3 floor — nil scrollView). */
  [[web XP_scrollView] setDecelerationRate:UIScrollViewDecelerationRateNormal];
  /* Composite through to the host view (the macOS WKWebView's drawsBackground
   * equivalent); UIWebView ships opaque + white otherwise. */
  [web XP_setBackgroundTransparent];
  [[self view] addSubview:web];
  [self setWebView:web];

  MessageSendViewController *bar = [[MessageSendViewController alloc]
    initWithFrame:CGRectMake(0, barTop, b.size.width, kComposeBarHeight)];
  [bar setDelegate:self];
  [[self view] addSubview:bar];
  [self setSendBar:bar];

  /* Bar rests at the view bottom, keyboard down, until the first keyboard
   * notification flips composing on and re-docks it above the keyboard. */
  [self setComposeBarBottomY:b.size.height];
  [self setComposing:NO];

  [self observeEngineNotifications];
  [self observeKeyboard];
  [self reloadContent];
  [self updateMarkSeenEnabled];
}

- (void)viewWillDisappear:(BOOL)animated
{
  [super viewWillDisappear:animated];
  [[self view] endEditing:YES];  /* drop the keyboard before the pop */
}

- (void)observeEngineNotifications
{
  NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
  [nc addObserver:self
         selector:@selector(messageJS:)
             name:ENILMessageJSNotification
           object:nil];
  [nc addObserver:self
         selector:@selector(syncDidFinish:)
             name:ENILSyncDidFinishNotification
           object:nil];
}

#pragma mark - Keyboard tracking

/* The 4.3 floor has no inputAccessoryView auto-docking (iOS 8.0), so the bar
 * is a bottom subview whose frame follows the keyboard. */
- (void)observeKeyboard
{
  NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
  [nc addObserver:self
         selector:@selector(keyboardWillShow:)
             name:UIKeyboardWillShowNotification
           object:nil];
  [nc addObserver:self
         selector:@selector(keyboardWillHide:)
             name:UIKeyboardWillHideNotification
           object:nil];
}

/* Re-dock the bar at its current -preferredHeight: pin its bottom to
 * composeBarBottomY, shrink the web view to match, and re-apply the compose
 * state (chevron + field width) — all inside one begin/commit animation so the
 * pieces move together. The bar's bottom stays put (above the keyboard, or at
 * the view bottom); only its top + the web view's height move. Driven by the
 * keyboard notifications and by -messageSendDidChangeHeight:.
 *
 * begin/commit rather than -animateWithDuration:delay:options: because the
 * keyboard notification hands us a UIViewAnimationCurve (-setAnimationCurve:
 * takes it directly), not the UIViewAnimationOptions bitmask the block API
 * expects. */
- (void)relayoutComposeBarAnimated:(BOOL)animated
                             curve:(UIViewAnimationCurve)curve
                          duration:(NSTimeInterval)dur
{
  CGFloat w = [[self view] bounds].size.width;
  /* Set composing first — it only records state + marks layout dirty, so
   * -preferredHeight below already reflects it and the bar collapses the moment
   * composing ends. The fade + frame changes are applied by -layoutIfNeeded
   * inside the animation block, so they ride the keyboard's own timing. */
  [[self sendBar] setComposing:[self composing]];
  CGFloat h = [[self sendBar] preferredHeight];
  CGFloat barTop = [self composeBarBottomY] - h;
  if (animated && dur > 0.0) {
    [UIView beginAnimations:nil context:NULL];
    [UIView setAnimationDuration:dur];
    [UIView setAnimationCurve:curve];
  }
  [[self sendBar] setFrame:CGRectMake(0, barTop, w, h)];
  [[self webView] setFrame:CGRectMake(0, 0, w, barTop)];
  [[self sendBar] layoutIfNeeded];
  if (animated && dur > 0.0) [UIView commitAnimations];
}

/* The keyboard notification carries its own animation timing; honour both so
 * our bar moves in lockstep with the system keyboard. The curve is a
 * UIViewAnimationCurve enum value (sometimes a private one), not options. */
- (NSTimeInterval)keyboardDuration:(NSDictionary *)info
{
  return [[info objectForKey:UIKeyboardAnimationDurationUserInfoKey] doubleValue];
}

- (UIViewAnimationCurve)keyboardCurve:(NSDictionary *)info
{
  return (UIViewAnimationCurve)
    [[info objectForKey:UIKeyboardAnimationCurveUserInfoKey] integerValue];
}

- (void)keyboardWillShow:(NSNotification *)note
{
  NSDictionary *info = [note userInfo];
  /* Convert the screen-space keyboard rect into view coords so the math stays
   * correct across rotation. */
  CGRect end = [[info objectForKey:UIKeyboardFrameEndUserInfoKey] CGRectValue];
  CGRect kb = [[self view] convertRect:end fromView:nil];
  CGFloat bottom = kb.origin.y;
  CGFloat maxBottom = [[self view] bounds].size.height;
  if (bottom > maxBottom) bottom = maxBottom;
  [self setComposeBarBottomY:bottom];
  [self setComposing:YES];
  [self relayoutComposeBarAnimated:YES
                             curve:[self keyboardCurve:info]
                          duration:[self keyboardDuration:info]];
  [self scrollWebViewToBottom];
}

- (void)keyboardWillHide:(NSNotification *)note
{
  NSDictionary *info = [note userInfo];
  [self setComposeBarBottomY:[[self view] bounds].size.height];
  [self setComposing:NO];
  [self relayoutComposeBarAnimated:YES
                             curve:[self keyboardCurve:info]
                          duration:[self keyboardDuration:info]];
}

- (void)scrollWebViewToBottom
{
  [[self webView] stringByEvaluatingJavaScriptFromString:
    @"window.scrollTo(0, document.body.scrollHeight);"];
}

#pragma mark - Rendering

/* Renders chats/<chatId>.html via the shared engine and loads the file URL.
 * UIWebView grants a file:// page a real file origin, so sibling media/sticker
 * resources resolve without the macOS WKWebView loadFileURL: dance. */
- (void)reloadContent
{
  @try {
    long long oldest = 0;
    BOOL more = NO;
    NSURL *url = [[self account] writeChatPageHTMLForChatId:[self chatId]
                                               outOldest:&oldest
                                              outHasMore:&more];
    if (url == nil) {
      ENILLog(@"MessageListViewController.reloadContent",
              @"nil URL for chat %@", [self chatId]);
      return;
    }
    [self setOldestTime:oldest];
    [self setHasMore:more];
    [[self webView] loadRequest:[NSURLRequest requestWithURL:url]];
  } @catch (NSException *exception) {
    ENILLog(@"MessageListViewController.reloadContent",
            @"exception: %@", exception);
  }
}

/* enil-action://load-more — prepend older history as an incremental JS update,
 * advancing the pagination cursor. Mirrors macOS -loadMoreMessages. */
- (void)loadMoreMessages
{
  if (![self hasMore] || ![[self chatId] length]) return;
  @try {
    BOOL more = NO;
    long long newOldest = [self oldestTime];
    NSString *js = [[self account] prependMessagesJSForChatId:[self chatId]
                                                 beforeTime:[self oldestTime]
                                                    hasMore:&more
                                              newOldestTime:&newOldest];
    if (![js length]) { [self setHasMore:NO]; return; }
    [[self webView] stringByEvaluatingJavaScriptFromString:js];
    [self setOldestTime:newOldest];
    [self setHasMore:more];
  } @catch (NSException *exception) {
    ENILLog(@"MessageListViewController.loadMoreMessages",
            @"exception: %@", exception);
  }
}

/* enil-media://<rel> — resolve <enilDir>/<rel> and push a full-screen viewer.
 * macOS opens the file in Preview; iOS keeps it in-app on the nav stack. */
- (void)presentMediaAtRelativePath:(NSString *)rel
{
  if (![rel length]) return;
  NSString *full = [[[self account] enilDir] stringByAppendingPathComponent:rel];
  MediaViewerViewController *viewer =
    [[MediaViewerViewController alloc] initWithImagePath:full];
  [[self navigationController] pushViewController:viewer animated:YES];
}

#pragma mark - Notifications

- (void)messageJS:(NSNotification *)note
{
  NSDictionary *info = [note userInfo];
  NSString *chatId = [info objectForKey:@"chat_id"];
  NSString *js = [info objectForKey:@"js"];
  if (![chatId isEqualToString:[self chatId]] || ![js length]) return;
  [[self webView] stringByEvaluatingJavaScriptFromString:js];
  [self updateMarkSeenEnabled];
}

- (void)syncDidFinish:(NSNotification *)note
{
  (void)note;
  [self reloadContent];
  [self updateMarkSeenEnabled];
}

#pragma mark - Mark as Seen

/* White nav-bar glyph: 18pt icon centered in the same 28pt canvas as
 * ChatListViewController's Settings/Sync buttons, so all three nav glyphs match
 * in size and alignment. The 28pt canvas stays under the navigation bar's
 * button content height, so a bordered UIBarButtonItem draws it 1:1 and
 * centered rather than resizing it into its wider bezel. */
- (UIImage *)whiteBarIconForIcon:(AIFontAwesomeIcon)icon
                            style:(AIFontAwesomeStyle)style
{
  return [AIFontAwesome imageForIcon:icon
                             style:style
                          iconSize:18.0
                        canvasSize:28.0
                             color:[UIColor whiteColor]
                             scale:0.0f];  /* 0 -> [UIScreen mainScreen].scale */
}

/* The macOS peer puts this on the compose bar's actions segment. iOS surfaces
 * it as the thread's top-right nav button: a white, bordered UIBarButtonItem
 * in the same style as ChatList's Settings/Sync buttons, drawn with a plain
 * solid check (AIFACheck). The item is retained so -updateMarkSeenEnabled can
 * toggle its native enabled state (which dims the glyph). */
- (UIBarButtonItem *)makeMarkSeenBarButton
{
  UIImage *image = [self whiteBarIconForIcon:AIFACheck
                                       style:AIFontAwesomeStyleSolid];
  UIBarButtonItem *item =
    [[UIBarButtonItem alloc] initWithImage:image
                                     style:UIBarButtonItemStyleBordered
                                    target:self
                                    action:@selector(markChatSeenTapped)];
  [item setAccessibilityLabel:NSLocalizedString(@"Mark as Seen", nil)];
  [self setMarkSeenButton:item];
  return item;
}

/* Mirrors macOS -enilMarkChatSeen:: no-op when nothing is unread, otherwise
 * advance the seen marker to the server head and refresh the button. */
- (void)markChatSeenTapped
{
  if ([[self chatId] length] == 0) return;
  @try {
    if ([[self account] unreadCountForChatId:[self chatId]] <= 0) return;
    [[self account] markChatSeenAtServerHead:[self chatId]];
    /* Disable the button in memory until the SSE push confirms the seen
     * state.  We no longer write to the DB optimistically — that created a
     * split source of truth where the button read fresh DB (unreadCount==0)
     * but the chat list cached its rows and still showed the blue dot.
     * Now only the SSE path writes to the DB; this in-memory flag keeps the
     * button greyed until the confirmation arrives. */
    [self setPendingMarkSeenChatId:[self chatId]];
    [self updateMarkSeenEnabled];
  } @catch (NSException *exception) {
    ENILLog(@"MessageListViewController.markChatSeenTapped",
            @"exception: %@", exception);
  }
}

/* Grey the button out when the chat has nothing unread — the macOS actions
 * segment's -updateMarkAsSeenEnabled equivalent. */
- (void)updateMarkSeenEnabled
{
  @try {
    BOOL canMarkSeen =
      ([[self chatId] length] > 0 &&
       [[self account] unreadCountForChatId:[self chatId]] > 0);
    /* If the DB now says nothing is unread, the SSE confirmation has landed —
     * clear our in-memory pending flag. */
    if (!canMarkSeen) {
      [self setPendingMarkSeenChatId:nil];
    }
    /* In-memory pending flag: keep the button disabled even if the DB still
     * reports unread messages, because we sent the RPC and are waiting for
     * the SSE push to confirm the seen state. */
    if ([self pendingMarkSeenChatId] && [self chatId] &&
        [[self pendingMarkSeenChatId] isEqualToString:[self chatId]]) {
      [[self markSeenButton] setEnabled:NO];
    } else {
      [[self markSeenButton] setEnabled:canMarkSeen];
    }
  } @catch (NSException *exception) {
    ENILLog(@"MessageListViewController.updateMarkSeenEnabled",
            @"exception: %@", exception);
  }
}

#pragma mark - MessageSendDelegate

/* Hand the composed text to the shared engine. -sendText:toChatId: runs the
 * wire-send on a background thread and posts ENILMessageJSNotification for the
 * optimistic bubble + confirm-swap, which -messageJS: already injects — so
 * there is nothing to append here. */
- (void)messageSend:(MessageSendViewController *)bar
      didSubmitText:(NSString *)text
    sticonResources:(NSArray *)sticonResources
{
  (void)bar;
  if ([text length] == 0 || [[self chatId] length] == 0) return;
  @try {
    BOOL ok = ([sticonResources count] > 0)
      ? [[self account] sendInlineSticons:sticonResources text:text toChatId:[self chatId]]
      : [[self account] sendText:text toChatId:[self chatId]];
    if (!ok)
      ENILLog(@"MessageListViewController.messageSend",
              @"send returned NO for chat %@", [self chatId]);
  } @catch (NSException *exception) {
    ENILLog(@"MessageListViewController.messageSend",
            @"exception: %@", exception);
  }
}

/* The bar grew/shrank between single- and multi-line. Re-dock it at its new
 * -preferredHeight on a short ease curve (the keyboard isn't moving, so there
 * is no system timing to borrow), then keep the latest message in view as the
 * web view shrinks. */
- (void)messageSendDidChangeHeight:(MessageSendViewController *)bar
{
  (void)bar;
  [self relayoutComposeBarAnimated:YES
                             curve:UIViewAnimationCurveEaseInOut
                          duration:0.2];
  [self scrollWebViewToBottom];
}

/* Present the modal sticker picker. It is a UITabBarController (stickers |
 * sticons), so it is the modal root directly — each tab carries its own
 * navigation controller for the title + Done button. Stickers are one-shot:
 * a tap dismisses and sends. */
- (void)messageSendDidTapStickers:(MessageSendViewController *)bar
{
  (void)bar;
  StickerViewController *picker = [self stickerPicker];
  if (picker == nil) {
    ENILLog(@"MessageListViewController.messageSendDidTapStickers",
            @"no shared sticker picker");
    return;
  }
  /* Claim the shared picker's delegate for the duration it is presented; every
   * dismiss path nils it again so a backgrounded thread never receives a pick. */
  [picker setPickerDelegate:self];
  [self enil_presentModalViewController:picker];
}

/* Present the system photo-library picker. The chosen image is persisted and
 * routed through the engine in -imagePickerController:didFinishPickingMediaWithInfo:.
 * UIImagePickerController is the bottom-most modal here (no wrapping nav), so
 * the host presents it directly. */
- (void)messageSendDidTapImagePicker:(MessageSendViewController *)bar
{
  (void)bar;
  if ([[self chatId] length] == 0) return;
  if (![UIImagePickerController isSourceTypeAvailable:
          UIImagePickerControllerSourceTypePhotoLibrary]) {
    ENILLog(@"MessageListViewController.imagePicker",
            @"photo library source unavailable");
    return;
  }
  UIImagePickerController *picker = [[UIImagePickerController alloc] init];
  [picker setSourceType:UIImagePickerControllerSourceTypePhotoLibrary];
  [picker setDelegate:self];
  [self enil_presentModalViewController:picker];
}

#pragma mark - StickerPickerDelegate

/* One-shot sticker send: dismiss, then route through the engine. The optimistic
 * bubble + confirm-swap ride the existing ENILMessageJSNotification path (same
 * as text send), so there is no manual appendMessage here. */
- (void)stickerPicker:(StickerViewController *)picker
     didPickPackageId:(NSString *)packageId
            stickerId:(NSString *)stickerId
{
  [picker setPickerDelegate:nil];
  [self enil_dismissModalViewController];
  if (![packageId length] || ![stickerId length] || ![[self chatId] length]) return;
  @try {
    if (![[self account] sendStickerId:stickerId
                           packageId:packageId
                            toChatId:[self chatId]]) {
      ENILLog(@"MessageListViewController.stickerPicker",
              @"sendStickerId returned NO for chat %@", [self chatId]);
    }
  } @catch (NSException *exception) {
    ENILLog(@"MessageListViewController.stickerPicker",
            @"exception: %@", exception);
  }
}

/* Inline-sticon pick: dismiss, then splice the sticon into the compose bar's
 * attributed draft at the current insertion point (unlike stickers, which send
 * one-shot). The bar carries the identity in the attributed text until send. */
- (void)stickerPicker:(StickerViewController *)picker
didPickSticonPackageId:(NSString *)packageId
             sticonId:(NSString *)sticonId
              altText:(NSString *)altText
{
  [picker setPickerDelegate:nil];
  [self enil_dismissModalViewController];
  if (![packageId length] || ![sticonId length]) return;
  [[self sendBar] insertSticonWithPackageId:packageId sticonId:sticonId altText:altText];
}

- (void)stickerPickerDidCancel:(StickerViewController *)picker
{
  [picker setPickerDelegate:nil];
  [self enil_dismissModalViewController];
}

#pragma mark - UIImagePickerControllerDelegate

/* Write the chosen image to a unique temp-directory JPEG; returns nil (never a
 * bogus path) on any failure so the caller can bail cleanly. */
- (NSString *)writeTempJPEGForImage:(UIImage *)image
{
  if (!image) return nil;
  NSData *data = UIImageJPEGRepresentation(image, 0.9f);
  if ([data length] == 0) return nil;
  NSString *name = [NSString stringWithFormat:@"compose-%@.jpg",
    [[NSProcessInfo processInfo] globallyUniqueString]];
  NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
  NSError *error = nil;
  if (![data writeToFile:path options:NSDataWritingAtomic error:&error]) {
    ENILLog(@"MessageListViewController.writeTempJPEG",
            @"write failed: %@", error);
    return nil;
  }
  return path;
}

/* Persist the picked image and hand its path to the engine. -sendImageAtPath:
 * toChatId: runs the wire-send on a background thread and posts
 * ENILMessageJSNotification for the optimistic bubble + confirm-swap (same path
 * as text and sticker sends), so there is nothing to append here. */
- (void)imagePickerController:(UIImagePickerController *)picker
        didFinishPickingMediaWithInfo:(NSDictionary *)info
{
  (void)picker;
  [self enil_dismissModalViewController];
  NSString *path = [self writeTempJPEGForImage:
    [info objectForKey:UIImagePickerControllerOriginalImage]];
  if ([path length] == 0 || [[self chatId] length] == 0) return;
  @try {
    if (![[self account] sendImageAtPath:path toChatId:[self chatId]]) {
      ENILLog(@"MessageListViewController.imagePicker",
              @"sendImageAtPath returned NO for chat %@", [self chatId]);
    }
  } @catch (NSException *exception) {
    ENILLog(@"MessageListViewController.imagePicker",
            @"exception: %@", exception);
  }
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController *)picker
{
  (void)picker;
  [self enil_dismissModalViewController];
}

#pragma mark - UIWebViewDelegate

/* Intercept the engine's custom schemes; let every other navigation through.
 * Same protocol as the macOS nav/policy delegate, one delegate method. */
- (BOOL)webView:(UIWebView *)webView
    shouldStartLoadWithRequest:(NSURLRequest *)request
                navigationType:(UIWebViewNavigationType)navigationType
{
  (void)webView; (void)navigationType;
  NSURL *url = [request URL];
  NSString *scheme = [url scheme];
  if ([scheme isEqualToString:@"enil-action"]) {
    if ([[url host] isEqualToString:@"load-more"]) [self loadMoreMessages];
    return NO;
  }
  if ([scheme isEqualToString:@"enil-media"]) {
    [self presentMediaAtRelativePath:
      [[url host] stringByAppendingString:[url path]]];
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
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_webView setDelegate:nil];
  [_sendBar setDelegate:nil];
  /* The shared picker outlives us; its pickerDelegate is unsafe_unretained, so
   * drop it if it still points here (the dismiss paths normally have already). */
  if ([_stickerPicker pickerDelegate] == self) [_stickerPicker setPickerDelegate:nil];
}

@end
