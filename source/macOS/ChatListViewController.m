#import "ChatListViewController.h"
#import "ENILResponderActions.h"
#import "StatusViewController.h"
#import "XPAppKit.h"

static const CGFloat kRowHeight  = 44.0;
static const CGFloat kPadLeft    =  8.0;
static const CGFloat kPadRight   =  8.0;
static const CGFloat kDotDiam    = 12.0;
static const CGFloat kBarHeight  = 32.0;
static const CGFloat kAvatarSize = 32.0;
static const CGFloat kAvatarPad  =  4.0;
static const CGFloat kLineGap    =  2.0;

static NSString * const kRowTypeSection = @"section";
static NSString * const kRowTypeChat    = @"chat";
static NSString * const kRowTypeContact = @"contact";

/* A selectable row is either an existing chat or a friend (implicit 1:1).
 * Returns the mid to open, or nil for section/account rows. */
static NSString *selectableMid(NSDictionary *row) {
  NSString *type = [row objectForKey:@"row_type"];
  if ([type isEqualToString:kRowTypeChat])    return [row objectForKey:@"chatMid"];
  if ([type isEqualToString:kRowTypeContact]) return [row objectForKey:@"mid"];
  return nil;
}

static NSColor *listTextColor(BOOL sel) {
  return sel ? XPColorControlHighlight : [NSColor controlTextColor];
}

static NSDictionary *sectionRow(NSString *title) {
  return [NSDictionary dictionaryWithObjectsAndKeys:
    kRowTypeSection, @"row_type",
    title, @"section_title",
    nil];
}

/* Build one Chats-section row from a chat summary (an ENILDictionary from
 * -[ENILAccount chats] or -chatSummaryForId:). Shared by -buildRows (full
 * rebuild) and -updateChatId: (single-row live update) so both produce
 * byte-identical rows — the cell draw path reads displayName / unreadCount /
 * lastDeliveredTime / avatar_mid / row_type. */
static NSMutableDictionary *chatRowFromSummary(NSDictionary *chat) {
  NSMutableDictionary *row = [NSMutableDictionary dictionary];
  NSEnumerator *ke = [chat keyEnumerator];
  NSString *k, *chatMid, *chatName;
  while ((k = [ke nextObject])) {
    id v = [chat objectForKey:k];
    if (v) [row setObject:v forKey:k];
  }
  [row setObject:kRowTypeChat forKey:@"row_type"];
  chatMid = [chat objectForKey:@"chatMid"];
  /* Same fallback chain as iOS -displayNameForChat:: name -> chatMid ->
   * "(unknown)". Stored once so the list cell and chat-title path share it. */
  chatName = [chat objectForKey:@"displayName"];
  if (![chatName length]) chatName = chatMid;
  if (![chatName length]) chatName = NSLocalizedString(@"(unknown)", nil);
  [row setObject:chatName forKey:@"displayName"];
  /* Key the avatar by chatMid: a 1:1 chat's mid is the peer's user mid
   * (avatars/<u-mid>.jpg); a group/room mid resolves to its own picture. A
   * group with no custom picture has no file and falls back to the placeholder
   * circle drawn by the cell. Decoded lazily at draw time (cachedAvatarForMid:). */
  if (chatMid) [row setObject:chatMid forKey:@"avatar_mid"];
  return row;
}

// ============================================================
// ENILListCell — handles section headers and list item rows
// ============================================================

/* The table's dataSource (ChatListViewController) vends avatars lazily so the
 * cell never touches disk for off-screen rows. */
@protocol ENILAvatarSource <NSObject>
- (NSImage *)cachedAvatarForMid:(NSString *)mid;
@end

@interface ENILListCell : NSCell @end

@implementation ENILListCell

- (void)drawSectionWithFrame:(NSRect)frame row:(NSDictionary *)row {
  NSString *title = [row objectForKey:@"section_title"];
  if (!title) title = @"";
  NSDictionary *attrs = [NSDictionary dictionaryWithObjectsAndKeys:
    [NSColor grayColor], NSForegroundColorAttributeName,
    XPFontTextStyleBoldSmall, NSFontAttributeName,
    nil];
  NSSize sz = [title sizeWithAttributes:attrs];
  CGFloat y = NSMinY(frame) + floor((NSHeight(frame) - sz.height) / 2.0);
  NSRect r = NSMakeRect(NSMinX(frame) + kPadLeft, y,
                        NSWidth(frame) - kPadLeft * 2.0, sz.height);
  [title drawInRect:r withAttributes:attrs];
}

- (void)drawItemWithFrame:(NSRect)frame row:(NSDictionary *)row inView:(NSView *)view {
  NSString *rowType = [row objectForKey:@"row_type"];
  NSString *name = [row objectForKey:@"displayName"];
  if (!name) name = @"";
  BOOL sel = [self isHighlighted];
  NSColor *tc = listTextColor(sel);

  CGFloat avatarX = NSMinX(frame) + kAvatarPad;
  CGFloat avatarY = NSMinY(frame) + floor((NSHeight(frame) - kAvatarSize) / 2.0);
  NSRect avatarRect = NSMakeRect(avatarX, avatarY, kAvatarSize, kAvatarSize);

  /* Resolve the avatar lazily through the dataSource so only visible rows
   * decode a JPEG. The row carries just the mid (avatar_mid); the controller
   * caches the decoded image. */
  NSImage *avatar = nil;
  if ([view isKindOfClass:[NSTableView class]]) {
    id<ENILAvatarSource> src =
      (id<ENILAvatarSource>)[(NSTableView *)view dataSource];
    NSString *amid = [row objectForKey:@"avatar_mid"];
    if (amid && [src respondsToSelector:@selector(cachedAvatarForMid:)])
      avatar = [src cachedAvatarForMid:amid];
  }
  if (avatar) {
    NSGraphicsContext *ctx = [NSGraphicsContext currentContext];
    [ctx saveGraphicsState];
    /* Preserve AppKit's visible/dirty clip when rounding the avatar. Replacing
     * it lets partially visible rows paint over the toolbars on Tiger/Leopard. */
    [[NSBezierPath bezierPathWithOvalInRect:avatarRect] addClip];
    [avatar XP_drawInRect:avatarRect respectingFlippedView:[view isFlipped]];
    [ctx restoreGraphicsState];
  } else {
    [[NSColor lightGrayColor] set];
    [[NSBezierPath bezierPathWithOvalInRect:avatarRect] fill];
  }

  BOOL isChat = [rowType isEqualToString:kRowTypeChat];
  int unseen = isChat ? [[row objectForKey:@"unreadCount"] intValue] : 0;
  CGFloat dotReserve = (unseen > 0) ? kDotDiam + kPadRight : 0.0;
  CGFloat textX = avatarX + kAvatarSize + kPadLeft;
  CGFloat textW = NSMaxX(frame) - textX - kPadRight - dotReserve;

  NSString *subtitle = @"";
  if (isChat) {
    NSNumber *ts = [row objectForKey:@"lastDeliveredTime"];
    if (ts && [ts longLongValue] > 0) {
      static NSDateFormatter *fmt = nil;
      if (!fmt) {
        fmt = [[NSDateFormatter alloc] init];
        [fmt setFormatterBehavior:NSDateFormatterBehavior10_4];
        [fmt setDateStyle:NSDateFormatterMediumStyle];
        [fmt setTimeStyle:NSDateFormatterShortStyle];
      }
      NSDate *d = [NSDate dateWithTimeIntervalSince1970:
        (NSTimeInterval)([ts longLongValue] / 1000)];
      subtitle = [fmt stringFromDate:d];
    }
  } else {
    subtitle = [row objectForKey:@"status_message"];
    if (!subtitle) subtitle = @"";
  }

  NSDictionary *nameAttrs = [NSDictionary dictionaryWithObjectsAndKeys:
    tc, NSForegroundColorAttributeName,
    XPFontTextStyleChatName, NSFontAttributeName,
    nil];
  NSDictionary *subAttrs = [NSDictionary dictionaryWithObjectsAndKeys:
    tc, NSForegroundColorAttributeName,
    XPFontTextStyleSmallLabel, NSFontAttributeName,
    nil];

  NSSize nameSz = [name sizeWithAttributes:nameAttrs];
  NSSize subSz  = [subtitle length] ? [subtitle sizeWithAttributes:subAttrs] : NSZeroSize;
  CGFloat blockH = nameSz.height + ([subtitle length] ? kLineGap + subSz.height : 0.0);
  CGFloat blockY = NSMinY(frame) + floor((NSHeight(frame) - blockH) / 2.0);

  [name drawInRect:NSMakeRect(textX, blockY, textW, nameSz.height)
    withAttributes:nameAttrs];
  if ([subtitle length]) {
    [subtitle drawInRect:NSMakeRect(textX, blockY + nameSz.height + kLineGap, textW, subSz.height)
      withAttributes:subAttrs];
  }

  if (unseen > 0) {
    NSRect dotRect = NSMakeRect(
      NSMaxX(frame) - kDotDiam - kPadRight,
      NSMinY(frame) + floor((NSHeight(frame) - kDotDiam) / 2.0),
      kDotDiam, kDotDiam);
    [[NSColor blueColor] set];
    [[NSBezierPath bezierPathWithOvalInRect:dotRect] fill];
  }
}

- (void)drawInteriorWithFrame:(NSRect)frame inView:(NSView *)view {
  NSDictionary *row = [self objectValue];
  if ([[row objectForKey:@"row_type"] isEqualToString:kRowTypeSection])
    [self drawSectionWithFrame:frame row:row];
  else
    [self drawItemWithFrame:frame row:row inView:view];
}

@end

// ============================================================
// ENILChatTableView — routes right-clicks to a per-row context menu
// ============================================================

@protocol ENILChatTableViewMenu <NSObject>
- (NSMenu *)contextMenuForRow:(NSInteger)row;
@end

@interface ENILChatTableView : NSTableView @end

@implementation ENILChatTableView
- (NSMenu *)menuForEvent:(NSEvent *)event;
{
  NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
  NSInteger row = [self rowAtPoint:pt];
  id<ENILChatTableViewMenu> ds = (id<ENILChatTableViewMenu>)[self dataSource];
  if (row < 0 || ![ds respondsToSelector:@selector(contextMenuForRow:)])
    return nil;
  return [ds contextMenuForRow:row];
}
@end

// ============================================================
// ChatListViewController
// ============================================================

@implementation ChatListViewController

- (NSImage *)avatarForMid:(NSString *)mid;
{
  NSString *path;
  NSImage *img;
  if (!mid || ![mid length]) return nil;
  path = [NSString stringWithFormat:@"%@/avatars/%@.jpg",
    [engine_ enilDir], mid];
  img = [[NSImage alloc] initWithContentsOfFile:path];
  return [img autorelease];
}

/* Cached front end to -avatarForMid:. Decodes each avatar at most once per
 * cache lifetime; a missing file caches as NSNull so a friend without a
 * picture never re-stats the disk on every redraw/scroll. Called from the
 * cell draw path, so only on-screen rows ever reach -avatarForMid:. */
- (NSImage *)cachedAvatarForMid:(NSString *)mid;
{
  id cached;
  NSImage *img;
  if (![mid isKindOfClass:[NSString class]] || ![mid length]) return nil;
  if (!avatarCache_) avatarCache_ = [[NSMutableDictionary alloc] init];
  cached = [avatarCache_ objectForKey:mid];
  if (cached) return (cached == [NSNull null]) ? nil : (NSImage *)cached;
  img = [self avatarForMid:mid];
  [avatarCache_ setObject:(img ? (id)img : (id)[NSNull null]) forKey:mid];
  return img;
}

- (void)flushAvatarCache;
{
  [avatarCache_ removeAllObjects];
}

- (NSArray *)buildRows;
{
  NSMutableArray *rows = [NSMutableArray array];
  NSDictionary *profile;
  NSArray *chats, *contacts;
  NSUInteger i;

  [rows addObject:sectionRow(NSLocalizedString(@"Account", nil))];
  profile = [engine_ profile];
  if (profile) {
    NSMutableDictionary *row = [NSMutableDictionary dictionary];
    NSString *mid = [profile objectForKey:@"mid"];
    NSString *displayName = [profile objectForKey:@"displayName"];
    [row setObject:@"account" forKey:@"row_type"];
    if (mid) [row setObject:mid forKey:@"mid"];
    /* Same fallback as iOS -configureAccountCell:: name -> "(unknown)". */
    if (![displayName length]) displayName = NSLocalizedString(@"(unknown)", nil);
    [row setObject:displayName forKey:@"displayName"];
    /* Defer the JPEG decode to draw time (see cachedAvatarForMid:). */
    if (mid) [row setObject:mid forKey:@"avatar_mid"];
    [rows addObject:row];
  }

  contacts = [engine_ contacts];

  [rows addObject:sectionRow(NSLocalizedString(@"Chats", nil))];
  chats = [engine_ chats];
  for (i = 0; i < [chats count]; i++)
    [rows addObject:chatRowFromSummary([chats objectAtIndex:i])];

  [rows addObject:sectionRow(NSLocalizedString(@"Friends", nil))];
  for (i = 0; i < [contacts count]; i++) {
    NSDictionary *contact = [contacts objectAtIndex:i];
    NSMutableDictionary *row = [NSMutableDictionary dictionary];
    NSEnumerator *ke = [contact keyEnumerator];
    NSString *k;
    while ((k = [ke nextObject])) {
      id v = [contact objectForKey:k];
      if (v) [row setObject:v forKey:k];
    }
    /* Same fallback chain as iOS -displayNameForContact:: override -> name ->
     * "(unknown)". */
    NSString *name = [contact objectForKey:@"displayNameOverridden"];
    if (![name length]) name = [contact objectForKey:@"displayName"];
    if (![name length]) name = NSLocalizedString(@"(unknown)", nil);
    [row setObject:name forKey:@"displayName"];
    [row setObject:kRowTypeContact forKey:@"row_type"];
    /* Decoded lazily at draw time (see cachedAvatarForMid:). */
    NSString *cmid = [contact objectForKey:@"mid"];
    if (cmid) [row setObject:cmid forKey:@"avatar_mid"];
    [rows addObject:row];
  }

  return rows;
}

- (NSInteger)rowForChatId:(NSString *)chatId;
{
  NSUInteger i;
  NSInteger contactMatch = -1;
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return -1;
  /* Prefer the Chats-section row; fall back to the Friends row so a
   * never-messaged 1:1 stays highlighted until its chat row materializes. */
  for (i = 0; i < [rows_ count]; i++) {
    NSDictionary *row = [rows_ objectAtIndex:i];
    NSString *type = [row objectForKey:@"row_type"];
    if ([type isEqualToString:kRowTypeChat]) {
      if ([[row objectForKey:@"chatMid"] isEqualToString:chatId]) return (NSInteger)i;
    } else if (contactMatch < 0 && [type isEqualToString:kRowTypeContact]) {
      if ([[row objectForKey:@"mid"] isEqualToString:chatId]) contactMatch = (NSInteger)i;
    }
  }
  return contactMatch;
}

- (void)selectStoredChat;
{
  NSInteger row = [self rowForChatId:selectedChatId_];
  if (row < 0) {
    [tableView_ deselectAll:self];
    return;
  }
  [tableView_ selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
            byExtendingSelection:NO];
  [tableView_ scrollRowToVisible:row];
}

- (id)initWithEngine:(ENILAccount *)engine {
  if ((self = [super init])) {
    engine_ = [engine retain];
  }
  return self;
}

/* Lifecycle split: subview creation + delegate wiring + observers stay in
 * -viewDidLoad; all frame math lives in -viewDidLayout so a single function
 * answers "where does each subview go for current bounds?". The autoresize
 * masks keep the layout coherent between layout passes. */

- (void)viewDidLoad {
  [super viewDidLoad];

  scrollView_ = [[NSScrollView alloc] initWithFrame:NSZeroRect];
  [scrollView_ setHasVerticalScroller:YES];
  [scrollView_ setHasHorizontalScroller:NO];
  [scrollView_ setAutohidesScrollers:YES];
  [scrollView_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [scrollView_ setBorderType:NSNoBorder];
  /* Pre-10.6 the window backing is uninitialized (black) and NSView/NSClipView
   * with drawsBackground:NO leaves the region unpainted, so the table sits
   * over a black hole. Vibrancy doesn't exist that far back anyway. The 10.6
   * floor sits inside the Legacy tier, so under the three-tier policy we gate
   * the transparent look at Middle and accept the opaque backing on Legacy. */
  if (AICCCurrentTier() >= AICCTierMiddle) {
    [scrollView_ setDrawsBackground:NO];
    [[scrollView_ contentView] setDrawsBackground:NO];
  }
  /* We drive content insets by hand in -viewDidLayout (top = toolbar, bottom
   * = status bar), so opt out of AppKit's automatic adjustment — otherwise it
   * overwrites the manual insets. No-op on Legacy, which keeps a carved frame. */
  [scrollView_ XP_setAutomaticallyAdjustsContentInsets:NO];

  tableView_ = [[ENILChatTableView alloc] initWithFrame:NSZeroRect];
  [tableView_ setAutoresizingMask:NSViewWidthSizable];
  [tableView_ setDataSource:self];
  [tableView_ setDelegate:self];
  [tableView_ setRowHeight:kRowHeight];
  [tableView_ setUsesAlternatingRowBackgroundColors:NO];
  if (AICCCurrentTier() >= AICCTierMiddle)
    [tableView_ setBackgroundColor:[NSColor clearColor]];
  [tableView_ setHeaderView:nil];
  [tableView_ XP_setSourceListStyle];
  [tableView_ XP_setFloatsGroupRows:YES];
  [tableView_ setFocusRingType:NSFocusRingTypeNone];
  [scrollView_ setFocusRingType:NSFocusRingTypeNone];

  NSTableColumn *col = [[[NSTableColumn alloc]
    initWithIdentifier:@"list"] autorelease];
  [col setDataCell:[[[ENILListCell alloc] init] autorelease]];
  [tableView_ addTableColumn:col];

  [scrollView_ setDocumentView:tableView_];
  [[scrollView_ contentView] setPostsFrameChangedNotifications:YES];
  [[NSNotificationCenter defaultCenter] addObserver:self
    selector:@selector(clipViewFrameChanged:)
        name:NSViewFrameDidChangeNotification
      object:[scrollView_ contentView]];

  statusController_ = [[StatusViewController alloc] initWithAccount:engine_];
  /* AI_addChildViewController: wires the responder chain on every tier —
   * see MessageListViewController for the rationale. */
  [self AI_addChildViewController:statusController_];
  NSView *bar = [statusController_ view];
  [bar setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];

  [[self view] addSubview:scrollView_];
  [[self view] addSubview:bar];

  [self reloadData];
  [tableView_ sizeLastColumnToFit];
}

- (void)viewDidLayout {
  [super viewDidLayout];
  if (!scrollView_) return;
  NSRect bounds = [[self view] bounds];
  CGFloat w = bounds.size.width;
  CGFloat h = bounds.size.height;
  if (AICCCurrentTier() >= AICCTierMiddle) {
    /* Edge-to-edge on all four pane edges: the scroll view fills the whole
     * pane (including up under the toolbar and down behind the status bar) so
     * AppKit drives the window toolbar's scroll-under/separator behaviour from
     * a real full-size content view. The FRAME never carves out the bars —
     * manual content insets do: top clears the titlebar/toolbar, bottom clears
     * the status bar that floats over the lower strip. */
    CGFloat top = [[[self view] window] XP_titlebarHeight];
    [scrollView_ setFrame:NSMakeRect(0, 0, w, h)];
    [scrollView_ XP_setContentInsetsTop:top left:0 bottom:kBarHeight right:0];
  } else {
    /* Legacy: unchanged — the frame itself carves out the status bar strip. */
    [scrollView_ setFrame:NSMakeRect(0, kBarHeight, w, h - kBarHeight)];
  }
  [[statusController_ view] setFrame:NSMakeRect(0, 0, w, kBarHeight)];
}

- (void)reloadData {
  [rows_ autorelease];
  rows_ = [[self buildRows] retain];
  [tableView_ reloadData];
  [self selectStoredChat];
}

/* Surgical single-chat refresh for live SSE updates: re-reads ONE chat summary
 * and re-seats its row, instead of -reloadData's full rebuild (which re-queries
 * profile + every contact + every chat on every incoming message). The Chats
 * section is sorted lastDeliveredTime DESC, so we drop the chat's old row and
 * re-insert it at the position that keeps that order — a new message floats to
 * the top; a read receipt (unchanged timestamp) lands back in place. A nil
 * summary means the chat lost its message box (deleted) and just drops out.
 * Falls back to a full -reloadData only for the unexpected (model not built
 * yet, or section layout not the expected Account/Chats/Friends shape). */
- (void)updateChatId:(NSString *)chatId {
  NSMutableArray *mut;
  NSDictionary *summary;
  NSUInteger i, sect = 0, chatsHeader = NSNotFound, scan;

  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return;
  if (!rows_) { [self reloadData]; return; }

  mut = [[rows_ mutableCopy] autorelease];

  /* Locate the Chats section header — the 2nd of three headers built by
   * -buildRows in fixed order: Account, Chats, Friends. */
  for (i = 0; i < [mut count]; i++) {
    if ([[[mut objectAtIndex:i] objectForKey:@"row_type"]
          isEqualToString:kRowTypeSection]) {
      if (++sect == 2) { chatsHeader = i; break; }
    }
  }
  if (chatsHeader == NSNotFound) { [self reloadData]; return; }

  /* Drop the chat's existing row, if any, scanning only the contiguous Chats
   * section (stops at the Friends header). */
  scan = chatsHeader + 1;
  while (scan < [mut count]) {
    NSDictionary *r = [mut objectAtIndex:scan];
    if (![[r objectForKey:@"row_type"] isEqualToString:kRowTypeChat]) break;
    if ([[r objectForKey:@"chatMid"] isEqualToString:chatId]) {
      [mut removeObjectAtIndex:scan];
      continue; /* row shifted into scan; re-test without advancing */
    }
    scan++;
  }

  /* Re-insert at the sorted position (skip rows newer-or-equal, insert before
   * the first older one). nil summary => chat is gone; removal above suffices. */
  summary = [engine_ chatSummaryForId:chatId];
  if (summary) {
    NSMutableDictionary *row = chatRowFromSummary(summary);
    long long ts = [[row objectForKey:@"lastDeliveredTime"] longLongValue];
    NSUInteger insertAt = chatsHeader + 1;
    while (insertAt < [mut count]) {
      NSDictionary *r = [mut objectAtIndex:insertAt];
      if (![[r objectForKey:@"row_type"] isEqualToString:kRowTypeChat]) break;
      if ([[r objectForKey:@"lastDeliveredTime"] longLongValue] < ts) break;
      insertAt++;
    }
    [mut insertObject:row atIndex:insertAt];
  }

  [rows_ autorelease];
  rows_ = [mut copy]; /* immutable snapshot, matching -reloadData's contract */
  [tableView_ reloadData];
  [self selectStoredChat];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
  (void)tableView;
  return (NSInteger)[rows_ count];
}

- (id)tableView:(NSTableView *)tableView
    objectValueForTableColumn:(NSTableColumn *)tableColumn
                          row:(NSInteger)row {
  (void)tableView; (void)tableColumn;
  return [rows_ objectAtIndex:(NSUInteger)row];
}

- (BOOL)tableView:(NSTableView *)tableView isGroupRow:(NSInteger)row;
{
  (void)tableView;
  if (row < 0 || row >= (NSInteger)[rows_ count]) return NO;
  NSDictionary *rowData = [rows_ objectAtIndex:(NSUInteger)row];
  return [[rowData objectForKey:@"row_type"] isEqualToString:kRowTypeSection];
}

- (CGFloat)tableView:(NSTableView *)tableView heightOfRow:(NSInteger)row;
{
  (void)tableView;
  if (row >= 0 && row < (NSInteger)[rows_ count]) {
    NSDictionary *rowData = [rows_ objectAtIndex:(NSUInteger)row];
    if ([[rowData objectForKey:@"row_type"] isEqualToString:kRowTypeSection])
      return kRowHeight / 2.0;
  }
  return kRowHeight;
}

- (BOOL)tableView:(NSTableView *)tableView shouldSelectRow:(NSInteger)row;
{
  (void)tableView;
  NSDictionary *rowData;
  if (row < 0 || row >= (NSInteger)[rows_ count]) return NO;
  rowData = [rows_ objectAtIndex:(NSUInteger)row];
  return selectableMid(rowData) != nil;
}

- (void)setDelegate:(id<ChatListViewControllerDelegate>)delegate;
{
  delegate_ = delegate;
}

- (void)selectChatId:(NSString *)chatId;
{
  if (chatId == selectedChatId_) return;
  [selectedChatId_ autorelease];
  selectedChatId_ = [chatId copy];
  [self selectStoredChat];
}

- (void)clearSelection;
{
  [selectedChatId_ autorelease];
  selectedChatId_ = nil;
  [tableView_ deselectAll:self];
}

- (NSString *)selectedChatId;
{
  return selectedChatId_;
}

- (NSMenu *)contextMenuForRow:(NSInteger)row;
{
  NSDictionary *rowData;
  NSMenuItem *del;
  BOOL isChat;
  if (row < 0 || row >= (NSInteger)[rows_ count]) return nil;
  rowData = [rows_ objectAtIndex:(NSUInteger)row];
  isChat  = [[rowData objectForKey:@"row_type"]
              isEqualToString:kRowTypeChat];

  if (!contextMenu_) {
    contextMenu_ = [[NSMenu alloc] initWithTitle:@""];
    /* We set enabled state explicitly per right-clicked row. */
    [contextMenu_ setAutoenablesItems:NO];
    NSMenuItem *item = [[[NSMenuItem alloc]
      initWithTitle:NSLocalizedString(@"Delete", nil)
             action:@selector(enilDeleteChat:)
      keyEquivalent:@""] autorelease];
    [contextMenu_ addItem:item];
  }

  /* "Delete" is enabled only for chat rows; Account, Friends and section
     rows show it disabled. The target chat travels in representedObject. */
  del = [contextMenu_ itemAtIndex:0];
  [del setEnabled:isChat];
  if (isChat) {
    NSString *cid  = [rowData objectForKey:@"chatMid"];
    NSString *name = [rowData objectForKey:@"displayName"];
    [del setRepresentedObject:[NSDictionary dictionaryWithObjectsAndKeys:
      cid,                          ENILResponderPayloadChatIdKey,
      (name ? name : cid),          ENILResponderPayloadDisplayNameKey,
      nil]];
  } else {
    [del setRepresentedObject:nil];
  }
  return contextMenu_;
}

- (void)chatRowClicked:(id)sender;
{
  (void)sender;
  NSInteger clicked  = [tableView_ clickedRow];
  NSInteger selected = [tableView_ selectedRow];
  NSDictionary *rowData;
  NSString *chatId, *name;

  if (clicked >= 0 && clicked < (NSInteger)[rows_ count]) {
    rowData = [rows_ objectAtIndex:(NSUInteger)clicked];
    if (!selectableMid(rowData)) return;
  }

  if (selected < 0 || selected >= (NSInteger)[rows_ count]) {
    [selectedChatId_ autorelease];
    selectedChatId_ = nil;
    NSDictionary *payload = [NSDictionary dictionary];
    if ([[self nextResponder] tryToPerform:@selector(enilSelectChat:) with:payload]) return;
    if (delegate_) [delegate_ chatSelected:nil displayName:nil];
    return;
  }

  rowData = [rows_ objectAtIndex:(NSUInteger)selected];
  chatId  = selectableMid(rowData);
  if (!chatId) return;
  name = [rowData objectForKey:@"displayName"];
  /* Friends-section rows are gated behind a confirmation sheet; existing
     chats from the Chats section open directly. Don't commit the selection
     here — enil_openChatId: applies it on confirm, clearSelection on cancel. */
  BOOL fromFriends = [[rowData objectForKey:@"row_type"]
                       isEqualToString:kRowTypeContact];
  NSDictionary *payload = [NSDictionary dictionaryWithObjectsAndKeys:
    chatId, ENILResponderPayloadChatIdKey,
    name, ENILResponderPayloadDisplayNameKey,
    [NSNumber numberWithBool:fromFriends], ENILResponderPayloadFromFriendsKey,
    nil];
  if ([[self nextResponder] tryToPerform:@selector(enilSelectChat:) with:payload]) return;
  if (delegate_) [delegate_ chatSelected:chatId displayName:name];
}

- (void)clipViewFrameChanged:(NSNotification *)notification;
{
  (void)notification;
  [tableView_ sizeLastColumnToFit];
}

- (void)tableViewSelectionDidChange:(NSNotification *)notification;
{
  (void)notification;
  [self chatRowClicked:nil];
}

- (void)dealloc;
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [scrollView_ release];
  [tableView_ release];
  [rows_ release];
  [selectedChatId_ release];
  [contextMenu_ release];
  [avatarCache_ release];
  [engine_ release];
  [statusController_ release];
  [super dealloc];
}

@end
