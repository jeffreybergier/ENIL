#import "AppDelegate.h"
#import "XPFoundation.h"
#import "ENILKeychain.h"
#import <AltivecCore/AltivecCore.h>

// --- MainMenu Implementation ---
@interface MainMenu (Private)
+ (void)buildAppMenu:(NSMenu *)mainMenu;
+ (void)buildChatMenu:(NSMenu *)mainMenu;
+ (void)buildEditMenu:(NSMenu *)mainMenu;
+ (void)buildViewMenu:(NSMenu *)mainMenu;
+ (void)buildAccountsMenu:(NSMenu *)mainMenu;
+ (void)buildWindowMenu:(NSMenu *)mainMenu;
@end

/* One open account = its folder + engine + window. A dumb holder; @public
   ivars because only AppDelegate touches it and accessor boilerplate would
   add nothing. dealloc order matters: release the controller first so its
   own dealloc ([engine_ stopSSE]+release) runs while our engine ref still
   keeps the engine alive, then drop our ref. */
@interface ENILAccountContext : NSObject {
 @public
  NSString             *path;
  ENILAccount          *engine;
  ChatWindowController *controller;
}
@end

@implementation ENILAccountContext
- (void)dealloc;
{
  [path release];
  [controller release];
  [engine release];
  [super dealloc];
}
@end

/* Tag stamped on every menu item the dynamic account list owns (the trailing
   separator + one item per account). rebuildAccountsMenu nukes everything
   carrying this tag and regenerates it, leaving the static items untouched. */
#define kENILAccountMenuTag 7301

// --- AppDelegate private helpers (declared so the old PPC GCC sees them
//     before applicationDidFinishLaunching: uses them) ---
@interface AppDelegate (Private)
- (NSString *)enilRootPath;
- (NSArray *)discoverAccountPaths;
- (NSUInteger)openAllAccounts;
- (ENILAccountContext *)contextForPath:(NSString *)accountPath;
- (BOOL)openChatForAccountPath:(NSString *)accountPath;
- (void)beginQRLoginExpectingMid:(NSString *)expectedMid;
- (void)beginReauthForAccountPath:(NSString *)accountPath;
- (void)tearDownChatIfOpenForPath:(NSString *)accountPath;
- (void)logOutAccountAtPath:(NSString *)accountPath;
- (void)setAccountsMenu:(NSMenu *)menu;
- (void)rebuildAccountsMenu;
- (void)selectAccountFromMenu:(id)sender;
- (void)pushKeychainToWorker;
- (void)keychainDidChange:(NSNotification *)note;
- (void)continueLaunchAfterCredentials;
- (void)setupNotifications;
- (void)sseEvent:(NSNotification *)note;
- (void)syncDidFinishForBadge:(NSNotification *)note;
- (void)refreshBadgeCount;
- (int)totalUnreadAcrossAccounts;
- (void)applicationDidResignActive:(NSNotification *)aNotification;
@end

@implementation MainMenu

+ (void)setupMenu;
{
  NSApplication *app = [NSApplication sharedApplication];
  NSMenu *mainMenu = [[[NSMenu alloc] initWithTitle:@"MainMenu"] autorelease];

  [self buildAppMenu:mainMenu];
  [self buildChatMenu:mainMenu];
  [self buildEditMenu:mainMenu];
  [self buildViewMenu:mainMenu];
  [self buildAccountsMenu:mainMenu];
  [self buildWindowMenu:mainMenu];

  [app setMainMenu:mainMenu];
}

+ (void)buildAppMenu:(NSMenu *)mainMenu;
{
  NSApplication *app = [NSApplication sharedApplication];
  NSMenuItem *appMenuItem = [mainMenu addItemWithTitle:@"" action:NULL keyEquivalent:@""];
  NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:@""] autorelease];
  [mainMenu setSubmenu:appMenu forItem:appMenuItem];

  if ([app respondsToSelector:@selector(setAppleMenu:)]) {
    [app performSelector:@selector(setAppleMenu:) withObject:appMenu];
  }

  [appMenu addItemWithTitle:NSLocalizedString(@"About ENIL", nil) action:@selector(showAboutWindow:) keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];

  /* Preferences… — Cmd+, is the standard macOS shortcut. Targets nil so the
     responder chain resolves to AppDelegate's -showPreferencesWindow:. */
  [appMenu addItemWithTitle:NSLocalizedString(@"Preferences…", nil)
                     action:@selector(showPreferencesWindow:)
              keyEquivalent:@","];
  [appMenu addItem:[NSMenuItem separatorItem]];

  [appMenu addItemWithTitle:NSLocalizedString(@"Hide ENIL", nil) action:@selector(hide:) keyEquivalent:@"h"];
  NSMenuItem *hideOthers = [appMenu addItemWithTitle:NSLocalizedString(@"Hide Others", nil)
                                              action:@selector(hideOtherApplications:)
                                       keyEquivalent:@"h"];
  [hideOthers setKeyEquivalentModifierMask:(XPEventModifierFlagCommand | XPEventModifierFlagOption)];
  [appMenu addItemWithTitle:NSLocalizedString(@"Show All", nil) action:@selector(unhideAllApplications:) keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];

  [appMenu addItemWithTitle:NSLocalizedString(@"Quit ENIL", nil) action:@selector(terminate:) keyEquivalent:@"q"];
}

/* "Chat" menu — occupies the slot where "File" normally sits. Every item is
   target-less: actions resolve through the first-responder chain to
   ChatWindowController (toolbar-mirrored commands) or MessageSendViewController
   ("Send Message"). Enable/disable is driven by each target's
   -validateMenuItem:, which reuses the very predicates the NSToolbar uses. */
+ (void)buildChatMenu:(NSMenu *)mainMenu;
{
  NSMenuItem *chatMenuItem = [mainMenu addItemWithTitle:NSLocalizedString(@"Chat", nil) action:NULL keyEquivalent:@""];
  NSMenu *chatMenu = [[[NSMenu alloc] initWithTitle:NSLocalizedString(@"Chat", nil)] autorelease];
  [mainMenu setSubmenu:chatMenu forItem:chatMenuItem];

  /* Not in the toolbar — Cmd+Return, mirrors the Send button. */
  [chatMenu addItemWithTitle:NSLocalizedString(@"Send Message", nil)
                      action:@selector(enilSendCurrentMessage:)
               keyEquivalent:@"\r"];

  [chatMenu addItem:[NSMenuItem separatorItem]];

  /* Toolbar-mirrored commands (same action + same validation). */
  NSMenuItem *markSeen = [chatMenu addItemWithTitle:NSLocalizedString(@"Mark as Seen", nil)
                                             action:@selector(enilMarkChatSeen:)
                                      keyEquivalent:@"\r"];
  [markSeen setKeyEquivalentModifierMask:XPEventModifierFlagOption];

  [chatMenu addItemWithTitle:NSLocalizedString(@"Load More Messages", nil)
                      action:@selector(enilLoadMoreMessages:)
               keyEquivalent:@""];

  [chatMenu addItem:[NSMenuItem separatorItem]];

  [chatMenu addItemWithTitle:NSLocalizedString(@"Close Chat", nil)
                      action:@selector(enilCloseChat:)
               keyEquivalent:@""];
  [chatMenu addItemWithTitle:NSLocalizedString(@"Leave Chat", nil)
                      action:@selector(enilLeaveChat:)
               keyEquivalent:@""];
  [chatMenu addItemWithTitle:NSLocalizedString(@"Delete Chat", nil)
                      action:@selector(enilDeleteChat:)
               keyEquivalent:@""];
}

+ (void)buildEditMenu:(NSMenu *)mainMenu;
{
  NSMenuItem *editMenuItem = [mainMenu addItemWithTitle:NSLocalizedString(@"Edit", nil) action:NULL keyEquivalent:@""];
  NSMenu *editMenu = [[[NSMenu alloc] initWithTitle:NSLocalizedString(@"Edit", nil)] autorelease];
  [mainMenu setSubmenu:editMenu forItem:editMenuItem];

  [editMenu addItemWithTitle:NSLocalizedString(@"Undo", nil) action:@selector(undo:) keyEquivalent:@"z"];
  [editMenu addItemWithTitle:NSLocalizedString(@"Redo", nil) action:@selector(redo:) keyEquivalent:@"Z"];
  [editMenu addItem:[NSMenuItem separatorItem]];
  [editMenu addItemWithTitle:NSLocalizedString(@"Cut", nil) action:@selector(cut:) keyEquivalent:@"x"];
  [editMenu addItemWithTitle:NSLocalizedString(@"Copy", nil) action:@selector(copy:) keyEquivalent:@"c"];
  [editMenu addItemWithTitle:NSLocalizedString(@"Paste", nil) action:@selector(paste:) keyEquivalent:@"v"];
  [editMenu addItemWithTitle:NSLocalizedString(@"Select All", nil) action:@selector(selectAll:) keyEquivalent:@"a"];
}

/* "View" menu — Toggle Sidebar + the four Attachments commands. All items
   are target-less so the responder chain resolves them on the active
   ChatWindowController:
     - toggleSidebar:        inherited from AICookieCutterWindowController
                             (every tier). Cmd+Ctrl+S matches the standard
                             macOS sidebar shortcut.
     - enilHideAttachments:  collapses the inspector
     - enilShowStickers:     uncollapses + switches to sticker mode
     - enilShowEmoji:        uncollapses + switches to sticon mode
     - enilAttachPhoto:      presents the image-picker sheet
   Enable/disable is driven entirely by ChatWindowController's
   -validateMenuItem:, which reuses the same predicates the toolbar segment
   uses for visual feedback. */
+ (void)buildViewMenu:(NSMenu *)mainMenu;
{
  NSMenuItem *viewMenuItem = [mainMenu addItemWithTitle:NSLocalizedString(@"View", nil) action:NULL keyEquivalent:@""];
  NSMenu *viewMenu = [[[NSMenu alloc] initWithTitle:NSLocalizedString(@"View", nil)] autorelease];
  [mainMenu setSubmenu:viewMenu forItem:viewMenuItem];

  NSMenuItem *toggleSidebar = [viewMenu addItemWithTitle:NSLocalizedString(@"Toggle Sidebar", nil)
                                                  action:@selector(toggleSidebar:)
                                           keyEquivalent:@"s"];
  [toggleSidebar setKeyEquivalentModifierMask:
    (XPEventModifierFlagCommand | XPEventModifierFlagControl)];

  [viewMenu addItem:[NSMenuItem separatorItem]];

  [viewMenu addItemWithTitle:NSLocalizedString(@"Hide Attachments", nil)
                      action:@selector(enilHideAttachments:)
               keyEquivalent:@""];
  [viewMenu addItemWithTitle:NSLocalizedString(@"Show Stickers", nil)
                      action:@selector(enilShowStickers:)
               keyEquivalent:@""];
  [viewMenu addItemWithTitle:NSLocalizedString(@"Show Emoji", nil)
                      action:@selector(enilShowEmoji:)
               keyEquivalent:@""];
  [viewMenu addItemWithTitle:NSLocalizedString(@"Attach Photo", nil)
                      action:@selector(enilAttachPhoto:)
               keyEquivalent:@""];

  /* Trailing separator: AppKit silently appends "Enter Full Screen" (and on
     Sonoma+ a "Customize Toolbar…" sibling) to any menu titled "View". This
     separator divides our items from whatever the system tacks on. */
  [viewMenu addItem:[NSMenuItem separatorItem]];
}

/* "Accounts" menu — sits between Edit and Window. Three sections:
     Top    — Add Account (app scoped → AppDelegate, NSApp's delegate is a
              guaranteed targetForAction: stop)
     Middle — Log Out, Reauthenticate (current-account scoped: target-less,
              resolving through the responder chain to ChatWindowController,
              which also validates them)
     Bottom — one item per open account (dynamic; see rebuildAccountsMenu).
   Only the static Top+Middle are built here; the bottom list is regenerated
   on every account add/remove by the AppDelegate, which we hand the submenu
   to now (its _accounts array isn't populated until didFinishLaunching, so
   the first real fill happens then). */
+ (void)buildAccountsMenu:(NSMenu *)mainMenu;
{
  NSMenuItem *accountsMenuItem = [mainMenu addItemWithTitle:NSLocalizedString(@"Accounts", nil) action:NULL keyEquivalent:@""];
  NSMenu *accountsMenu = [[[NSMenu alloc] initWithTitle:NSLocalizedString(@"Accounts", nil)] autorelease];
  [mainMenu setSubmenu:accountsMenu forItem:accountsMenuItem];

  [accountsMenu addItemWithTitle:NSLocalizedString(@"Add Account", nil)
                          action:@selector(addAccount:)
                   keyEquivalent:@""];
  [accountsMenu addItem:[NSMenuItem separatorItem]];
  [accountsMenu addItemWithTitle:NSLocalizedString(@"Log Out", nil)
                          action:@selector(enilLogOut:)
                   keyEquivalent:@""];
  [accountsMenu addItemWithTitle:NSLocalizedString(@"Reauthenticate", nil)
                          action:@selector(enilReauthenticate:)
                   keyEquivalent:@""];

  [(AppDelegate *)[NSApp delegate] setAccountsMenu:accountsMenu];
}

+ (void)buildWindowMenu:(NSMenu *)mainMenu;
{
  NSApplication *app = [NSApplication sharedApplication];
  NSMenuItem *windowMenuItem = [mainMenu addItemWithTitle:NSLocalizedString(@"Window", nil) action:NULL keyEquivalent:@""];
  NSMenu *windowMenu = [[[NSMenu alloc] initWithTitle:NSLocalizedString(@"Window", nil)] autorelease];
  [mainMenu setSubmenu:windowMenu forItem:windowMenuItem];
  [app setWindowsMenu:windowMenu];

  [windowMenu addItemWithTitle:NSLocalizedString(@"Minimize", nil) action:@selector(performMiniaturize:) keyEquivalent:@"m"];
  [windowMenu addItemWithTitle:NSLocalizedString(@"Zoom", nil) action:@selector(performZoom:) keyEquivalent:@""];
  [windowMenu addItem:[NSMenuItem separatorItem]];
  [windowMenu addItemWithTitle:NSLocalizedString(@"Bring All to Front", nil) action:@selector(arrangeInFront:) keyEquivalent:@""];
}

@end

// --- AppDelegate Implementation ---
@implementation AppDelegate

- (void)applicationWillFinishLaunching:(NSNotification *)aNotification;
{
  (void)aNotification;
  [MainMenu setupMenu];
}

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification;
{
  (void)aNotification;

  /* App-wide local notifications. Independent of worker credentials, so set
     this up before the credentials gate below (which can early-return into
     Preferences) — otherwise a fresh, not-yet-configured install would never
     register its SSE observer. */
  [self setupNotifications];

  /* Process-global bootstrap (fonts, curl CA bundle, LINE language) must run
     before ANY network call — the no-accounts QR boot does worker /sign and
     createSession without ever starting an engine. The CA cert lives in the
     AltivecCore bundle; everything else the shared bootstrap derives itself. */
  {
    NSString *cacert = [AltivecCore certPath];
    NSParameterAssert(cacert);
    [ENILAccount bootstrapProcessWithCACertPath:cacert];
  }

  _accounts = [[NSMutableArray alloc] init];

  /* Worker credentials gate. Push whatever ENILKeychain has into the C
     worker layer; if the cascade (env → keychain) is empty, open
     Preferences and defer the rest of launch until the user saves. */
  [[NSNotificationCenter defaultCenter]
    addObserver:self
       selector:@selector(keychainDidChange:)
           name:ENILKeychainDidChangeNotification
         object:nil];
  [self pushKeychainToWorker];
  if (![[ENILKeychain sharedKeychain] hasCredentials]) {
    ENILLog(@"AppDelegate.applicationDidFinishLaunching",
            @"no worker credentials - opening Preferences");
    [self showPreferencesWindow:self];
    return;
  }
  [self continueLaunchAfterCredentials];
}

/* The "real" launch flow — runs immediately when credentials are already
   present, or deferred from -keychainDidChange: once the user saves them
   in Preferences. Idempotent: -openAllAccounts is harmless to call again,
   and we only fall into QR login when there are still zero accounts. */
- (void)continueLaunchAfterCredentials;
{
  if ([self openAllAccounts] == 0) {
    ENILLog(@"AppDelegate.continueLaunchAfterCredentials",
            @"no valid account - starting QR login");
    [self beginQRLoginExpectingMid:nil];
  }
}

// Incoming-message notifications. NSUserNotification (via XPUserNotificationCenter)
// has no permission prompt, so there is nothing to request — just observe SSE
// events app-wide (object:nil covers every open account); the posting account is
// read from -[note object] in -sseEvent:, which decides what's notifiable.
- (void)setupNotifications;
{
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(sseEvent:)
                                               name:ENILSSEEventNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(syncDidFinishForBadge:)
                                               name:ENILSyncDidFinishNotification
                                             object:nil];
}

// Posted on the main thread by ENILAccount after the message is in the DB, so
// -notificationForSSEMessageData: can read the decrypted text + sender. Only
// incoming text/sticon/sticker raise a banner; everything else returns nil.
// Multi-account: object:nil above catches every account, and [note object] is
// the engine that posted it — so we ask THAT account to build the payload.
- (void)sseEvent:(NSNotification *)note;
{
  @try {
    NSDictionary *info = [note userInfo];
    if (![[info objectForKey:@"type"] isEqualToString:@"message"]) return;

    ENILAccount *account = [note object];
    if (![account isKindOfClass:[ENILAccount class]]) return;

    NSDictionary *payload = [account notificationForSSEMessageData:[info objectForKey:@"data"]];
    XPUserNotificationCenter *center = [XPUserNotificationCenter defaultCenter];
    if (payload != nil) {
      ENILLog(@"AppDelegate.sseEvent", @"posting notification: %@",
              [payload objectForKey:@"title"]);
      [center postNotificationWithTitle:[payload objectForKey:@"title"]
                                   body:[payload objectForKey:@"body"]];
    }
    [self refreshBadgeCount];
  } @catch (NSException *exception) {
    ENILLog(@"AppDelegate.sseEvent", @"exception: %@", exception);
  }
}

- (void)syncDidFinishForBadge:(NSNotification *)note;
{
  (void)note;
  [self refreshBadgeCount];
}

- (void)refreshBadgeCount;
{
  /* One Dock icon, many windows: badge the app-wide total, not just the
     account that posted. */
  [[XPUserNotificationCenter defaultCenter] setBadgeCount:[self totalUnreadAcrossAccounts]];
}

// Sum unread across every open account — the Dock badge is app-scoped.
- (int)totalUnreadAcrossAccounts;
{
  int total = 0;
  NSUInteger a, n = [_accounts count];
  for (a = 0; a < n; a++) {
    ENILAccountContext *ctx = [_accounts objectAtIndex:a];
    total += [ctx->engine totalUnreadCount];
  }
  return total;
}

// Wipe delivered banners when ENIL comes to the front. The center's default
// presentation rule only banners while we're NOT frontmost, so reactivation is
// the natural point to clear what the user is now here to read. Fires on launch
// and on every reactivation. Option-1 clearing: all-or-nothing, no per-chat
// targeting (see notes in XPUserNotificationCenter).
- (void)applicationDidBecomeActive:(NSNotification *)aNotification;
{
  (void)aNotification;
  [[XPUserNotificationCenter defaultCenter] removeAllDelivered];
  [[XPUserNotificationCenter defaultCenter] setBadgeCount:0];
}

- (void)applicationDidResignActive:(NSNotification *)aNotification;
{
  (void)aNotification;
  [[XPUserNotificationCenter defaultCenter] setBadgeCount:0];
}

- (void)pushKeychainToWorker;
{
  [ENILAccount syncWorkerCredentialsFromKeychain];
}

- (void)keychainDidChange:(NSNotification *)note;
{
  (void)note;
  [self pushKeychainToWorker];
  /* If launch was blocked on missing credentials, resume now. The guard is
     "still no accounts open" — once an account is open, subsequent saves
     just reconfigure the worker layer without retriggering QR. */
  if ([_accounts count] == 0 && !_qrLoginWindowController &&
      [[ENILKeychain sharedKeychain] hasCredentials]) {
    [self continueLaunchAfterCredentials];
  }
}

- (NSString *)enilRootPath;
{
  NSArray *paths = NSSearchPathForDirectoriesInDomains(
    NSApplicationSupportDirectory, NSUserDomainMask, YES);
  return [[paths objectAtIndex:0] stringByAppendingPathComponent:@"ENIL"];
}

/* Every ENIL/<dir> whose folder name equals the mid inside its session.json
   is a real account. This one rule self-excludes the legacy "default" debug
   account (folder "default" ≠ its u-prefixed mid) and every transient
   ".staging-*" dir (no finalized mid yet) — no blocklist, no data deletion,
   no migration. */
- (NSArray *)discoverAccountPaths;
{
  NSString *root = [self enilRootPath];
  NSFileManager *fm = [NSFileManager defaultManager];
  NSArray *names = [fm XP_contentsOfDirectoryAtPath:root error:NULL];
  NSMutableArray *paths = [NSMutableArray array];
  NSUInteger i, n = [names count];
  for (i = 0; i < n; i++) {
    NSString *name = [names objectAtIndex:i];
    NSString *dir  = [root stringByAppendingPathComponent:name];
    NSString *sess, *mid;
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:dir isDirectory:&isDir] || !isDir) continue;
    sess = [dir stringByAppendingPathComponent:@"session.json"];
    mid  = [ENILAccount validatedMidForSessionAtPath:sess];
    if (mid && [name isEqualToString:mid]) [paths addObject:dir];
  }
  return paths;
}

/* Open one ChatWindowController per discovered account. A dead/cooldown'd
   session just fails its startWithError: and is skipped (logged) — never
   blocks launch, never auto-triggers QR. Returns how many opened. */
- (NSUInteger)openAllAccounts;
{
  NSArray *paths = [self discoverAccountPaths];
  NSUInteger i, n = [paths count], opened = 0;
  for (i = 0; i < n; i++) {
    if ([self openChatForAccountPath:[paths objectAtIndex:i]]) opened++;
  }
  return opened;
}

- (ENILAccountContext *)contextForPath:(NSString *)accountPath;
{
  NSUInteger i, n = [_accounts count];
  for (i = 0; i < n; i++) {
    ENILAccountContext *ctx = [_accounts objectAtIndex:i];
    if ([ctx->path isEqualToString:accountPath]) return ctx;
  }
  return nil;
}

/* Additive: an already-open account is just brought forward (never tear down
   a live on-screen NSWindowController — that leaves a zombie in AppKit's
   global window lists and crashes the next key-state broadcast). A new one
   gets its own engine + window appended to _accounts. Returns NO only if the
   folder has no usable session (caller decides what to do). */
- (BOOL)openChatForAccountPath:(NSString *)accountPath;
{
  ENILAccountContext *existing = [self contextForPath:accountPath];
  if (existing) {
    [[existing->controller window] makeKeyAndOrderFront:self];
    return YES;
  }

  ENILAccount *engine = [[ENILAccount alloc] initWithAccountPath:accountPath];
  NSError *error = nil;
  if (![engine startWithError:&error]) {
    ENILLog(@"AppDelegate.openChatForAccountPath", @"%@ (%@)",
          [error localizedDescription], accountPath);
    [engine release];
    return NO;
  }

  {
    ENILAccountContext *ctx = [[ENILAccountContext alloc] init];
    ctx->path       = [accountPath copy];
    ctx->engine     = engine; /* takes the +1 from alloc */
    ctx->controller = [[ChatWindowController alloc] initWithEngine:engine];
    [ctx->controller showWindow:self];
    [_accounts addObject:ctx];
    [ctx release]; /* _accounts owns it now */
  }
  [self rebuildAccountsMenu]; /* add its row */
  return YES;
}

/* The single QR entry point for BOTH Add Account and Reauthenticate — they
   only differ by expectedMid. The flow is always identity-blind: the mid
   doesn't exist until sign-in completes, so it always runs into a fresh
   STAGING dir and the real ENIL/<mid>/ home is chosen in the delegate
   callback from whatever was actually scanned. expectedMid (nil for Add
   Account) lets that callback detect "user scanned a different account than
   the one being reauthenticated" and ask. */
- (void)beginQRLoginExpectingMid:(NSString *)expectedMid;
{
  if (_qrLoginWindowController) return; /* one at a time */

  NSString *staging = [[self enilRootPath] stringByAppendingPathComponent:
    [NSString stringWithFormat:@".staging-%.0f",
      [NSDate timeIntervalSinceReferenceDate]]];
  [[NSFileManager defaultManager] XP_createDirectoryAtPath:staging
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:NULL];
  _qrLoginWindowController = [[QRLoginWindowController alloc]
    initWithAccountDir:staging expectedMid:expectedMid delegate:self];
  [_qrLoginWindowController start];
}

/* Reauthenticate the account at accountPath. By the name==mid invariant the
   folder name IS the expected mid; passing it through means the post-login
   callback can confirm before adopting a different scanned account. */
- (void)beginReauthForAccountPath:(NSString *)accountPath;
{
  if (!accountPath) return;
  [self beginQRLoginExpectingMid:[accountPath lastPathComponent]];
}

/* If `accountPath` is the currently-open account, tear its window + engine
   down cleanly. Used before swapping a re-authenticated account's
   session.json — the live engine holds enil.sqlite open and runs SSE against
   the now-invalidated token, so it must be released before the file changes
   underneath it. */
- (void)tearDownChatIfOpenForPath:(NSString *)accountPath;
{
  ENILAccountContext *ctx = [self contextForPath:accountPath];
  if (!ctx) return;
  [ctx retain];                  /* survive removal from the array */
  [_accounts removeObject:ctx];
  [[ctx->controller window] orderOut:nil];
  [ctx->controller close];
  [ctx release];                 /* last ref → dealloc stops SSE + frees engine */
  [self rebuildAccountsMenu];    /* drop its row */
}

/* Log Out: tear the account's window/engine down, then move its whole folder
   to the Trash. App-scoped (AppDelegate owns _accounts) and invoked deferred
   from ChatWindowController's sheet callback, so by the time we run the
   controller is no longer mid-callback — safe to release it. If that was the
   last account the app would be windowless, so drop to add-account. */
- (void)logOutAccountAtPath:(NSString *)accountPath;
{
  if (!accountPath) return;
  [self tearDownChatIfOpenForPath:accountPath];
  if (![[NSFileManager defaultManager] XP_trashItemAtPath:accountPath
                                                    error:NULL]) {
    ENILLog(@"AppDelegate.logOutAccountAtPath", @"FAILED to remove %@",
          accountPath);
  }
  if ([_accounts count] == 0) [self beginQRLoginExpectingMid:nil];
}

- (void)setAccountsMenu:(NSMenu *)menu;
{
  _accountsMenu = menu; /* owned by the main menu, not retained here */
}

/* Regenerate the bottom (dynamic) section: a separator + one item per open
   account, all stamped kENILAccountMenuTag so a rebuild can wipe exactly the
   old rows and nothing static. Idempotent; safe before _accounts exists. */
- (void)rebuildAccountsMenu;
{
  NSInteger i;
  NSUInteger a, n;
  if (!_accountsMenu) return;

  for (i = [_accountsMenu numberOfItems] - 1; i >= 0; i--) {
    if ([[_accountsMenu itemAtIndex:i] tag] == kENILAccountMenuTag) {
      [_accountsMenu removeItemAtIndex:i];
    }
  }

  n = [_accounts count];
  if (n == 0) return;

  {
    NSMenuItem *sep = [NSMenuItem separatorItem];
    [sep setTag:kENILAccountMenuTag];
    [_accountsMenu addItem:sep];
  }
  for (a = 0; a < n; a++) {
    ENILAccountContext *ctx = [_accounts objectAtIndex:a];
    NSDictionary *profile = [ctx->engine profile];
    NSString *name = profile ? [profile objectForKey:@"displayName"] : nil;
    NSMenuItem *item;
    if (![name length]) name = [[ctx->engine enilDir] lastPathComponent];
    item = [[[NSMenuItem alloc] initWithTitle:name
                                       action:@selector(selectAccountFromMenu:)
                                keyEquivalent:@""] autorelease];
    [item setTarget:self];                  /* app-scoped, like addAccount: */
    [item setTag:kENILAccountMenuTag];
    [item setRepresentedObject:ctx->path];  /* resolve live at click time */
    [_accountsMenu addItem:item];
  }
}

/* Bring the clicked account's window forward. Resolve by path (not a retained
   context) so a row that raced a teardown simply no-ops. */
- (void)selectAccountFromMenu:(id)sender;
{
  ENILAccountContext *ctx = [self contextForPath:[sender representedObject]];
  if (ctx) [[ctx->controller window] makeKeyAndOrderFront:sender];
}

/* Lazy-instantiate and release-on-close: the About box is a transient
   info window, so we build it on demand and drop our ref once the user
   closes it (see -aboutWindowWillClose:). */
- (void)showAboutWindow:(id)sender;
{
  (void)sender;
  if (!_aboutWindowController) {
    _aboutWindowController = [[AboutWindowController alloc] init];
    [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(aboutWindowWillClose:)
             name:NSWindowWillCloseNotification
           object:[_aboutWindowController window]];
  }
  [_aboutWindowController showWindow:self];
  [[_aboutWindowController window] makeKeyAndOrderFront:self];
}

/* Drop our ref when the About window closes. autorelease (never release):
   we're called synchronously from within AppKit's close cycle while the
   window — whose delegate machinery still references the controller — is
   mid-teardown; releasing here is a use-after-free. */
- (void)aboutWindowWillClose:(NSNotification *)note;
{
  [[NSNotificationCenter defaultCenter]
    removeObserver:self
              name:NSWindowWillCloseNotification
            object:[note object]];
  [_aboutWindowController autorelease];
  _aboutWindowController = nil;
}

/* Lazy-instantiate so the window object only exists once the user (or the
   first-launch gate) actually needs it. AppKit's responder chain resolves
   Cmd+, here via NSApp's delegate. */
- (void)showPreferencesWindow:(id)sender;
{
  (void)sender;
  if (!_preferencesWindowController) {
    _preferencesWindowController = [[PreferencesWindowController alloc] init];
  }
  [_preferencesWindowController showWindow:self];
  [[_preferencesWindowController window] makeKeyAndOrderFront:self];
}

/* QR add-account. App-scoped (resolves to NSApp's delegate). */
- (void)addAccount:(id)sender;
{
  (void)sender;
  [self beginQRLoginExpectingMid:nil];
}

#pragma mark - QRLoginWindowControllerDelegate

/* `stagingDir` holds a freshly-written session.json for a now-known mid.
   Relocate it to ENIL/<mid>/, replacing any prior session.json there (the
   re-login just invalidated it server-side) while preserving that account's
   enil.sqlite + media, then open its window behind the QR window. Returns NO
   on any local failure (validate / copy / engine start) so the controller
   can surface it — the success path is otherwise silent. The controller owns
   its own window + our ref teardown (see qrLoginWindowControllerDidComplete:). */
- (BOOL)qrLoginWindowController:(QRLoginWindowController *)c
        didFinishWithAccountDir:(NSString *)stagingDir;
{
  NSFileManager *fm = [NSFileManager defaultManager];
  NSString *stagingSession =
    [stagingDir stringByAppendingPathComponent:@"session.json"];
  NSString *midStr, *target, *expectedMid;

  midStr = [ENILAccount validatedMidForSessionAtPath:stagingSession];
  if (!midStr) {
    ENILLog(@"AppDelegate.qrLoginDidFinish", @"staged session has no mid: %@",
          stagingDir);
    [fm XP_removeItemAtPath:stagingDir error:NULL];
    return NO;
  }

  /* Reauthenticate scanned a DIFFERENT account than the one being
     reauthenticated. The flow is identity-blind, so from here on it would
     happily adopt whatever was scanned — but for reauth we owe the user a
     choice first. Modal (not a sheet) keeps the synchronous handoff intact;
     a one-off blocking identity decision is a justified deviation from the
     sheet convention. Add Account passes expectedMid==nil and never asks. */
  expectedMid = [c expectedMid];
  if (expectedMid && ![expectedMid isEqualToString:midStr]) {
    NSString *scannedName = midStr;
    NSString *currentName = expectedMid;
    NSString *scannedDisplayName =
      [ENILAccount displayNameForSessionAtPath:stagingSession];
    ENILAccountContext *cur;
    NSDictionary *curProfile;
    NSString *n;
    NSAlert *alert;

    if ([scannedDisplayName length]) scannedName = scannedDisplayName;
    cur = [self contextForPath:
      [[self enilRootPath] stringByAppendingPathComponent:expectedMid]];
    curProfile = cur ? [cur->engine profile] : nil;
    n = curProfile ? [curProfile objectForKey:@"displayName"] : nil;
    if ([n length]) currentName = n;

    alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:NSLocalizedString(@"Different account scanned", nil)];
    [alert setInformativeText:[NSString stringWithFormat:
      NSLocalizedString(
        @"You signed in as \"%@\", not \"%@\". \"%@\" will be left signed out. "
         "Add \"%@\" as a separate account instead?", nil),
      scannedName, currentName, currentName, scannedName]];
    [alert addButtonWithTitle:NSLocalizedString(@"Add Account", nil)];
    [alert addButtonWithTitle:NSLocalizedString(@"Cancel", nil)];
    if ([alert runModal] != NSAlertFirstButtonReturn) {
      [fm XP_removeItemAtPath:stagingDir error:NULL];
      return YES; /* clean close, no error sheet; reauth target untouched */
    }
    /* Confirmed: fall through and adopt the scanned account additively. */
  }

  target        = [[self enilRootPath] stringByAppendingPathComponent:midStr];

  /* Drop the live engine for this mid (its token is now dead) before the
     file changes underneath it. */
  [self tearDownChatIfOpenForPath:target];

  if (![ENILAccount activateQRLoginAtPath:stagingDir accountPath:target]) {
    ENILLog(@"AppDelegate.qrLoginDidFinish", @"FAILED to place session.json at %@ "
          @"— staging kept for recovery: %@", target, stagingDir);
    return NO;
  }
  [fm XP_removeItemAtPath:stagingDir error:NULL];

  if (![self openChatForAccountPath:target]) {
    ENILLog(@"AppDelegate.qrLoginDidFinish", @"session placed but unusable: %@",
          target);
    return NO;
  }
  return YES;
}

/* Success teardown: the controller has already opened nothing of ours and
   closed its own window; we just drop our ref. autorelease (never release):
   we're invoked synchronously from within the controller's -finishWithResult:
   which still has it on the stack — releasing here is a use-after-free. */
- (void)qrLoginWindowControllerDidComplete:(QRLoginWindowController *)c;
{
  (void)c;
  [_qrLoginWindowController autorelease];
  _qrLoginWindowController = nil;
}

/* Called after the user dismisses the failure sheet (window already closed
   by the controller). Discard the orphaned staging dir — it only holds
   resumable QR/key state, never a usable session — and drop our ref via
   autorelease (we're called from within the controller — see above). */
- (void)qrLoginWindowControllerDidFail:(QRLoginWindowController *)c;
{
  NSString *staging = [c accountDir];
  if (staging) {
    [[NSFileManager defaultManager] XP_removeItemAtPath:staging error:NULL];
  }
  [_qrLoginWindowController autorelease];
  _qrLoginWindowController = nil;
}

- (void)dealloc;
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_accounts release];
  [_aboutWindowController release];
  [_preferencesWindowController release];
  [_qrLoginWindowController release];
  [super dealloc];
}

@end
