//
//  ChatListViewController.m
//  ENIL
//

#import "ChatListViewController.h"
#import "MessageListViewController.h"
#import "PreferencesViewController.h"
#import "ENILRootCoordinator.h"
#import "UIViewController+ENILModal.h"
#import "SyncMiniBarView.h"
#import "ENILAccount.h"
#import "XPFoundation.h"
#import "AIFontAwesome.h"

static const CGFloat kAvatarSize = 32.0;  /* matches macOS ENILListCell */

/* Three grouped sections, mirroring the macOS ChatListViewController -buildRows:
 * the logged-in account, then existing chats, then the friend roster. UIKit's
 * real sections + plain-style sticky headers stand in for the macOS "group row"
 * fake-sections (-isGroupRow: + XP_setFloatsGroupRows:). */
enum {
  kSectionAccount = 0,
  kSectionChats,
  kSectionFriends,
  kSectionCount
};

@interface ChatListViewController () <UIActionSheetDelegate>
@property (nonatomic, strong) ENILAccount *account;
@property (nonatomic, assign) ENILRootCoordinator *coordinator; /* back-pointer (4.3 floor) */
@property (nonatomic, strong) NSDictionary *profile; /* account row (ENILStructDict) */
@property (nonatomic, copy)   NSArray *chats;
@property (nonatomic, copy)   NSArray *contacts; /* friend roster */
@property (nonatomic, strong) UIBarButtonItem *syncButton;
@property (nonatomic, strong) UIBarButtonItem *linkButton; /* SSE on/off toggle */
@property (nonatomic, strong) SyncMiniBarView *syncBar; /* centered toolbar status */
@property (nonatomic, strong) UIImage *placeholderAvatar;
/* mid -> UIImage avatar cache. UITableView already vends cells lazily, so this
 * isn't about the launch decode (the macOS peer's problem); it stops the
 * decode + circular-clip from re-running on every cell reuse / scroll. A
 * missing file caches as NSNull so an avatar-less row never re-stats disk.
 * Flushed on sync finish — the only time avatar files change on disk. */
@property (nonatomic, strong) NSMutableDictionary *avatarCache;
@property (nonatomic, copy)   NSString *pendingChatId;      /* friend-tap action sheet ctx */
@property (nonatomic, copy)   NSString *pendingDisplayName; /* friend-tap action sheet ctx */
@end

@implementation ChatListViewController

- (instancetype)initWithAccount:(ENILAccount *)account
                    coordinator:(ENILRootCoordinator *)coordinator
{
  if (account == nil) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException
                                  reason:@"account is nil"
                                userInfo:nil];
  }
  if ((self = [super initWithStyle:UITableViewStylePlain])) {
    _account = account;
    _coordinator = coordinator;
    self.navigationItem.title = NSLocalizedString(@"ENIL", nil);
  }
  return self;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  /* Sync: macOS's SyncMini glyph, white and bordered. -syncStateChanged:
   * toggles enabled (native on a real bar item). */
  UIImage *syncImage = [self whiteBarIconForIcon:AIFAArrowRotateRight];
  self.syncButton =
    [[UIBarButtonItem alloc] initWithImage:syncImage
                                     style:UIBarButtonItemStyleBordered
                                    target:self
                                    action:@selector(syncAction)];
  self.syncButton.accessibilityLabel = NSLocalizedString(@"Sync", nil);
  /* The sync button rides at the right of the bottom toolbar (installed in
   * -installSyncBar), mirroring the macOS SyncMiniViewController which pins its
   * sync button to the bar's right edge. Not in the navigation bar. */
  /* SSE on/off toggle — rides at the LEFT of the bottom toolbar. The glyph is
   * link / link-slash, refreshed from the account's persisted desired state in
   * -updateLinkButtonIcon. */
  self.linkButton =
    [[UIBarButtonItem alloc] initWithImage:[self whiteBarIconForIcon:AIFALink]
                                     style:UIBarButtonItemStyleBordered
                                    target:self
                                    action:@selector(toggleSSEAction)];
  [self updateLinkButtonIcon];
  /* Settings / account switcher: white gear, bordered style. */
  UIImage *settingsImage = [self whiteBarIconForIcon:AIFAGear];
  self.navigationItem.leftBarButtonItem =
    [[UIBarButtonItem alloc] initWithImage:settingsImage
                                     style:UIBarButtonItemStyleBordered
                                    target:self
                                    action:@selector(accountsAction)];
  self.navigationItem.leftBarButtonItem.accessibilityLabel =
    NSLocalizedString(@"Accounts", nil);
  [self installSyncBar];
  [self observeSyncNotifications];
  /* The coordinator starts SSE (-> Live) before this controller exists, so the
   * state notification fired before our bar's queue and our own observer were
   * listening. Re-announce now that both are wired, so the bar shows "Syncing
   * Live" rather than a stale "Sync Required". */
  [self.account announceSyncState];
  [self reloadData];
}

/* Host the status view in the navigation controller's real bottom UIToolbar,
 * laid out as [flexLeft, statusView, flexRight, syncButton]: the two flexible
 * spaces center the content-hugging status view in the region LEFT of the
 * trailing sync button, so rotation needs no width math and the platform
 * toolbar draws the chrome/gloss for us. This mirrors the macOS
 * SyncMiniViewController, which pins its sync button to the bar's right edge and
 * centers the status text in the space beside it. The view owns its own
 * ENILSyncStatusQueue and self-wires to the sync notifications, so there is no
 * per-event plumbing here: placement is the whole job. */
- (void)installSyncBar
{
  self.syncBar = [[SyncMiniBarView alloc] initWithFrame:CGRectZero
                                                account:self.account];
  UIBarButtonItem *flexLeft = [[UIBarButtonItem alloc]
    initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace
                         target:nil action:NULL];
  UIBarButtonItem *flexRight = [[UIBarButtonItem alloc]
    initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace
                         target:nil action:NULL];
  UIBarButtonItem *item =
    [[UIBarButtonItem alloc] initWithCustomView:self.syncBar];
  self.toolbarItems =
    [NSArray arrayWithObjects:self.linkButton, flexLeft, item, flexRight,
                              self.syncButton, nil];
}

/* The bottom toolbar is per-navigation-controller, shared across pushes, so we
 * own its visibility only while this screen is on top: show on appear, hide on
 * push to the message thread (which carries no toolbar of its own). */
- (void)viewWillAppear:(BOOL)animated
{
  [super viewWillAppear:animated];
  [self.navigationController setToolbarHidden:NO animated:animated];
}

- (void)viewWillDisappear:(BOOL)animated
{
  [super viewWillDisappear:animated];
  [self.navigationController setToolbarHidden:YES animated:animated];
}

/* Tight `color` glyph (iconSize == canvasSize). AIFontAwesome bakes the tint in
 * directly, so no mask-and-fill pass here. */
- (UIImage *)tintedIconForIcon:(AIFontAwesomeIcon)icon
                     pointSize:(CGFloat)pointSize
                         color:(UIColor *)color
{
  return [AIFontAwesome imageForIcon:icon
                             style:AIFontAwesomeStyleSolid
                          iconSize:pointSize
                        canvasSize:pointSize
                             color:color
                             scale:0.0f];  /* 0 -> [UIScreen mainScreen].scale */
}

/* White nav-bar glyph: 18pt icon centered in a 28pt canvas. The canvas stays a
 * 28pt square — deliberately under the navigation bar's button content height —
 * so a bordered UIBarButtonItem draws the image 1:1, centered, and does NOT
 * resize it into its wider-than-tall bezel (which is what stretched a 36pt
 * canvas horizontally). The transparent border supplies the bezel padding. */
- (UIImage *)whiteBarIconForIcon:(AIFontAwesomeIcon)icon
{
  return [AIFontAwesome imageForIcon:icon
                             style:AIFontAwesomeStyleSolid
                          iconSize:18.0
                        canvasSize:28.0
                             color:[UIColor whiteColor]
                             scale:0.0f];  /* 0 -> [UIScreen mainScreen].scale */
}

/* Present the settings / account switcher (PreferencesViewController) modally,
 * wrapped in its own nav controller for a title. Preferences owns its own Done
 * button (it commits worker edits, then self-dismisses when modal). The
 * "[Account ▾]" entry point from PLAN.IOS.md; the macOS peer is the Accounts
 * menu. */
- (void)accountsAction
{
  PreferencesViewController *prefs = [[PreferencesViewController alloc] init];
  prefs.coordinator = self.coordinator;
  prefs.dismissesOnDone = YES;   /* modal: Done closes back to the chat list */
  UINavigationController *nav =
    [[UINavigationController alloc] initWithRootViewController:prefs];
  [self enil_presentModalViewController:nav];
}

/* The SyncMiniBarView's own queue observes ENILSyncStatusNotification for the
 * label/progress/shimmer; this controller only watches the events that change
 * its *data* (finish + SSE -> reload) or its nav button (state -> enable). */
- (void)observeSyncNotifications
{
  NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
  [nc addObserver:self
         selector:@selector(syncDidFinish:)
             name:ENILSyncDidFinishNotification
           object:nil];
  [nc addObserver:self
         selector:@selector(syncStateChanged:)
             name:ENILSyncStateChangedNotification
           object:nil];
  [nc addObserver:self
         selector:@selector(sseEvent:)
             name:ENILSSEEventNotification
           object:nil];
}

/* Reload all three sections' backing data, mirroring the macOS -buildRows
 * (profile -> Account, chats -> Chats, contacts -> Friends). Every [account *]
 * accessor returns ENILStructDict rows; read keys individually and never
 * -dictionaryWithDictionary: one, which crashes on its nil optionals (AGENTS.md). */
- (void)reloadData
{
  @try {
    self.profile  = [self.account profile];
    self.chats    = [self.account chats];
    self.contacts = [self.account contacts];
  } @catch (NSException *exception) {
    ENILLog(@"ChatListViewController.reloadData", @"exception: %@", exception);
    self.profile  = nil;
    self.chats    = [NSArray array];
    self.contacts = [NSArray array];
  }
  [self.tableView reloadData];
}

#pragma mark - Sync

- (void)syncAction
{
  @try {
    [self.account startSync];   /* bar reflects progress from the queue */
  } @catch (NSException *exception) {
    ENILLog(@"ChatListViewController.syncAction", @"exception: %@", exception);
  }
}

/* Flip the persisted SSE preference. The account stops/starts the stream and
 * announces the new state, which lands in -syncStateChanged: to repaint the
 * glyph — so we don't update the icon here, the state notification does. */
- (void)toggleSSEAction
{
  @try {
    [self.account setSSEEnabled:![self.account isSSEEnabled]];
  } @catch (NSException *exception) {
    ENILLog(@"ChatListViewController.toggleSSEAction", @"exception: %@", exception);
  }
}

/* link when SSE is enabled, link-slash when the user has turned it off. Driven
 * by the persisted desired state, not live connection status, so a transient
 * Error/Syncing phase still shows "link" (enabled, just not connected now). */
- (void)updateLinkButtonIcon
{
  BOOL enabled = [self.account isSSEEnabled];
  self.linkButton.image =
    [self whiteBarIconForIcon:(enabled ? AIFALink : AIFALinkSlash)];
  self.linkButton.accessibilityLabel = enabled
    ? NSLocalizedString(@"Disconnect", nil)
    : NSLocalizedString(@"Connect", nil);
}

- (void)syncDidFinish:(NSNotification *)note
{
  BOOL ok = [[[note userInfo] objectForKey:@"ok"] boolValue];
  ENILLog(@"ChatListViewController.syncDidFinish", @"ok=%d", (int)ok);
  /* A sync may have downloaded new/updated avatar JPEGs — drop the cached
   * decodes so the next draw picks them up. -sseEvent: deliberately does not:
   * SSE never touches avatar files, so its reloads keep the cache warm. */
  [self flushAvatarCache];
  [self reloadData];
}

- (void)syncStateChanged:(NSNotification *)note
{
  int state = [[[note userInfo] objectForKey:@"state"] intValue];
  self.syncButton.enabled = (state != ENILSyncStateSyncing);
  [self updateLinkButtonIcon];
}

/* SSE delivers messages out of band and never posts ENILSyncDidFinishNotification,
 * so the list must update here to pick up new unreadCount / lastDeliveredTime —
 * otherwise the unseen dot won't appear until the next full sync. Mirrors
 * -[ChatWindowController sseEvent:]: a message event moves at most one chat, so
 * apply a surgical single-row update; only rare structural events full-reload. */
- (void)sseEvent:(NSNotification *)note
{
  NSDictionary *userInfo = [note userInfo];
  NSString *type = [userInfo objectForKey:@"type"];
  if ([type isEqualToString:@"message"]) {
    /* The account layer surfaces which chat changed order/unread as @"chat_id";
     * update that one row instead of rebuilding the whole roster (the old
     * unconditional -reloadData re-queried profile + every contact + every chat
     * on every incoming message). Reactions/deletes/peer-read carry no chat_id
     * — no list change — and are reflected in the open thread via its
     * ENILMessageJSNotification, so the list is left untouched here. */
    NSString *cid = [userInfo objectForKey:@"chat_id"];
    if ([cid length]) [self updateChatId:cid];
    return;
  }
  if ([type isEqualToString:@"ping"] ||
      [type isEqualToString:@"reconnect"] ||
      [type isEqualToString:@"connInfoRevision"]) return;
  /* Non-message structural events (talkException, fullSync, …) are rare; a full
   * rebuild keeps the list coherent. */
  [self reloadData];
}

/* Surgical single-chat refresh for live SSE updates: re-reads ONE chat summary
 * and re-seats its row in the Chats section, instead of -reloadData's full
 * rebuild (profile + every contact + every chat). The Chats section is sorted
 * lastDeliveredTime DESC, so we drop the chat's old row and re-insert it at the
 * position that keeps that order — a new message floats to the top; a read
 * receipt (unchanged timestamp) lands back in place. A nil summary means the
 * chat lost its message box (deleted) and just drops out. Only the Chats section
 * reloads, leaving the Account/Friends rows and their warm avatar cache alone.
 * The summary is the same ENILStructDict shape as -[ENILAccount chats] rows, so
 * it goes straight into self.chats — no transform needed. */
- (void)updateChatId:(NSString *)chatId
{
  NSMutableArray *mut;
  NSDictionary *summary = nil;
  NSUInteger i;

  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return;

  mut = [NSMutableArray arrayWithArray:self.chats];

  /* Drop the chat's existing row, if any (chatMid is unique within Chats). */
  for (i = 0; i < [mut count]; i++) {
    if ([[[mut objectAtIndex:i] objectForKey:@"chatMid"] isEqualToString:chatId]) {
      [mut removeObjectAtIndex:i];
      break;
    }
  }

  @try {
    summary = [self.account chatSummaryForId:chatId];
  } @catch (NSException *exception) {
    ENILLog(@"ChatListViewController.updateChatId", @"exception: %@", exception);
    [self reloadData];
    return;
  }

  /* Re-insert at the sorted slot: skip rows newer-or-equal, insert before the
   * first older one. nil summary => chat is gone; the removal above suffices. */
  if (summary != nil) {
    long long ts = [[summary objectForKey:@"lastDeliveredTime"] longLongValue];
    NSUInteger insertAt = 0;
    while (insertAt < [mut count]) {
      long long rowTs =
        [[[mut objectAtIndex:insertAt] objectForKey:@"lastDeliveredTime"] longLongValue];
      if (rowTs < ts) break;
      insertAt++;
    }
    [mut insertObject:summary atIndex:insertAt];
  }

  self.chats = mut; /* copy -> immutable snapshot, matching -reloadData */
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:kSectionChats]
                withRowAnimation:UITableViewRowAnimationNone];
}

#pragma mark - Row data

- (NSDictionary *)chatAtRow:(NSInteger)row
{
  NSUInteger r = (NSUInteger)row;
  return r < [self.chats count] ? [self.chats objectAtIndex:r] : nil;
}

- (NSDictionary *)contactAtRow:(NSInteger)row
{
  NSUInteger r = (NSUInteger)row;
  return r < [self.contacts count] ? [self.contacts objectAtIndex:r] : nil;
}

- (NSString *)displayNameForChat:(NSDictionary *)chat
{
  NSString *name = [chat objectForKey:@"displayName"];
  if ([name length]) return name;
  NSString *mid = [chat objectForKey:@"chatMid"];
  return [mid length] ? mid : NSLocalizedString(@"(unknown)", nil);
}

/* Friend display name, matching macOS -buildRows: the per-account override wins,
 * then the LINE displayName. */
- (NSString *)displayNameForContact:(NSDictionary *)contact
{
  NSString *name = [contact objectForKey:@"displayNameOverridden"];
  if (![name length]) name = [contact objectForKey:@"displayName"];
  return [name length] ? name : NSLocalizedString(@"(unknown)", nil);
}

/* Subtitle for account/friend rows — the LINE status message, or empty. */
- (NSString *)statusMessageOf:(NSDictionary *)row
{
  NSString *s = [row objectForKey:@"statusMessage"];
  return [s length] ? s : @"";
}

/* Subtitle line for a chat row — the last-activity timestamp, matching the macOS
 * list cell. lastDeliveredTime is epoch millis on the chat row (chat_summary_t);
 * 0/absent yields an empty subtitle. */
- (NSString *)subtitleForChat:(NSDictionary *)chat
{
  long long ms = [[chat objectForKey:@"lastDeliveredTime"] longLongValue];
  if (ms <= 0) return @"";
  static NSDateFormatter *fmt = nil;
  if (fmt == nil) {
    fmt = [[NSDateFormatter alloc] init];
    [fmt setDateStyle:NSDateFormatterMediumStyle];
    [fmt setTimeStyle:NSDateFormatterShortStyle];
  }
  NSDate *d = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)(ms / 1000)];
  return [fmt stringFromDate:d];
}

/* Circular 32pt avatar for a user mid, loaded from <enilDir>/avatars/<mid>.jpg.
 * Only user mids are downloaded (see enil_sync download_avatars), so this
 * resolves 1:1 chats; returns nil when there's no file so the caller falls back
 * to the placeholder. Mirrors the macOS cell's oval-clipped avatar. */
- (UIImage *)avatarImageForMid:(NSString *)mid
{
  if (![mid length]) return nil;
  NSString *path = [NSString stringWithFormat:@"%@/avatars/%@.jpg",
                    [self.account enilDir], mid];
  UIImage *raw = [UIImage imageWithContentsOfFile:path];
  if (raw == nil) return nil;
  CGRect rect = CGRectMake(0.0f, 0.0f, kAvatarSize, kAvatarSize);
  UIGraphicsBeginImageContextWithOptions(rect.size, NO, 0.0f);
  [[UIBezierPath bezierPathWithOvalInRect:rect] addClip];
  [raw drawInRect:rect];
  UIImage *circular = UIGraphicsGetImageFromCurrentImageContext();
  UIGraphicsEndImageContext();
  return circular;
}

/* Cached front end to -avatarImageForMid:. Decodes + circular-clips each avatar
 * at most once per cache lifetime; a missing file caches as NSNull so an
 * avatar-less row never re-stats disk on cell reuse / scroll. Called from the
 * cell-config path, so only on-screen rows ever reach -avatarImageForMid:. */
- (UIImage *)cachedAvatarForMid:(NSString *)mid
{
  if (![mid isKindOfClass:[NSString class]] || ![mid length]) return nil;
  if (self.avatarCache == nil) self.avatarCache = [NSMutableDictionary dictionary];
  id cached = [self.avatarCache objectForKey:mid];
  if (cached != nil) return (cached == [NSNull null]) ? nil : (UIImage *)cached;
  UIImage *img = [self avatarImageForMid:mid];
  [self.avatarCache setObject:(img ? (id)img : (id)[NSNull null]) forKey:mid];
  return img;
}

/* Drop every cached avatar so the next cell-config re-reads from disk. Called
 * after a sync that may have downloaded new/updated avatar files; SSE reloads
 * (-sseEvent:) skip it — they never touch avatar files, so the cache stays
 * warm there. */
- (void)flushAvatarCache
{
  [self.avatarCache removeAllObjects];
}

/* Neutral grey circle for rows without an avatar file (groups/rooms, or a 1:1
 * whose picture hasn't synced). Cached — the same image serves every such row,
 * and giving every row an image keeps the text left-margin aligned. */
- (UIImage *)placeholderAvatar
{
  if (_placeholderAvatar != nil) return _placeholderAvatar;
  CGRect rect = CGRectMake(0.0f, 0.0f, kAvatarSize, kAvatarSize);
  UIGraphicsBeginImageContextWithOptions(rect.size, NO, 0.0f);
  [[UIColor lightGrayColor] setFill];
  [[UIBezierPath bezierPathWithOvalInRect:rect] fill];
  _placeholderAvatar = UIGraphicsGetImageFromCurrentImageContext();
  UIGraphicsEndImageContext();
  return _placeholderAvatar;
}

/* Blue 12pt unseen dot for the trailing accessory slot, matching the macOS list
 * cell (NSColor blueColor, kDotDiam 12). Returns nil for seen chats so a
 * recycled cell drops its stale dot. */
- (UIView *)unseenAccessoryForChat:(NSDictionary *)chat
{
  int unseen = [[chat objectForKey:@"unreadCount"] intValue];
  if (unseen <= 0) return nil;
  UIImage *dot = [self tintedIconForIcon:AIFACircle pointSize:12.0
                                   color:[UIColor blueColor]];
  return [[UIImageView alloc] initWithImage:dot];
}

#pragma mark - UITableViewDataSource

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
  (void)tableView;
  return kSectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section
{
  (void)tableView;
  switch (section) {
    case kSectionAccount: return self.profile ? 1 : 0;
    case kSectionChats:   return (NSInteger)[self.chats count];
    case kSectionFriends: return (NSInteger)[self.contacts count];
  }
  return 0;
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section
{
  (void)tableView;
  switch (section) {
    case kSectionAccount: return NSLocalizedString(@"Account", nil);
    case kSectionChats:   return NSLocalizedString(@"Chats", nil);
    case kSectionFriends: return NSLocalizedString(@"Friends", nil);
  }
  return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
  static NSString *cellId = @"ChatCell";
  UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:cellId];
  if (cell == nil) {
    cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                  reuseIdentifier:cellId];
  }
  /* Cells recycle across all three sections, so every branch must set the full
   * mutable set (text, detail, image, accessory, selection) or a reused cell
   * keeps the previous row's state. */
  switch (indexPath.section) {
    case kSectionAccount: [self configureAccountCell:cell]; break;
    case kSectionChats:   [self configureChatCell:cell atRow:indexPath.row]; break;
    case kSectionFriends: [self configureContactCell:cell atRow:indexPath.row]; break;
  }
  return cell;
}

/* Logged-in account: avatar + display name + status message. Non-selectable
 * (matches macOS, where -selectableMid returns nil for the account row). */
- (void)configureAccountCell:(UITableViewCell *)cell
{
  NSString *mid  = [self.profile objectForKey:@"mid"];
  NSString *name = [self.profile objectForKey:@"displayName"];
  cell.textLabel.text = [name length] ? name : NSLocalizedString(@"(unknown)", nil);
  cell.detailTextLabel.text = [self statusMessageOf:self.profile];
  UIImage *avatar = [self cachedAvatarForMid:mid];
  cell.imageView.image = avatar ? avatar : [self placeholderAvatar];
  cell.accessoryView = nil;
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
}

/* Existing chat: avatar + name + last-activity time + unread dot. The avatar is
 * keyed by chatMid: a 1:1 chat's mid is the peer's user mid (avatars/<u-mid>.jpg
 * from the contacts sync); a group/room mid resolves to its own picture
 * (avatars/<c-mid>.jpg from download_avatars). A group with no custom picture has
 * no file and falls back to the placeholder — never a random member's avatar. */
- (void)configureChatCell:(UITableViewCell *)cell atRow:(NSInteger)row
{
  NSDictionary *chat = [self chatAtRow:row];
  cell.textLabel.text = [self displayNameForChat:chat];
  cell.detailTextLabel.text = [self subtitleForChat:chat];
  NSString *chatMid = [chat objectForKey:@"chatMid"];
  UIImage *avatar = [self cachedAvatarForMid:chatMid];
  cell.imageView.image = avatar ? avatar : [self placeholderAvatar];
  cell.accessoryView = [self unseenAccessoryForChat:chat];
  cell.selectionStyle = UITableViewCellSelectionStyleBlue;
}

/* Friend (implicit 1:1): avatar + name + status message. Tapping confirms via a
 * UIActionSheet before opening, so no unseen dot here. */
- (void)configureContactCell:(UITableViewCell *)cell atRow:(NSInteger)row
{
  NSDictionary *contact = [self contactAtRow:row];
  cell.textLabel.text = [self displayNameForContact:contact];
  cell.detailTextLabel.text = [self statusMessageOf:contact];
  UIImage *avatar = [self cachedAvatarForMid:[contact objectForKey:@"mid"]];
  cell.imageView.image = avatar ? avatar : [self placeholderAvatar];
  cell.accessoryView = nil;
  cell.selectionStyle = UITableViewCellSelectionStyleBlue;
}

#pragma mark - UITableViewDelegate

/* The account row is non-selectable, the macOS analog of -shouldSelectRow:
 * returning NO for it. */
- (NSIndexPath *)tableView:(UITableView *)tableView
  willSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
  (void)tableView;
  return indexPath.section == kSectionAccount ? nil : indexPath;
}

- (void)tableView:(UITableView *)tableView
    didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  /* Chats open directly; Friends are gated behind a confirmation sheet, exactly
   * as the macOS list routes Friends-section clicks through -enil_confirmOpenChatId:
   * (a 1:1 chat may not exist yet) while Chats-section clicks open immediately. */
  switch (indexPath.section) {
    case kSectionChats:   [self openChatRow:indexPath.row]; break;
    case kSectionFriends: [self confirmOpenContactRow:indexPath.row]; break;
    default: break;
  }
}

- (void)openChatRow:(NSInteger)row
{
  NSDictionary *chat = [self chatAtRow:row];
  [self openChatId:[chat objectForKey:@"chatMid"]
       displayName:[self displayNameForChat:chat]];
}

- (void)openChatId:(NSString *)chatId displayName:(NSString *)name
{
  if (![chatId length]) return;
  MessageListViewController *thread =
    [[MessageListViewController alloc] initWithAccount:self.account
                                                chatId:chatId
                                           displayName:name
                                         stickerPicker:[self.coordinator sharedStickerPicker]];
  [self.navigationController pushViewController:thread animated:YES];
}

#pragma mark - Friend-tap confirmation (UIActionSheet)

/* Mirror of the macOS -enil_confirmOpenChatId:displayName: NSAlert as a
 * UIActionSheet (the alert's two button titles + message vary by whether a chat
 * already exists). For a friend, the chat id IS the contact's user mid. The
 * pending id/name carry into the delegate callback; -openChatId: applies them on
 * confirm. */
- (void)confirmOpenContactRow:(NSInteger)row
{
  NSDictionary *contact = [self contactAtRow:row];
  NSString *chatId = [contact objectForKey:@"mid"];
  if (![chatId length]) return;
  NSString *name = [self displayNameForContact:contact];
  BOOL exists = [self.account chatExistsForMid:chatId];

  self.pendingChatId = chatId;
  self.pendingDisplayName = name;

  /* UIActionSheet has only a title (no separate informative-text field like
   * NSAlert), so the "will be created on first message" line folds into a second
   * title line for the not-yet-existing case. */
  NSString *title = exists
    ? [NSString stringWithFormat:
        NSLocalizedString(@"Open chat with \"%@\"?", nil), name]
    : [NSString stringWithFormat:@"%@\n%@",
        [NSString stringWithFormat:
          NSLocalizedString(@"Start a new chat with \"%@\"?", nil), name],
        NSLocalizedString(@"A 1:1 chat will be created when you send your "
                          @"first message.", nil)];
  NSString *confirm =
    NSLocalizedString(exists ? @"Open Chat" : @"Start Chat", nil);

  UIActionSheet *sheet =
    [[UIActionSheet alloc] initWithTitle:title
                                delegate:self
                       cancelButtonTitle:NSLocalizedString(@"Cancel", nil)
                  destructiveButtonTitle:nil
                       otherButtonTitles:confirm, nil];
  [self presentActionSheet:sheet];
}

/* Anchor to the bottom toolbar when it's showing (this screen installs one), so
 * the sheet rises above it rather than from behind; otherwise fall back to the
 * full view. */
- (void)presentActionSheet:(UIActionSheet *)sheet
{
  UIToolbar *toolbar = self.navigationController.toolbar;
  if (toolbar != nil && !toolbar.hidden) {
    [sheet showFromToolbar:toolbar];
  } else {
    [sheet showInView:self.view];
  }
}

- (void)actionSheet:(UIActionSheet *)actionSheet
clickedButtonAtIndex:(NSInteger)buttonIndex
{
  NSString *chatId = self.pendingChatId;
  NSString *name   = self.pendingDisplayName;
  self.pendingChatId = nil;
  self.pendingDisplayName = nil;
  if (buttonIndex == actionSheet.cancelButtonIndex) return;
  [self openChatId:chatId displayName:name];
}

- (void)dealloc
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
}

@end
