//
//  MessageListViewController.h
//  ENIL
//

#import <UIKit/UIKit.h>

@class ENILAccount;
@class StickerViewController;

/* One chat's message thread. A UIWebView loads the shared
 * chats/<chatId>.html render and observes ENILMessageJSNotification for
 * incremental updates; ENILSyncDidFinishNotification triggers a re-render.
 * Custom-scheme taps (enil-action://load-more, enil-media://<rel>) are
 * intercepted in -webView:shouldStartLoadWithRequest:navigationType:. The
 * macOS peer is source/macOS/MessageListViewController. */
@interface MessageListViewController : UIViewController
/* stickerPicker is the account-shared picker (owned by ENILRootCoordinator),
 * presented from the compose bar's sticker button. Held resident so reopening
 * is instant; nil disables the button. */
- (instancetype)initWithAccount:(ENILAccount *)account
                         chatId:(NSString *)chatId
                    displayName:(NSString *)displayName
                  stickerPicker:(StickerViewController *)stickerPicker;
@end
