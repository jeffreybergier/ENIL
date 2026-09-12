#import "ChatWindowController.h"
#import "ENILResponderActions.h"
#import "ENILAccount.h"
#import "MessageListViewController.h"
#import "MessageSendViewController.h"
#import "AIFontAwesome.h"
#import "XPFoundation.h"

NSString * const ENILResponderPayloadChatIdKey      = @"chat_id";
NSString * const ENILResponderPayloadDisplayNameKey = @"display_name";
NSString * const ENILResponderPayloadTextKey        = @"text";
NSString * const ENILResponderPayloadResourcesKey   = @"resources";
NSString * const ENILResponderPayloadPackageIdKey   = @"package_id";
NSString * const ENILResponderPayloadStickerIdKey   = @"sticker_id";
NSString * const ENILResponderPayloadIsSticonKey    = @"is_sticon";
NSString * const ENILResponderPayloadImagePathKey   = @"image_path";
NSString * const ENILResponderPayloadAltTextKey     = @"alt_text";
NSString * const ENILResponderPayloadFromFriendsKey = @"from_friends";

static NSString * const kENILTbMoreMessages         = @"ENILTbMoreMessages";
static NSString * const kENILTbLeaveChat            = @"ENILTbLeaveChat";
static NSString * const kENILTbDeleteChat           = @"ENILTbDeleteChat";
static NSString * const kENILTbPickerMode           = @"ENILTbPickerMode";

/* Picker toolbar segments. "None" collapses the inspector and preserves the
 * last sticker/sticon mode for the next time the user opens it. "Photo" is
 * a transient action (opens a sheet); after the sheet ends the segment is
 * restored to the previous inspector-derived state via enil_syncPickerSegment. */
enum {
  kPickerSegPhoto    = 0,
  kPickerSegStickers = 1,
  kPickerSegEmoji    = 2,
  kPickerSegNone     = 3
};

static BOOL enil_chatIdEqual(NSString *a, NSString *b)
{
  if (a == b) return YES;
  if (!a || !b) return NO;
  return [a isEqualToString:b];
}

@interface ChatWindowController ()
- (void)enil_updateWindowTitleAndSubtitle;
- (void)enil_handleDismissSheetSender:(id)sender;
- (void)enil_handleStartSync;
- (void)enil_handleSelectChatPayload:(NSDictionary *)payload;
- (void)enil_confirmOpenChatId:(NSString *)chatId displayName:(NSString *)name;
- (void)enil_openChatId:(NSString *)chatId displayName:(NSString *)name;
- (void)enil_handlePickStickerPayload:(NSDictionary *)payload;
- (void)enil_handleSendMessagePayload:(NSDictionary *)payload;
- (void)enil_handleSendImagePayload:(NSDictionary *)payload;
- (NSString *)enil_activeChatIdIfConsistent;
- (NSString *)enil_activeChatIdForSendPayload:(NSDictionary *)payload;
- (void)enil_handleCloseChat;
- (void)enil_handleLeaveChat;
- (void)enil_confirmDeleteChatId:(NSString *)chatId displayName:(NSString *)name;
- (void)enilMarkChatSeen:(id)sender;
- (void)enilLoadMoreMessages:(id)sender;
- (void)enilCloseChat:(id)sender;
- (void)enilLeaveChat:(id)sender;
- (void)enilSendCurrentMessage:(id)sender;
- (void)enilLogOut:(id)sender;
- (void)enilReauthenticate:(id)sender;
- (BOOL)enil_activeChatIsLeavable;
- (BOOL)enil_isLoggedIn;
- (void)pickerModeSegmentChanged:(id)sender;
- (void)enil_inspectorDidBecomeVisible;
- (void)enil_syncPickerSegment;
- (void)enil_toolbarDidRemoveItem:(NSNotification *)note;
- (NSMenuItem *)enil_attachmentMenuFormRepresentation;
- (void)enilImagePickerSheetDidEnd:(id)sender;
- (void)enilHideAttachments:(id)sender;
- (void)enilShowStickers:(id)sender;
- (void)enilShowEmoji:(id)sender;
- (void)enilAttachPhoto:(id)sender;
@end

#pragma mark - ChatWindowController Implementation

@implementation ChatWindowController

- (id)initWithEngine:(ENILAccount *)engine;
{
  NSString *accountId = [[engine enilDir] lastPathComponent];
  /* Cookie cutter prefixes this with "AICCWindow-" for the frame autosave
   * key, so the final NSWindow autosave name is "AICCWindow-<accountId>".
   * Pre-migration users had "ENILWindow-<accountId>" — one-time loss of saved
   * window position on first launch after the migration. */
  if ((self = [super initWithTitle:@"ENIL" autosaveName:accountId])) {
    engine_ = [engine retain];

    /* The three pane VCs MUST be set before the window loads (cookie-cutter
     * contract). Constructing them here in init satisfies that — the setters
     * only store, and -windowDidLoad reads them later when building the
     * split view. */
    chatsController_    = [[ChatListViewController    alloc] initWithEngine:engine_];
    messagesController_ = [[MessageListViewController alloc] initWithEngine:engine_];
    stickerController_  = [[StickerViewController alloc] initWithEngine:engine_];

    [chatsController_   setDelegate:self];
    [stickerController_ setDelegate:self];

    [self setSidebarViewController:  chatsController_];
    [self setDetailViewController:   messagesController_];
    [self setInspectorViewController:stickerController_];

    [self setSidebarWidthLimits:AIMinMidMaxMake(160.0, 240.0, 320.0)];
    [self setInspectorWidthLimits:AIMinMidMaxMake(160.0, 280.0, 512.0)];
    /* Per-account split-divider persistence (same key shape as pre-migration
     * "ENILSplit-<accountId>"). One name covers both dividers — the cookie
     * cutter routes it to whichever NSSplitView the active tier built. */
    [self setSplitViewAutosaveName:
      [@"ENILSplit-" stringByAppendingString:accountId]];
  }
  return self;
}

- (void)windowDidLoad;
{
  /* Base builds the window + three-pane split (NSSplitViewController on
   * Middle/Modern, manual AISplitView on Legacy). After this returns the
   * sidebar/detail/inspector views are alive and in the view hierarchy. */
  [super windowDidLoad];

  /* Toolbar — owned by the subclass per the cookie cutter's "base installs
   * no toolbar" contract. NSToolbar has been around since 10.0 so this is
   * unconditional. */
  {
    NSToolbar *tb = [[[NSToolbar alloc]
      initWithIdentifier:@"ENILMainToolbar-v3"] autorelease];
    [tb setDelegate:self];
    [tb setAllowsUserCustomization:YES];
    /* Default to icons only. Set BEFORE -setToolbar: so the value is the
     * initial default for a fresh install; after setAutosavesConfiguration:YES
     * + setToolbar:, any prior user choice from "Customize Toolbar…" wins. */
    [tb setDisplayMode:NSToolbarDisplayModeIconOnly];
    [tb setAutosavesConfiguration:YES];
    [[self window] setToolbar:tb];
    [self aiSetUnifiedToolbarStyle];
    if ([[tb items] count] == 0) {
      NSArray *idents = [self toolbarDefaultItemIdentifiers:tb];
      NSUInteger i;
      for (i = 0; i < [idents count]; i++)
        [tb insertItemWithItemIdentifier:[idents objectAtIndex:i]
                                 atIndex:(NSInteger)i];
    }
  }

  /* Multi-account: ENILSSEEventNotification is posted with object:<engine>,
     so scope this observer to engine_ — otherwise another account's incoming
     message would reload THIS window's chat list. ENILSyncDidFinish is posted
     object:nil from the engine-agnostic C progress bridge (enil_cocoa.m,
     which has no ENILAccount handle), so it can't be object-scoped without
     threading account identity through enil_progress_post; left global. The
     fallout is benign — a sibling account's sync-finish triggers a redundant
     re-query against THIS window's own engine_, never cross-account data. */
  [[NSNotificationCenter defaultCenter] addObserver:self
    selector:@selector(syncDidFinish:)
        name:ENILSyncDidFinishNotification
      object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
    selector:@selector(sseEvent:)
        name:ENILSSEEventNotification
      object:engine_];
  /* Invalidate pickerSegmented_ when the picker toolbar item is dragged out
   * of the toolbar via the customization sheet — otherwise the weak ref
   * dangles into a deallocated NSSegmentedControl. Re-populated by the next
   * factory call with flag==YES when (if) the user drags it back in. */
  [[NSNotificationCenter defaultCenter] addObserver:self
    selector:@selector(enil_toolbarDidRemoveItem:)
        name:NSToolbarDidRemoveItemNotification
      object:[[self window] toolbar]];

  [engine_ startSSE];

  /* First paint — no chat selected yet, so this renders "ENIL" + the
   * account display name (empty until the profile sync lands), and points
   * the title-bar proxy icon at session.json. */
  [self enil_updateWindowTitleAndSubtitle];

  /* If NSSplitView restored the inspector open from its autosave, the pane is
   * visible but the picker never left …None (no toolbar/menu reveal ran), so
   * it would paint blank until the user switched segments. Apply the same
   * default-and-render tail a reveal would. No-op when restored collapsed. */
  [self enil_inspectorDidBecomeVisible];
}

/* Wraps the inherited toggle so the sticker pane re-renders when it becomes
 * visible. reloadActiveTabIfDirty is a no-op when nothing changed since the
 * last show, so this is cheap. Also re-syncs the picker toolbar segment so
 * an external trigger (menu / keyboard / -[NSSplitViewItem setCollapsed:])
 * still leaves "None vs Stickers vs Emoji" matching the visible state. */
- (void)toggleInspector:(id)sender;
{
  [super toggleInspector:sender];
  [self enil_inspectorDidBecomeVisible];
}

/* Shared "the inspector is now visible" tail. Two callers reach an open
 * inspector without having picked a tab: a bare -toggleInspector: (menu /
 * keyboard, not Show Stickers / Emoji) and -windowDidLoad when NSSplitView
 * restores the pane open from its autosave. Both leave the picker in its
 * never-loaded …None state, so the WebView renders nothing; default to
 * stickers so the pane is never blank, flush the lazy re-render, and snap the
 * toolbar segment to match. No-op while collapsed — normal reveals set the
 * mode before reaching here, and a collapse skips the body and only re-syncs
 * the segment. */
- (void)enil_inspectorDidBecomeVisible;
{
  if (![self isInspectorCollapsed]) {
    if (![stickerController_ hasSelectedMode]) [stickerController_ setSticonMode:NO];
    [stickerController_ reloadActiveTabIfDirty];
  }
  [self enil_syncPickerSegment];
}

#pragma mark - Sheet Actions

static const CGFloat kSheetBtnH   = 32.0;
static const CGFloat kSheetBtnW   = 80.0;
static const CGFloat kSheetBtnPad = 12.0;

- (NSWindow *)makeSheetWithContentView:(NSView *)contentView
                                 title:(NSString *)title;
{
  NSRect   cvFrame  = [contentView frame];
  CGFloat  totalH   = cvFrame.size.height + kSheetBtnH + kSheetBtnPad * 2;
  NSRect   sheetRect = NSMakeRect(0, 0, cvFrame.size.width, totalH);

  NSWindow *sheet = [[[NSWindow alloc]
    initWithContentRect:sheetRect
              styleMask:XPWindowStyleMaskTitled
                backing:NSBackingStoreBuffered
                  defer:NO] autorelease];
  [sheet setTitle:title];
  [sheet setReleasedWhenClosed:NO];

  [contentView setFrameOrigin:NSMakePoint(0, kSheetBtnH + kSheetBtnPad * 2)];
  [[sheet contentView] addSubview:contentView];

  NSButton *done = [[[NSButton alloc]
    initWithFrame:NSMakeRect(cvFrame.size.width - kSheetBtnPad - kSheetBtnW,
                             kSheetBtnPad, kSheetBtnW, kSheetBtnH)] autorelease];
  [done setTitle:NSLocalizedString(@"Done", nil)];
  [done setBezelStyle:XPBezelStyleRounded];
  [done setTarget:self];
  [done setAction:@selector(enilDismissSheet:)];
  [[sheet contentView] addSubview:done];

  return sheet;
}
#pragma mark - ENILResponderActions

- (void)enilDismissSheet:(id)sender;
{
  [self enil_handleDismissSheetSender:sender];
}

- (void)enilStartSync:(id)sender;
{
  (void)sender;
  [self enil_handleStartSync];
}

/* Bottom-bar link toggle: flip the persisted SSE preference. The engine
   stops/starts the stream and announces the new state, which the
   SyncMiniViewController observes (via its queue) to repaint its glyph. */
- (void)enilToggleSSE:(id)sender;
{
  (void)sender;
  [engine_ setSSEEnabled:![engine_ isSSEEnabled]];
}

- (void)enilSelectChat:(id)sender;
{
  if (![sender isKindOfClass:[NSDictionary class]]) return;
  [self enil_handleSelectChatPayload:(NSDictionary *)sender];
}

/* Single delete command. Reused by the toolbar button, the chat-list
   context menu, and (later) the main menu. A menu item carries the target
   chat in its representedObject (a {chatId, displayName} dict); the toolbar
   item carries nothing, so we fall back to the open chat. */
- (void)enilDeleteChat:(id)sender;
{
  NSString *chatId = nil, *name = nil;
  if ([sender respondsToSelector:@selector(representedObject)]) {
    id ro = [sender representedObject];
    if ([ro isKindOfClass:[NSDictionary class]]) {
      chatId = [ro objectForKey:ENILResponderPayloadChatIdKey];
      name   = [ro objectForKey:ENILResponderPayloadDisplayNameKey];
    }
  }
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) {
    chatId = [self enil_activeChatIdIfConsistent];
    name   = activeDisplayName_;
  }
  if (![chatId length]) return;
  [self enil_confirmDeleteChatId:chatId displayName:(name ? name : chatId)];
}

- (void)enilPickSticker:(id)sender;
{
  if (![sender isKindOfClass:[NSDictionary class]]) return;
  [self enil_handlePickStickerPayload:(NSDictionary *)sender];
}

- (void)enilSendMessage:(id)sender;
{
  if (![sender isKindOfClass:[NSDictionary class]]) return;
  [self enil_handleSendMessagePayload:(NSDictionary *)sender];
}

- (void)enilSendImage:(id)sender;
{
  if (![sender isKindOfClass:[NSDictionary class]]) return;
  [self enil_handleSendImagePayload:(NSDictionary *)sender];
}

#pragma mark - Action Handlers

- (void)enil_handleDismissSheetSender:(id)sender;
{
  NSWindow *sheet = [(NSControl *)sender window];
  [NSApp endSheet:sheet];
  [sheet orderOut:nil];
}

#pragma mark - StickerViewControllerDelegate

- (void)stickerViewController:(StickerViewController *)vc
               didSelectPackageId:(NSString *)packageId
                        stickerId:(NSString *)stickerId
                         isSticon:(BOOL)isSticon;
{
  (void)vc;
  NSDictionary *payload = [NSDictionary dictionaryWithObjectsAndKeys:
    packageId, ENILResponderPayloadPackageIdKey,
    stickerId, ENILResponderPayloadStickerIdKey,
    [NSNumber numberWithBool:isSticon], ENILResponderPayloadIsSticonKey,
    nil];
  [self enil_handlePickStickerPayload:payload];
}

#pragma mark - ChatListViewControllerDelegate

- (void)chatSelected:(NSString *)chatId displayName:(NSString *)name;
{
  NSDictionary *payload = [NSDictionary dictionaryWithObjectsAndKeys:
    chatId, ENILResponderPayloadChatIdKey,
    name, ENILResponderPayloadDisplayNameKey,
    nil];
  [self enil_handleSelectChatPayload:payload];
}

- (void)enil_handleStartSync;
{
  [engine_ startSync];
}

- (void)enil_handleSelectChatPayload:(NSDictionary *)payload;
{
  NSString *chatId = [payload objectForKey:ENILResponderPayloadChatIdKey];
  NSString *name = [payload objectForKey:ENILResponderPayloadDisplayNameKey];
  if (chatId && (![chatId isKindOfClass:[NSString class]] || ![chatId length])) return;
  if (![name isKindOfClass:[NSString class]] || ![name length]) name = chatId;

  /* Only Friends-section clicks are confirmed via a sheet. Existing chats
     picked from the Chats section open directly, as do clear-selection. */
  if ([[payload objectForKey:ENILResponderPayloadFromFriendsKey] boolValue]) {
    [self enil_confirmOpenChatId:chatId displayName:name];
    return;
  }
  [self enil_openChatId:chatId displayName:name];
}

- (void)enil_confirmOpenChatId:(NSString *)chatId displayName:(NSString *)name;
{
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return;
  if (![name isKindOfClass:[NSString class]] || ![name length]) name = chatId;
  BOOL exists = [engine_ chatExistsForMid:chatId];
  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:[NSString stringWithFormat:
    NSLocalizedString(exists ? @"Open chat with \"%@\"?"
                             : @"Start a new chat with \"%@\"?", nil),
    name]];
  if (!exists)
    [alert setInformativeText:NSLocalizedString(
      @"A 1:1 chat will be created when you send your first message.", nil)];
  [alert addButtonWithTitle:NSLocalizedString(exists ? @"Open Chat" : @"Start Chat", nil)];
  [alert addButtonWithTitle:NSLocalizedString(@"Cancel", nil)];

  /* Ownership of the payload transfers to the didEnd callback. */
  NSDictionary *ctx = [[NSDictionary dictionaryWithObjectsAndKeys:
    chatId, ENILResponderPayloadChatIdKey,
    name,   ENILResponderPayloadDisplayNameKey, nil] retain];
  [alert XP_beginSheetModalForWindow:[self window]
                       modalDelegate:self
                      didEndSelector:@selector(enil_openChatAlertDidEnd:returnCode:contextInfo:)
                         contextInfo:(void *)ctx];
}

- (void)enil_openChatAlertDidEnd:(NSAlert *)alert
                      returnCode:(NSInteger)returnCode
                     contextInfo:(void *)contextInfo;
{
  NSDictionary *ctx = [(NSDictionary *)contextInfo autorelease];
  [[alert window] orderOut:nil];
  if (returnCode != NSAlertFirstButtonReturn) {
    /* Cancelled — clear any row selection in the list. */
    [chatsController_ clearSelection];
    return;
  }
  [self enil_openChatId:[ctx objectForKey:ENILResponderPayloadChatIdKey]
            displayName:[ctx objectForKey:ENILResponderPayloadDisplayNameKey]];
}

- (void)enil_openChatId:(NSString *)chatId displayName:(NSString *)name;
{
  [activeChatId_ autorelease];
  activeChatId_ = [chatId copy];
  [activeDisplayName_ autorelease];
  activeDisplayName_ = [name copy];

  [self enil_updateWindowTitleAndSubtitle];
  [chatsController_ selectChatId:activeChatId_];
  [messagesController_ reloadWithChatId:activeChatId_ displayName:activeDisplayName_];
  /* Refresh the toolbar Photo segment — it disables when no chat is active. */
  [self enil_syncPickerSegment];
}

/* Single source of truth for the window's title bar:
 *   title       : selected chat's display name, or "ENIL" if no chat.
 *   subtitle    : lastDeliveredTime of the selected chat formatted as a
 *                 short date/time, or the account's profile display name
 *                 when no chat is selected.
 *   representedURL: this account's session.json, so the proxy icon points
 *                 at the on-disk session blob (drag-out works for inspection).
 *
 * NSDateFormatter allocation is heavy (ICU + locale parse on first use),
 * and this helper runs on every SSE event for the active chat, so cache the
 * formatter as a function-local static. Title-bar updates only happen on the
 * main thread, so the lazy init does not need dispatch_once. Caveat: the
 * cached formatter snapshots [NSLocale currentLocale] at first use — a
 * runtime locale switch will only re-pick up after relaunch. Acceptable for
 * a title bar; we are not localising real content. */
- (void)enil_updateWindowTitleAndSubtitle;
{
  static NSDateFormatter *fmt = nil;
  NSWindow *w = [self window];
  NSURL    *sessionURL;

  if (!fmt) {
    fmt = [[NSDateFormatter alloc] init];
    /* Tiger-parity: 10.4 behavior is the only one available on PPC; on 10.5+
     * it's already the default but setting it explicitly makes the formatter
     * render identically across every tier. Matches ChatListViewController. */
    [fmt setFormatterBehavior:NSDateFormatterBehavior10_4];
    [fmt setDateStyle:NSDateFormatterMediumStyle];
    [fmt setTimeStyle:NSDateFormatterShortStyle];
  }

  sessionURL = [NSURL fileURLWithPath:
    [[engine_ enilDir] stringByAppendingPathComponent:@"session.json"]];
  [w XP_setRepresentedURL:sessionURL];

  if (activeChatId_ && [activeChatId_ length]) {
    NSString *title = (activeDisplayName_ && [activeDisplayName_ length])
                      ? activeDisplayName_ : activeChatId_;
    long long ms = [engine_ lastDeliveredTimeForChatId:activeChatId_];
    NSString *sub = (ms > 0)
      ? [fmt stringFromDate:
          [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)(ms / 1000.0)]]
      : @"";
    [w XP_setTitle:title subtitle:sub];
    return;
  }

  /* No selected chat — fall back to the account display name. ENILStructDict
     returns nil (not NSNull) for unset optionals, so kind-check before use. */
  {
    NSString *acct = [[engine_ profile] objectForKey:@"displayName"];
    if (![acct isKindOfClass:[NSString class]] || ![acct length]) acct = @"";
    [w XP_setTitle:@"ENIL" subtitle:acct];
  }
}

- (void)enil_handlePickStickerPayload:(NSDictionary *)payload;
{
  NSString *packageId = [payload objectForKey:ENILResponderPayloadPackageIdKey];
  NSString *stickerId = [payload objectForKey:ENILResponderPayloadStickerIdKey];
  NSNumber *isSticon = [payload objectForKey:ENILResponderPayloadIsSticonKey];

  if (![packageId isKindOfClass:[NSString class]] || ![packageId length]) return;
  if (![stickerId isKindOfClass:[NSString class]] || ![stickerId length]) return;

  if ([isSticon boolValue]) {
    NSString *altText = [payload objectForKey:ENILResponderPayloadAltTextKey];
    if (![altText isKindOfClass:[NSString class]]) altText = nil;
    [messagesController_ insertSticonWithPackageId:packageId
                                          sticonId:stickerId
                                           altText:altText];
    return;
  }

  NSString *chatId = [self enil_activeChatIdIfConsistent];
  if (!chatId) return;
  ENILLog(@"ChatWindowController.enilPickSticker", @"pkg=%@ id=%@ chat=%@",
        packageId, stickerId, chatId);
  [engine_ sendStickerId:stickerId packageId:packageId toChatId:chatId];
}

- (void)enil_handleSendMessagePayload:(NSDictionary *)payload;
{
  NSString *chatId = [self enil_activeChatIdForSendPayload:payload];
  NSString *text = [payload objectForKey:ENILResponderPayloadTextKey];
  NSArray *resources = [payload objectForKey:ENILResponderPayloadResourcesKey];

  if (!chatId) return;
  if (![text isKindOfClass:[NSString class]] || ![text length]) return;
  if (![resources isKindOfClass:[NSArray class]]) resources = nil;

  if ([resources count] > 0) {
    [engine_ sendInlineSticons:resources text:text toChatId:chatId];
    return;
  }
  [engine_ sendText:text toChatId:chatId];
}

- (void)enil_handleSendImagePayload:(NSDictionary *)payload;
{
  NSString *chatId = [self enil_activeChatIdForSendPayload:payload];
  NSString *path   = [payload objectForKey:ENILResponderPayloadImagePathKey];
  if (!chatId) return;
  if (![path isKindOfClass:[NSString class]] || ![path length]) return;
  [engine_ sendImageAtPath:path toChatId:chatId];
}

- (NSString *)enil_activeChatIdIfConsistent;
{
  NSString *active   = activeChatId_;
  NSString *shown    = [messagesController_ chatId];
  NSString *selected = [chatsController_ selectedChatId];

  if (!active || ![active length]) {
    ENILLog(@"ChatWindowController.sendGuard", @"ABORT: no active chat");
    NSBeep();
    return nil;
  }
  if (!enil_chatIdEqual(shown, active) || !enil_chatIdEqual(selected, active)) {
    ENILLog(@"ChatWindowController.sendGuard",
            @"ABORT: internal mismatch (active=%@ shown=%@ selected=%@)",
            active, shown, selected);
    NSBeep();
    return nil;
  }
  return active;
}

- (NSString *)enil_activeChatIdForSendPayload:(NSDictionary *)payload;
{
  NSString *active = [self enil_activeChatIdIfConsistent];
  if (!active) return nil;

  NSString *payloadChatId = [payload objectForKey:ENILResponderPayloadChatIdKey];
  if (!enil_chatIdEqual(payloadChatId, active)) {
    ENILLog(@"ChatWindowController.sendGuard", @"ABORT: payload mismatch "
          @"(payload=%@ active=%@)", payloadChatId, active);
    NSBeep();
    return nil;
  }
  return active;
}

- (void)syncDidFinish:(NSNotification *)note;
{
  (void)note;
  /* A sync may have downloaded new/updated avatar JPEGs — drop the cached
   * decodes so the next draw picks them up. SSE reloads (-sseEvent:) skip
   * this: they never touch avatar files, so the cache stays warm there. */
  [chatsController_ flushAvatarCache];
  [chatsController_ reloadData];
  /* reloadData only marks the picker dirty (lazy re-render). If the inspector
   * is already visible, freshly downloaded stickers/sticons won't repaint
   * until the next show/tab-switch — so flush it now, like -toggleInspector:. */
  [stickerController_ reloadData];
  if (![self isInspectorCollapsed])
    [stickerController_ reloadActiveTabIfDirty];
  [[[self window] toolbar] validateVisibleItems];
  /* Profile may have just landed (first sync) — refresh in case the
   * no-chat-selected fallback subtitle now has an account name to show. */
  [self enil_updateWindowTitleAndSubtitle];
}

- (void)sseEvent:(NSNotification *)note;
{
  NSDictionary *userInfo = [note userInfo];
  NSString *type = [userInfo objectForKey:@"type"];
  if ([type isEqualToString:@"ping"] ||
      [type isEqualToString:@"reconnect"] ||
      [type isEqualToString:@"connInfoRevision"]) return;

  if ([type isEqualToString:@"message"]) {
    /* A message event moves at most one chat row. The account layer surfaces
     * which chat changed order/unread as @"chat_id"; apply a surgical update
     * instead of rebuilding the whole roster (the old unconditional
     * -reloadData re-queried profile + every contact + every chat on every
     * incoming message). Reactions/deletes/peer-read carry no chat_id — no
     * list change — and are reflected in the open chat via its
     * ENILMessageJSNotification, so the list is left untouched here. */
    NSString *cid = [userInfo objectForKey:@"chat_id"];
    if ([cid length]) [chatsController_ updateChatId:cid];
    [[[self window] toolbar] validateVisibleItems];
    /* Subtitle tracks the active chat's lastDeliveredTime. Cheap single-row. */
    [self enil_updateWindowTitleAndSubtitle];
    return;
  }

  /* Non-message structural events (talkException, fullSync, …) are rare; a full
   * rebuild is acceptable and keeps every pane coherent. */
  [chatsController_ reloadData];
  [[[self window] toolbar] validateVisibleItems];
  [self enil_updateWindowTitleAndSubtitle];
  [messagesController_ reloadData];
  [stickerController_ reloadData];
  if (![self isInspectorCollapsed])
    [stickerController_ reloadActiveTabIfDirty];
}

- (void)dealloc;
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [engine_ stopSSE];
  [engine_ release];
  [chatsController_ release];
  [messagesController_ release];
  [stickerController_ release];
  [activeChatId_ release];
  [activeDisplayName_ release];
  [super dealloc];
}

#pragma mark - NSToolbar Delegate

- (NSArray *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar;
{
  (void)toolbar;
  return [NSArray arrayWithObjects:
    kENILTbMoreMessages,
    kENILTbLeaveChat,
    kENILTbDeleteChat,
    kENILTbPickerMode,
    XPToolbarSeparatorItemIdentifier,
    NSToolbarSpaceItemIdentifier,
    NSToolbarFlexibleSpaceItemIdentifier,
    nil];
}

- (NSArray *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar;
{
  (void)toolbar;
  return [NSArray arrayWithObjects:
    NSToolbarFlexibleSpaceItemIdentifier,
    kENILTbPickerMode,
    nil];
}

/* Menu-form representation for the custom-view Attachment item — used when the
 * item is shown text-only or clipped into the overflow menu. The four entries
 * dispatch the same sender-agnostic ENILResponderActions selectors the View
 * menu and the segmented control already use, so all three share one validated
 * code path (validateMenuItem: enables/disables each). Mirrors the segment
 * order: Photo, Stickers, Emoji, None. */
- (NSMenuItem *)enil_attachmentMenuFormRepresentation;
{
  NSMenuItem *rep = [[[NSMenuItem alloc]
    initWithTitle:NSLocalizedString(@"Attachment", nil)
           action:NULL
    keyEquivalent:@""] autorelease];
  NSMenu *sub = [[[NSMenu alloc] initWithTitle:@""] autorelease];
  [[sub addItemWithTitle:NSLocalizedString(@"Photo", nil)
                  action:@selector(enilAttachPhoto:)
           keyEquivalent:@""] setTarget:self];
  [[sub addItemWithTitle:NSLocalizedString(@"Stickers", nil)
                  action:@selector(enilShowStickers:)
           keyEquivalent:@""] setTarget:self];
  [[sub addItemWithTitle:NSLocalizedString(@"Emoji", nil)
                  action:@selector(enilShowEmoji:)
           keyEquivalent:@""] setTarget:self];
  [[sub addItemWithTitle:NSLocalizedString(@"None", nil)
                  action:@selector(enilHideAttachments:)
           keyEquivalent:@""] setTarget:self];
  [rep setSubmenu:sub];
  return rep;
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
     itemForItemIdentifier:(NSString *)identifier
 willBeInsertedIntoToolbar:(BOOL)flag;
{
  (void)toolbar;
  NSString *label = nil;
  AIFontAwesomeIcon  icon  = 0;
  AIFontAwesomeStyle style = AIFontAwesomeStyleSolid;

  /* Attachment item — custom NSSegmentedControl view, not a glyph button.
   * Factored before the FA-icon table because it doesn't share that shape. */
  if ([identifier isEqualToString:kENILTbPickerMode]) {
    NSSegmentedControl *seg = [[[NSSegmentedControl alloc]
      initWithFrame:NSZeroRect] autorelease];
    [seg setSegmentCount:4];
    /* setImage:forSegment: is 10.3+; XP_setToolTip:forSegment: is a no-op
     * on pre-10.13, so both can be called unconditionally. */
    CGFloat scale = [[self window] XP_backingScaleFactor];
    if (scale < 1.0) scale = 1.0;
    [seg setImage:[AIFontAwesome imageForIcon:AIFACertificate
                                      style:AIFontAwesomeStyleSolid
                                   iconSize:14.0
                                 canvasSize:20.0
                                      scale:scale]
       forSegment:kPickerSegStickers];
    [seg setImage:[AIFontAwesome imageForIcon:AIFAFaceGrin
                                      style:AIFontAwesomeStyleRegular
                                   iconSize:14.0
                                 canvasSize:20.0
                                      scale:scale]
       forSegment:kPickerSegEmoji];
    [seg setImage:[AIFontAwesome imageForIcon:AIFAImage
                                      style:AIFontAwesomeStyleRegular
                                   iconSize:14.0
                                 canvasSize:20.0
                                      scale:scale]
       forSegment:kPickerSegPhoto];
    [seg setImage:[AIFontAwesome imageForIcon:AIFACircleXmark
                                      style:AIFontAwesomeStyleRegular
                                   iconSize:14.0
                                 canvasSize:20.0
                                      scale:scale]
       forSegment:kPickerSegNone];
    [seg XP_setToolTip:NSLocalizedString(@"Stickers", nil) forSegment:kPickerSegStickers];
    [seg XP_setToolTip:NSLocalizedString(@"Emoji",    nil) forSegment:kPickerSegEmoji];
    [seg XP_setToolTip:NSLocalizedString(@"Photo",    nil) forSegment:kPickerSegPhoto];
    [seg XP_setToolTip:NSLocalizedString(@"None",     nil) forSegment:kPickerSegNone];
    [seg setTarget:self];
    [seg setAction:@selector(pickerModeSegmentChanged:)];
    [seg sizeToFit];

    NSToolbarItem *item = [[[NSToolbarItem alloc]
      initWithItemIdentifier:identifier] autorelease];
    [item setLabel:NSLocalizedString(@"Attachment", nil)];
    [item setPaletteLabel:NSLocalizedString(@"Attachment", nil)];
    [item setView:seg];
    /* NSToolbarItem with a custom view needs explicit size on every tier
     * (no auto-sizing pre-10.13). sizeToFit produces the natural width;
     * height matches the standard small-control footprint. */
    [item setMinSize:[seg frame].size];
    [item setMaxSize:[seg frame].size];
    /* A custom-view item has no built-in text/overflow representation: in the
     * clipped-items menu AppKit would otherwise synthesize one wired to this
     * item's action (pickerModeSegmentChanged:) and call it with an NSMenuItem
     * sender, crashing on -selectedSegment. Supply an explicit submenu. */
    [item setMenuFormRepresentation:[self enil_attachmentMenuFormRepresentation]];

    /* Track the live control only for the real toolbar; ignore the
     * customization-palette copy (flag == NO) so a stale palette view
     * doesn't shadow the active one. */
    if (flag) {
      pickerSegmented_ = seg;   /* weak; toolbar item owns it */
      [self enil_syncPickerSegment];
    }
    return item;
  }

  (void)flag;

  if ([identifier isEqualToString:kENILTbMoreMessages]) {
    label = NSLocalizedString(@"More Messages", nil);
    icon = AIFATruckRampBox;
  } else if ([identifier isEqualToString:kENILTbLeaveChat]) {
    label = NSLocalizedString(@"Leave Chat", nil);
    icon = AIFAPersonThroughWindow;
  } else if ([identifier isEqualToString:kENILTbDeleteChat]) {
    label = NSLocalizedString(@"Delete Chat", nil);
    icon = AIFATrashCan;
    style = AIFontAwesomeStyleRegular;
  }
  if (!label) return nil;

  NSToolbarItem *item = [[[NSToolbarItem alloc]
    initWithItemIdentifier:identifier] autorelease];
  [item setLabel:label];
  [item setPaletteLabel:label];
  /* 24pt glyph centred in a 32pt canvas = the regular toolbar slot, so
   * NSToolbar draws the image 1:1 (no rescale). Rasterised at the window's
   * backing scale (1.0 pre-HiDPI). Template image — AppKit tints. */
  [item setImage:[AIFontAwesome imageForIcon:icon
                                     style:style
                                  iconSize:24.0
                                canvasSize:32.0
                                     scale:[[self window] XP_backingScaleFactor]]];
  [item setTarget:self];
  [item setAction:@selector(toolbarItemClicked:)];
  return item;
}

/* Validation predicates — one source of truth shared by the toolbar
   (validateToolbarItem:) and the Chat menu (validateMenuItem:). */
- (BOOL)enil_hasActiveChat;
{
  return (activeChatId_ && [activeChatId_ length]) ? YES : NO;
}

- (BOOL)enil_canMarkActiveChatSeen;
{
  return ([self enil_hasActiveChat]
          && [engine_ unreadCountForChatId:activeChatId_] > 0) ? YES : NO;
}

- (BOOL)enil_canLoadMoreMessages;
{
  return [messagesController_ hasMoreMessages] ? YES : NO;
}

- (BOOL)enil_isLoggedIn;
{
  NSString *tok = [engine_ accessToken];
  return (tok && [tok length]) ? YES : NO;
}

- (BOOL)validateToolbarItem:(NSToolbarItem *)item;
{
  NSString *identifier = [item itemIdentifier];
  if ([identifier isEqualToString:kENILTbLeaveChat])
    return [self enil_activeChatIsLeavable];
  if ([identifier isEqualToString:kENILTbDeleteChat])
    return [self enil_hasActiveChat];
  if ([identifier isEqualToString:kENILTbMoreMessages])
    return [self enil_canLoadMoreMessages];
  return YES;
}

/* Chat-menu items dispatch the same actions as the toolbar through the
   responder chain (target == nil). Map each action selector back to the
   shared predicate so the menu and toolbar stay in lock-step. */
- (BOOL)validateMenuItem:(NSMenuItem *)item;
{
  SEL action = [item action];
  if (action == @selector(enilCloseChat:))        return [self enil_hasActiveChat];
  if (action == @selector(enilLeaveChat:))        return [self enil_activeChatIsLeavable];
  if (action == @selector(enilDeleteChat:))       return [self enil_hasActiveChat];
  if (action == @selector(enilMarkChatSeen:))     return [self enil_canMarkActiveChatSeen];
  if (action == @selector(enilLoadMoreMessages:)) return [self enil_canLoadMoreMessages];
  if (action == @selector(enilSendCurrentMessage:)) return [messagesController_ canSendCurrentMessage];
  if (action == @selector(enilLogOut:)) return [self enil_isLoggedIn];
  /* Reauthenticate is a recovery action — stays enabled even when the
     session is invalid/expired, which is exactly when it is needed. */
  if (action == @selector(enilReauthenticate:)) return YES;
  /* View ▸ Attachments group — each item disables when its click would be a
     no-op against the live picker state, so the menu mirrors the toolbar
     segment's selected/unselected feedback. */
  if (action == @selector(enilHideAttachments:))
    return ![self isInspectorCollapsed];
  if (action == @selector(enilShowStickers:))
    return ([self isInspectorCollapsed] || [stickerController_ isSticonMode]);
  if (action == @selector(enilShowEmoji:))
    return ([self isInspectorCollapsed] || ![stickerController_ isSticonMode]);
  /* Attach Photo opens a sheet against the active chat's send bar — pointless
     without a chat selected. Matches presentImagePicker's own bail check. */
  if (action == @selector(enilAttachPhoto:)) return [self enil_hasActiveChat];
  return YES;
}

- (void)toolbarItemClicked:(NSToolbarItem *)sender;
{
  NSString *identifier = [sender itemIdentifier];
  ENILLog(@"ChatWindowController.toolbarItemClicked", @"%@", identifier);
  if ([identifier isEqualToString:kENILTbLeaveChat]) {
    [self enil_handleLeaveChat];
    return;
  }
  if ([identifier isEqualToString:kENILTbDeleteChat]) {
    [self enilDeleteChat:sender];
    return;
  }
  if ([identifier isEqualToString:kENILTbMoreMessages]) {
    [self enilLoadMoreMessages:sender];
    return;
  }
}

/* Picker-mode toolbar segment changed. The 3 segments fold inspector
 * visibility (None ↔ collapsed) and picker mode (Stickers/Emoji) into one
 * control — so the action both drives the inspector collapse state AND
 * flips the picker mode in lockstep. Dispatches to the same ENILResponder-
 * Actions selectors the View menu items use, so menu and toolbar share one
 * code path. */
- (void)pickerModeSegmentChanged:(id)sender;
{
  /* Only the live NSSegmentedControl drives this action now — the Attachment
   * item's menuFormRepresentation routes overflow / text-only clicks to the
   * per-mode selectors directly. Guard anyway so a stray NSMenuItem sender can
   * never reach -selectedSegment (the unrecognized-selector crash). */
  if (![sender isKindOfClass:[NSSegmentedControl class]]) return;
  NSInteger seg = [(NSSegmentedControl *)sender selectedSegment];
  ENILLog(@"ChatWindowController.pickerModeSegmentChanged", @"seg=%ld",
        (long)seg);

  if (seg == kPickerSegNone)     { [self enilHideAttachments:sender]; return; }
  if (seg == kPickerSegPhoto)    { [self enilAttachPhoto:sender];     return; }
  if (seg == kPickerSegStickers) { [self enilShowStickers:sender];    return; }
  if (seg == kPickerSegEmoji)    { [self enilShowEmoji:sender];       return; }
}

/* Collapse the inspector if it's showing. No-op when already collapsed.
 * Toolbar Hide segment + View ▸ Hide Attachments both land here. */
- (void)enilHideAttachments:(id)sender;
{
  (void)sender;
  if (![self isInspectorCollapsed]) [self toggleInspector:nil];
}

/* Show the inspector in sticker mode. Set the mode BEFORE uncollapsing so
 * the inspector reveals the correct tab on first paint — toggleInspector:
 * ends by calling -reloadActiveTabIfDirty, which reads isSticonMode. No-op
 * when stickers are already showing. */
- (void)enilShowStickers:(id)sender;
{
  (void)sender;
  [stickerController_ setSticonMode:NO];
  if ([self isInspectorCollapsed]) [self toggleInspector:nil];
  else                             [self enil_syncPickerSegment];
}

- (void)enilShowEmoji:(id)sender;
{
  (void)sender;
  [stickerController_ setSticonMode:YES];
  if ([self isInspectorCollapsed]) [self toggleInspector:nil];
  else                             [self enil_syncPickerSegment];
}

/* Photo is a transient sheet action — do NOT change inspector state. The
 * toolbar segment will visually show "Photo" while the sheet is up; when
 * the sheet ends, MessageSendViewController fires enilImagePickerSheetDidEnd:
 * up the responder chain and we re-sync the segment from actual inspector
 * state. */
- (void)enilAttachPhoto:(id)sender;
{
  (void)sender;
  [messagesController_ presentImagePicker];
}

/* Responder-chain callback fired by MessageSendViewController.openPanelDidEnd:
 * regardless of whether the user picked a file. enil_syncPickerSegment derives
 * the correct segment from the actual inspector state, so the toolbar snaps
 * back to whatever was selected before Photo was clicked. */
- (void)enilImagePickerSheetDidEnd:(id)sender;
{
  (void)sender;
  [self enil_syncPickerSegment];
}

- (void)enil_toolbarDidRemoveItem:(NSNotification *)note;
{
  NSToolbarItem *item = [[note userInfo] objectForKey:@"item"];
  if (![item isKindOfClass:[NSToolbarItem class]]) return;
  if ([[item itemIdentifier] isEqualToString:kENILTbPickerMode])
    pickerSegmented_ = nil;
}

/* Bring the toolbar segment back in sync with the actual inspector +
 * picker-mode state. Cheap; safe to call whenever either of those changes.
 * No-op until the toolbar factory has built the live control. */
- (void)enil_syncPickerSegment;
{
  NSInteger want;
  if (!pickerSegmented_) return;
  if ([self isInspectorCollapsed])
    want = kPickerSegNone;
  else
    want = [stickerController_ isSticonMode] ? kPickerSegEmoji
                                             : kPickerSegStickers;
  [pickerSegmented_ setSelectedSegment:want];
  /* Photo segment is the only one that needs an active chat — go through
   * NSSegmentedCell since -setEnabled:forSegment: has lived there since
   * 10.3 (NSSegmentedControl's forwarder only became reliable later). */
  [(NSSegmentedCell *)[pickerSegmented_ cell]
      setEnabled:[self enil_hasActiveChat]
      forSegment:kPickerSegPhoto];
}

- (void)enilMarkChatSeen:(id)sender;
{
  (void)sender;
  NSString *chatId = [self enil_activeChatIdIfConsistent];
  if (!chatId) return;
  if ([engine_ unreadCountForChatId:chatId] <= 0) return;
  [engine_ markChatSeenAtServerHead:chatId];
  [[[self window] toolbar] validateVisibleItems];
}

/* Single command; reused by the HTML "Load earlier messages" button,
   the toolbar item, and (later) the main menu. */
- (void)enilLoadMoreMessages:(id)sender;
{
  (void)sender;
  [messagesController_ loadMoreMessages];
  [[[self window] toolbar] validateVisibleItems];
}

/* Menu/toolbar action wrappers — keep dispatch uniform with the rest of
   the ENILResponderActions family (enilDeleteChat:, enilMarkChatSeen:, …). */
- (void)enilCloseChat:(id)sender;
{
  (void)sender;
  [self enil_handleCloseChat];
}

- (void)enilLeaveChat:(id)sender;
{
  (void)sender;
  [self enil_handleLeaveChat];
}

/* "Send Message" (Cmd+Return). Forwarded down to the embedded send
   controller; validation mirrors the Send button's enabled state. */
- (void)enilSendCurrentMessage:(id)sender;
{
  (void)sender;
  [messagesController_ sendCurrentMessage];
}

/* "Log Out" (Accounts menu). Owned + validated here because logout is
   window/session-scoped. Confirmation UX mirrors Leave/Delete Chat; on
   confirm the teardown + folder removal is deferred to the AppDelegate
   (see enil_logOutAlertDidEnd:). No server RPC — LINE's chrome-extension
   protocol has no logout; sign-out is purely local. */
- (void)enilLogOut:(id)sender;
{
  (void)sender;
  if (![self enil_isLoggedIn]) return;

  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:NSLocalizedString(@"Log out of this account?", nil)];
  [alert setInformativeText:NSLocalizedString(
    @"All of this account's data on this device — messages, chats, and "
     "downloaded media — will be permanently deleted. This cannot be "
     "undone. You will need to scan a QR code to log in again.", nil)];
  [alert addButtonWithTitle:NSLocalizedString(@"Log Out", nil)];
  [alert addButtonWithTitle:NSLocalizedString(@"Cancel", nil)];
  [alert XP_beginSheetModalForWindow:[self window]
                       modalDelegate:self
                      didEndSelector:@selector(enil_logOutAlertDidEnd:returnCode:contextInfo:)
                         contextInfo:NULL];
}

- (void)enil_logOutAlertDidEnd:(NSAlert *)alert
                    returnCode:(NSInteger)returnCode
                   contextInfo:(void *)contextInfo;
{
  (void)contextInfo;
  [[alert window] orderOut:nil];
  if (returnCode != NSAlertFirstButtonReturn) return;

  /* logOutAccountAtPath: tears down THIS controller (releases it). Running
     that from inside our own sheet didEnd is a use-after-free, so defer to
     the next runloop turn after this call stack unwinds. Copy the path now —
     engine_ won't survive the teardown — and never touch self afterwards.
     App-scoped: the AppDelegate owns _accounts and the folder. */
  {
    NSString *accountPath = [[[engine_ enilDir] copy] autorelease];
    [(NSObject *)[NSApp delegate]
        performSelector:@selector(logOutAccountAtPath:)
             withObject:accountPath
             afterDelay:0.0];
  }
}

/* "Reauthenticate" (Accounts menu). Current-account scoped (owned/validated
   here), but the QR flow + _accounts live on the AppDelegate, so hand off
   the account path. Unlike Log Out this does NOT tear self down (the QR
   window comes up alongside; the swap happens later in the post-login
   callback), so a deferred perform is just for menu-action hygiene, not a
   use-after-free guard. The path is copied because the same QR flow may end
   up reauthenticating us in place. */
- (void)enilReauthenticate:(id)sender;
{
  (void)sender;
  {
    NSString *accountPath = [[[engine_ enilDir] copy] autorelease];
    [(NSObject *)[NSApp delegate]
        performSelector:@selector(beginReauthForAccountPath:)
             withObject:accountPath
             afterDelay:0.0];
  }
}

- (void)enil_handleCloseChat;
{
  [self enil_handleSelectChatPayload:nil];
}

- (BOOL)enil_activeChatIsLeavable;
{
  if (!activeChatId_ || ![activeChatId_ length]) return NO;
  return [engine_ isOneToOneChatId:activeChatId_] ? NO : YES;
}

- (void)enil_handleLeaveChat;
{
  NSString *chatId = [self enil_activeChatIdIfConsistent];
  if (!chatId || ![self enil_activeChatIsLeavable]) return;

  NSString *name = activeDisplayName_ ? activeDisplayName_ : chatId;
  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:
    [NSString stringWithFormat:NSLocalizedString(@"Leave \"%@\"?", nil), name]];
  [alert setInformativeText:NSLocalizedString(
    @"You will be removed from this chat and its messages will be "
     "deleted from this device. This cannot be undone.", nil)];
  [alert addButtonWithTitle:NSLocalizedString(@"Leave Chat", nil)];
  [alert addButtonWithTitle:NSLocalizedString(@"Cancel", nil)];

  /* Snapshot the target chat id — the active chat may change while the
     sheet is up. Ownership transfers to the didEnd callback. */
  [alert XP_beginSheetModalForWindow:[self window]
                       modalDelegate:self
                      didEndSelector:@selector(enil_leaveChatAlertDidEnd:returnCode:contextInfo:)
                         contextInfo:(void *)[chatId retain]];
}

- (void)enil_leaveChatAlertDidEnd:(NSAlert *)alert
                       returnCode:(NSInteger)returnCode
                      contextInfo:(void *)contextInfo;
{
  NSString *chatId = [(NSString *)contextInfo autorelease];
  [[alert window] orderOut:nil];
  if (returnCode != NSAlertFirstButtonReturn) return;
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return;

  ENILLog(@"ChatWindowController.enil_handleLeaveChat", @"leaving %@", chatId);
  [engine_ removeChat:chatId lastReadMessageId:nil lastReadMessageTime:0];
  if (enil_chatIdEqual(chatId, activeChatId_)) [self enil_handleCloseChat];
  [chatsController_ reloadData];
}

- (void)enil_confirmDeleteChatId:(NSString *)chatId displayName:(NSString *)name;
{
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return;
  if (![name length]) name = chatId;
  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:
    [NSString stringWithFormat:NSLocalizedString(@"Delete \"%@\"?", nil), name]];
  [alert setInformativeText:NSLocalizedString(
    @"This chat and its messages will be deleted from this device. "
     "It is not deleted on LINE — it will reappear if a new message "
     "arrives. This cannot be undone.", nil)];
  [alert addButtonWithTitle:NSLocalizedString(@"Delete Chat", nil)];
  [alert addButtonWithTitle:NSLocalizedString(@"Cancel", nil)];

  /* Snapshot the target chat id — the active chat may change while the
     sheet is up. Ownership transfers to the didEnd callback. */
  [alert XP_beginSheetModalForWindow:[self window]
                       modalDelegate:self
                      didEndSelector:@selector(enil_deleteChatAlertDidEnd:returnCode:contextInfo:)
                         contextInfo:(void *)[chatId retain]];
}

- (void)enil_deleteChatAlertDidEnd:(NSAlert *)alert
                        returnCode:(NSInteger)returnCode
                       contextInfo:(void *)contextInfo;
{
  NSString *chatId = [(NSString *)contextInfo autorelease];
  [[alert window] orderOut:nil];
  if (returnCode != NSAlertFirstButtonReturn) return;
  if (![chatId isKindOfClass:[NSString class]] || ![chatId length]) return;

  ENILLog(@"ChatWindowController.enil_handleDeleteChat", @"deleting %@", chatId);
  if (![engine_ deleteChatLocally:chatId]) return;
  if (enil_chatIdEqual(chatId, activeChatId_)) [self enil_handleCloseChat];
  [chatsController_ reloadData];
}

@end
