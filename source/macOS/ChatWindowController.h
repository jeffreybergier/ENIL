#import <AppKit/AppKit.h>
#import "AICookieCutterWindowController.h"
#import "AboutWindowController.h"
#import "ENILAccount.h"
#import "ChatListViewController.h"
#import "MessageListViewController.h"
#import "StickerViewController.h"
#import "ENILResponderActions.h"
#import "XPAppKit.h"

// --- ChatWindowController Interface ---
@interface ChatWindowController : AICookieCutterWindowController
    <ChatListViewControllerDelegate, StickerViewControllerDelegate,
     ENILResponderActions, XPToolbarDelegate> {
 @private
  ENILAccount                    *engine_;
  ChatListViewController         *chatsController_;
  MessageListViewController      *messagesController_;
  StickerViewController      *stickerController_;
  NSString                       *activeChatId_;
  NSString                       *activeDisplayName_;
  /* Weak ref to the live picker toolbar segmented control. NSToolbarItem
   * retains its -view, so we don't own it — but we DO need to find it to
   * sync its selection when the inspector is toggled from elsewhere
   * (menu / keyboard / programmatic). Updated each time the toolbar
   * factory builds the item with willBeInsertedIntoToolbar:YES. */
  NSSegmentedControl             *pickerSegmented_;
}
- (id)initWithEngine:(ENILAccount *)engine;
@end
