#import <AppKit/AppKit.h>
#import "AICookieCutterWindowController.h"
#import "ENILAccount.h"
#import "XPAppKit.h"

@class StatusViewController;

@protocol ChatListViewControllerDelegate
- (void)chatSelected:(NSString *)chatId displayName:(NSString *)name;
@end

@interface ChatListViewController : AIViewController <XPTableViewDataSource, XPTableViewDelegate> {
 @private
  NSScrollView         *scrollView_;
  NSTableView          *tableView_;
  NSArray              *rows_;
  NSString             *selectedChatId_;
  NSMenu               *contextMenu_;
  ENILAccount          *engine_;
  StatusViewController *statusController_;
  id<ChatListViewControllerDelegate> delegate_;
  /* mid -> NSImage avatar cache. Lazily populated at cell-draw time so only
   * the ~15 visible rows ever decode a JPEG, instead of every contact/chat
   * up front (the launch beachball on big DBs). A missing file caches as
   * NSNull so we never re-stat it. Flushed on sync finish — that is the only
   * time avatar files change on disk. */
  NSMutableDictionary  *avatarCache_;
}
- (id)initWithEngine:(ENILAccount *)engine;
- (void)reloadData;
/* Surgical single-chat refresh: re-reads one chat summary and re-seats its row
 * (or drops it) without the full-roster rebuild -reloadData performs. Driven by
 * ENILSSEEventNotification's @"chat_id" so an incoming message touches one row. */
- (void)updateChatId:(NSString *)chatId;
/* Drop every cached avatar so the next draw re-reads from disk. Call after a
 * sync that may have downloaded new/updated avatar files. */
- (void)flushAvatarCache;
/* Lazy, cached avatar lookup by mid — the cell draw path calls this so disk
 * I/O is bounded to visible rows. Returns nil when no avatar file exists. */
- (NSImage *)cachedAvatarForMid:(NSString *)mid;
- (void)selectChatId:(NSString *)chatId;
- (void)clearSelection;
- (NSString *)selectedChatId;
- (void)setDelegate:(id<ChatListViewControllerDelegate>)delegate;
@end
