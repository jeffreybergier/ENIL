#import <AppKit/AppKit.h>
#import "AICookieCutterWindowController.h"
#import "AIWebViewController.h"
#import "ENILAccount.h"
#import "MessageSendViewController.h"

/* MessageListViewController — the chat pane's everything-VC.
 *
 * Class structure: subclass of AIWebViewController. Its view IS the
 * AIWebViewController container — the WKWebView (Middle/Modern) or
 * WebKit1 WebView (Legacy) is the base-class-managed subview, with its
 * frame explicitly sized to (0, barHeight, w, h-barHeight) in
 * -viewDidLayout so the send bar sits as a true sibling below it (rather
 * than overlapping). The widthSizable | heightSizable autoresize mask
 * the base class applies to the WebView keeps the bottom margin equal
 * to barHeight across container resizes once the initial frame anchors
 * it. The send bar (MessageSendViewController) is a child VC added to
 * the same container at y=0. The bar paints its own background
 * (ENILBottomToolbarView), so there is no dependency on AppKit's window
 * content-border chrome.
 *
 * Per-chat HTML caching: -contentURL delegates to
 * -[ENILAccount writeChatPageHTMLForChatId:outOldest:outHasMore:], which
 * writes <enilDir>/chats/<chatId>.html (or chats/_empty.html when no chat
 * is selected) and returns the file URL. WKWebView loads the file via
 * -loadFileURL:allowingReadAccessToURL: scoped to enilDir_ — the only
 * load entry point that gives the page a real file:// origin, so
 * <img src="media/..."> for sticker/media files isn't blocked as
 * cross-origin from an opaque-origin document. The legacy WebKit1 path
 * has no opaque-origin SOP block so the base class hands the URL to
 * -[WebFrame loadRequest:] directly. */
@interface MessageListViewController : AIWebViewController <MessageSendDelegate> {
 @private
  ENILAccount               *engine_;
  NSString                  *chatId_;
  MessageSendViewController *sendController_;
  long long                  oldestTime_; /* pagination cursor — created_at of
                                             the oldest currently-rendered
                                             message; 0 when empty. */
  BOOL                       hasMore_;    /* older history remains on the
                                             server. */
}

- (id)initWithEngine:(ENILAccount *)engine;
- (NSString *)chatId;
- (void)reloadWithChatId:(NSString *)chatId displayName:(NSString *)name;
- (void)reloadData;
/* Page in the next batch of older messages (no-op if none). Safe to call
 * from the HTML button, toolbar, or main menu. */
- (void)loadMoreMessages;
- (BOOL)hasMoreMessages;
- (void)insertSticonWithPackageId:(NSString *)packageId
                         sticonId:(NSString *)sticonId
                          altText:(NSString *)altText;

/* Send passthrough to the embedded MessageSendViewController, so the Chat
 * menu's "Send Message" can be driven from ChatWindowController (a stable
 * responder) rather than depending on text-view first-responder state. */
- (BOOL)canSendCurrentMessage;
- (void)sendCurrentMessage;

/* Trigger the embedded send VC's image-picker sheet — used by the toolbar
 * Photo segment in ChatWindowController. */
- (void)presentImagePicker;

@end
