#import <AltivecCocoa/AICollectionTableViewController.h>
//
//  ChatListViewController.h
//  ENIL
//

#import <UIKit/UIKit.h>

@class ENILAccount;
@class ENILRootCoordinator;

/* The chat roster (navigation root). Lists the active account's chats; tapping
 * one pushes the message thread. The left nav button presents the settings /
 * account switcher (PreferencesViewController). The macOS peer is the sidebar
 * pane (source/macOS/ChatListViewController). */
@interface ChatListViewController : AICollectionTableViewController
- (instancetype)initWithAccount:(ENILAccount *)account
                    coordinator:(ENILRootCoordinator *)coordinator;
@end
