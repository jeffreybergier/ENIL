//
//  ENILRootCoordinator.m
//  ENIL
//

#import "ENILRootCoordinator.h"
#import "ChatListViewController.h"
#import "PreferencesViewController.h"
#import "QRLoginViewController.h"
#import "StickerViewController.h"
#import "UIViewController+ENILModal.h"
#import "ENILAccount.h"
#import "ENILKeychain.h"
#import "XPFoundation.h"
#import "XPUIKit.h"

/* Persisted mid of the account the user last had active (iOS is single-active,
 * unlike the macOS all-windows model). Preferred at launch when its folder
 * still validates; written when an account is chosen in Settings. */
static NSString * const kENILLastActiveAccountKey = @"ENILLastActiveAccountMid";

@interface ENILRootCoordinator () <QRLoginViewControllerDelegate>
@property (nonatomic, strong) UINavigationController *nav;
@property (nonatomic, strong) ENILAccount *activeAccount;
@property (nonatomic, copy)   NSString *activeAccountPath;
@property (nonatomic, strong) QRLoginViewController *qrLogin; /* live login, 1 at a time */
@property (nonatomic, strong) StickerViewController *stickerPicker; /* per-account, lazy */
@end

@implementation ENILRootCoordinator

- (instancetype)initWithWindow:(UIWindow *)window
{
  if (window == nil) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException
                                  reason:@"window is nil"
                                userInfo:nil];
  }
  if ((self = [super init])) {
    _nav = [[UINavigationController alloc] init];
    window.rootViewController = _nav;
  }
  return self;
}

- (void)start
{
  [[NSNotificationCenter defaultCenter]
    addObserver:self
       selector:@selector(keychainDidChange:)
           name:ENILKeychainDidChangeNotification
         object:nil];
  /* A finished sync may have pulled new sticker/sticon packs; invalidate the
   * cached picker so it re-renders on next appearance instead of showing a
   * stale grid (no-op until a picker has been built). */
  [[NSNotificationCenter defaultCenter]
    addObserver:self
       selector:@selector(syncDidFinishReloadPicker:)
           name:ENILSyncDidFinishNotification
         object:nil];
  [ENILAccount syncWorkerCredentialsFromKeychain];
  [self continueAfterCredentials];
}

/* Worker-credential gate, ported from the macOS AppDelegate. No credentials ->
 * Settings editor. Credentials but no account -> placeholder (QR login is a
 * later milestone). Otherwise activate one. */
- (void)continueAfterCredentials
{
  @try {
    if (![[ENILKeychain sharedKeychain] hasCredentials]) {
      ENILLog(@"ENILRootCoordinator.continueAfterCredentials",
              @"no worker credentials");
      [self showSettings];
      return;
    }
    if (![self activateFirstAccount]) {
      ENILLog(@"ENILRootCoordinator.continueAfterCredentials", @"no account");
      /* Credentials but no account: the settings root doubles as the
       * empty-state — its Accounts section offers Add Account (QR login). */
      [self showSettings];
    }
  } @catch (NSException *exception) {
    ENILLog(@"ENILRootCoordinator.continueAfterCredentials", @"exception: %@",
            exception);
  }
}

/* Resume a launch that was blocked on missing credentials once the user saves
 * them. Guard: still no active account (a later save just reconfigures the
 * worker layer without re-activating). */
- (void)keychainDidChange:(NSNotification *)note
{
  (void)note;
  [ENILAccount syncWorkerCredentialsFromKeychain];
  if (self.activeAccount == nil &&
      [[ENILKeychain sharedKeychain] hasCredentials]) {
    [self continueAfterCredentials];
  }
}

/* ~/Library/Application Support/ENIL, matching the macOS AppDelegate. */
- (NSString *)enilRootPath
{
  NSArray *paths = NSSearchPathForDirectoriesInDomains(
    NSApplicationSupportDirectory, NSUserDomainMask, YES);
  return [[paths objectAtIndex:0] stringByAppendingPathComponent:@"ENIL"];
}

/* Every ENIL/<dir> whose folder name equals the mid inside its session.json is
 * a real account — the name==mid invariant from the macOS AppDelegate. This one
 * rule self-excludes transient ".staging-*" dirs (no finalized mid yet). */
- (NSArray *)discoverAccountPaths
{
  NSString *root = [self enilRootPath];
  NSFileManager *fm = [NSFileManager defaultManager];
  NSArray *names = [fm XP_contentsOfDirectoryAtPath:root error:NULL];
  NSMutableArray *paths = [NSMutableArray array];
  ENILLog(@"ENILRootCoordinator.discoverAccountPaths",
          @"scanning %@ (%lu entr%@)", root, (unsigned long)[names count],
          [names count] == 1 ? @"y" : @"ies");
  NSUInteger i, n = [names count];
  for (i = 0; i < n; i++) {
    NSString *dir = [self acceptedAccountDirForName:[names objectAtIndex:i]
                                             inRoot:root];
    if (dir) [paths addObject:dir];
  }
  ENILLog(@"ENILRootCoordinator.discoverAccountPaths",
          @"%lu account(s) found", (unsigned long)[paths count]);
  return paths;
}

/* Full dir path iff `name` is a real account — folder name == the mid inside
 * its session.json (the name==mid invariant). Returns nil otherwise, logging
 * the precise reason so a hand-placed session.json can be debugged from the
 * device console (syslog). */
- (NSString *)acceptedAccountDirForName:(NSString *)name inRoot:(NSString *)root
{
  NSFileManager *fm = [NSFileManager defaultManager];
  NSString *dir = [root stringByAppendingPathComponent:name];
  BOOL isDir = NO;
  if (![fm fileExistsAtPath:dir isDirectory:&isDir] || !isDir) return nil;

  NSString *sess = [dir stringByAppendingPathComponent:@"session.json"];
  NSString *mid = [ENILAccount validatedMidForSessionAtPath:sess];
  if (mid == nil) {
    ENILLog(@"ENILRootCoordinator.acceptedAccountDirForName",
            @"skip '%@': session.json missing, invalid JSON, or lacks a "
            @"non-empty accessToken + mid", name);
    return nil;
  }
  if (![name isEqualToString:mid]) {
    ENILLog(@"ENILRootCoordinator.acceptedAccountDirForName",
            @"skip '%@': session mid is '%@' but must equal the folder name",
            name, mid);
    return nil;
  }
  ENILLog(@"ENILRootCoordinator.acceptedAccountDirForName",
          @"account '%@'", name);
  return dir;
}

/* The persisted last-active mid, or nil if never set / cleared. */
- (NSString *)lastActiveAccountMid
{
  return [[NSUserDefaults standardUserDefaults]
    objectForKey:kENILLastActiveAccountKey];
}

/* Record (or clear, on empty mid) the user's last-active account. */
- (void)setLastActiveAccountMid:(NSString *)mid
{
  NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
  if ([mid length]) {
    [defaults setObject:mid forKey:kENILLastActiveAccountKey];
  } else {
    [defaults removeObjectForKey:kENILLastActiveAccountKey];
  }
  [defaults synchronize];
}

/* The discovered path whose mid (folder name) matches `mid`, or nil. */
- (NSString *)pathForMid:(NSString *)mid inPaths:(NSArray *)paths
{
  NSUInteger i, n = [paths count];
  for (i = 0; i < n; i++) {
    NSString *path = [paths objectAtIndex:i];
    if ([[path lastPathComponent] isEqualToString:mid]) return path;
  }
  return nil;
}

/* The last-active account if it still validates, else the first discovered.
 * Caller guarantees `paths` is non-empty. */
- (NSString *)preferredAccountPathInPaths:(NSArray *)paths
{
  NSString *mid = [self lastActiveAccountMid];
  NSString *match = [mid length] ? [self pathForMid:mid inPaths:paths] : nil;
  if (match) return match;
  if ([mid length]) {
    ENILLog(@"ENILRootCoordinator.preferredAccountPathInPaths",
            @"last-active '%@' no longer present — using first", mid);
  }
  return [paths objectAtIndex:0];
}

- (BOOL)activateFirstAccount
{
  NSArray *paths = [self discoverAccountPaths];
  if ([paths count] == 0) return NO;
  return [self activateAccountAtPath:[self preferredAccountPathInPaths:paths]];
}

/* Start the engine for one account and make its chat list the nav root. A dead
 * session just fails startWithError: and is skipped (logged), mirroring the
 * macOS openChatForAccountPath:. */
- (BOOL)activateAccountAtPath:(NSString *)path
{
  ENILAccount *engine = [[ENILAccount alloc] initWithAccountPath:path];
  NSError *error = nil;
  if (![engine startWithError:&error]) {
    ENILLog(@"ENILRootCoordinator.activateAccountAtPath",
            @"start failed: %@ (%@)", [error localizedDescription], path);
    return NO;
  }
  self.activeAccount = engine;
  self.activeAccountPath = path;

  /* Mirror macOS ChatWindowController: bring up SSE as soon as the account is
   * active. -startSSE self-gates on enil_db_has_local_rev, so an already-synced
   * account connects at launch with no manual sync; a never-synced one logs
   * "localRev missing" and waits for the first sync to re-arm it. */
  [engine startSSE];

  ChatListViewController *list =
    [[ChatListViewController alloc] initWithAccount:engine coordinator:self];
  [self.nav setViewControllers:[NSArray arrayWithObject:list] animated:NO];
  ENILLog(@"ENILRootCoordinator.activateAccountAtPath", @"active account %@",
          [path lastPathComponent]);
  return YES;
}

/* The settings root, used both as the worker-credential gate (no credentials)
 * and the empty-state (credentials but no account — its Accounts section
 * offers Add Account). Saving credentials posts ENILKeychainDidChange ->
 * -keychainDidChange: continues the launch; adding an account swaps this off
 * the root via -activateAccountAtPath:. */
- (void)showSettings
{
  PreferencesViewController *settings = [[PreferencesViewController alloc] init];
  settings.coordinator = self;
  [self.nav setViewControllers:[NSArray arrayWithObject:settings] animated:NO];
}

#pragma mark - Account management

- (NSArray *)accountSummaries
{
  NSArray *paths = [self discoverAccountPaths];
  NSUInteger i, n = [paths count];
  NSMutableArray *out = [NSMutableArray arrayWithCapacity:n];
  for (i = 0; i < n; i++) {
    NSString *path = [paths objectAtIndex:i];
    NSString *sess = [path stringByAppendingPathComponent:@"session.json"];
    NSString *name = [ENILAccount displayNameForSessionAtPath:sess];
    if (![name length]) name = [path lastPathComponent];
    BOOL active = [path isEqualToString:self.activeAccountPath];
    [out addObject:[NSDictionary dictionaryWithObjectsAndKeys:
      path, @"path",
      name, @"displayName",
      [NSNumber numberWithBool:active], @"active",
      nil]];
  }
  return out;
}

/* Release the live engine after stopping its SSE stream. The old chat list
 * still holds a strong ref until the nav root is swapped, but stopSSE has
 * already torn the network thread down so the overlap is inert. */
- (void)teardownActiveEngine
{
  if (self.activeAccount == nil) return;
  [self.activeAccount stopSSE];
  self.activeAccount = nil;
  self.activeAccountPath = nil;
  /* Drop the cached picker so the next account builds its own against the new
   * engine — the old one still holds the torn-down account's ENILAccount. */
  self.stickerPicker = nil;
}

#pragma mark - Shared per-account UI

- (StickerViewController *)sharedStickerPicker
{
  if (self.activeAccount == nil) return nil;
  if (self.stickerPicker == nil) {
    self.stickerPicker =
      [[StickerViewController alloc] initWithEngine:self.activeAccount];
  }
  return self.stickerPicker;
}

- (void)syncDidFinishReloadPicker:(NSNotification *)note
{
  (void)note;
  [self.stickerPicker setNeedsReload];  /* no-op when never built */
}

/* YES when the active account wants the live SSE link (persisted sseEnabled);
 * NO when no account is active. AppDelegate gates its bg assertion on this. */
- (BOOL)wantsSSELink
{
  return [self.activeAccount isSSEEnabled];
}

- (int)activeUnreadCount
{
  return [self.activeAccount totalUnreadCount];
}

- (BOOL)switchToAccountAtPath:(NSString *)path
{
  if (![path length]) return NO;
  if ([path isEqualToString:self.activeAccountPath]) {
    [self setLastActiveAccountMid:[path lastPathComponent]];
    [self.nav enil_dismissModalViewController];
    return YES;
  }
  [self teardownActiveEngine];
  if (![self activateAccountAtPath:path]) {
    ENILLog(@"ENILRootCoordinator.switchToAccountAtPath", @"start failed: %@",
            path);
    if (![self activateFirstAccount]) [self showSettings];
    return NO;
  }
  [self setLastActiveAccountMid:[path lastPathComponent]];
  [self.nav enil_dismissModalViewController];
  return YES;
}

- (BOOL)logOutAccountAtPath:(NSString *)path
{
  if (![path length]) return NO;
  BOOL wasActive = [path isEqualToString:self.activeAccountPath];
  if (wasActive) [self teardownActiveEngine];

  if (![[NSFileManager defaultManager] XP_trashItemAtPath:path error:NULL]) {
    ENILLog(@"ENILRootCoordinator.logOutAccountAtPath", @"FAILED to remove %@",
            path);
  }
  if (!wasActive) return NO;   /* a background account; the list just reloads */

  /* The live account is gone: promote the next one, or fall back to the
   * settings root (its Accounts section offers Add Account). Either way the
   * Preferences modal that triggered this is dismissed to reveal the new root. */
  if (![self activateFirstAccount]) [self showSettings];
  [self.nav enil_dismissModalViewController];
  return YES;
}

/* The single QR entry point for BOTH Add Account and Reauthenticate — they
 * differ only by expectedMid. The flow is identity-blind: the mid doesn't
 * exist until sign-in completes, so it always runs into a fresh STAGING dir and
 * the real ENIL/<mid>/ home is chosen in -qrLoginViewController:didFinish... .
 * Mirrors -[AppDelegate beginQRLoginExpectingMid:]. */
- (void)beginQRLoginExpectingMid:(NSString *)expectedMid
              fromViewController:(UIViewController *)presenter
{
  if (self.qrLogin) return;   /* one at a time */
  NSString *staging = [[self enilRootPath] stringByAppendingPathComponent:
    [NSString stringWithFormat:@".staging-%.0f",
      [NSDate timeIntervalSinceReferenceDate]]];
  NSError *directoryError = nil;
  if (![[NSFileManager defaultManager] XP_createDirectoryAtPath:staging
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&directoryError]) {
    ENILLog(@"ENILRootCoordinator.beginQRLoginExpectingMid",
            @"cannot create login directory: %@", directoryError);
    [presenter XP_showAlertWithTitle:NSLocalizedString(@"Couldn't Add Account", nil)
                            message:[directoryError localizedDescription]
                       dismissTitle:NSLocalizedString(@"OK", nil)];
    return;
  }
  QRLoginViewController *qr = [[QRLoginViewController alloc]
    initWithAccountDir:staging expectedMid:expectedMid delegate:self];
  self.qrLogin = qr;
  UINavigationController *wrap =
    [[UINavigationController alloc] initWithRootViewController:qr];
  [presenter enil_presentModalViewController:wrap];
}

- (void)presentAddAccountFromViewController:(UIViewController *)presenter
{
  [self beginQRLoginExpectingMid:nil fromViewController:presenter];
}

/* Reauthenticate is current-account scoped (matches macOS): the active mid is
 * the expected one, so a mismatched scan can be detected. */
- (void)presentReauthFromViewController:(UIViewController *)presenter
{
  [self beginQRLoginExpectingMid:[self.activeAccountPath lastPathComponent]
              fromViewController:presenter];
}

#pragma mark - QRLoginViewControllerDelegate

/* The staged session.json carries a now-known mid. Relocate it to ENIL/<mid>/,
 * replacing any prior session there (the re-login just invalidated it server-
 * side) while preserving that account's enil.sqlite + media, then activate it.
 * Returns NO on any local failure so the controller surfaces it. Mirrors
 * -[AppDelegate qrLoginWindowController:didFinishWithAccountDir:]. */
- (BOOL)qrLoginViewController:(QRLoginViewController *)c
       didFinishWithAccountDir:(NSString *)stagingDir
{
  NSFileManager *fm = [NSFileManager defaultManager];
  NSString *stagingSession =
    [stagingDir stringByAppendingPathComponent:@"session.json"];
  NSString *mid = [ENILAccount validatedMidForSessionAtPath:stagingSession];
  if (!mid) {
    ENILLog(@"ENILRootCoordinator.qrLoginDidFinish",
            @"staged session has no mid: %@", stagingDir);
    [fm XP_removeItemAtPath:stagingDir error:NULL];
    return NO;
  }

  /* Reauth scanned a different account than expected. macOS asks via a blocking
   * modal mid-callback; iOS can't nest one cleanly here, so we adopt the
   * scanned account additively (the user still ends up signed in) and log it —
   * the reauth target is simply left untouched. */
  NSString *expected = [c expectedMid];
  if (expected && ![expected isEqualToString:mid]) {
    ENILLog(@"ENILRootCoordinator.qrLoginDidFinish",
            @"reauth expected '%@' but scanned '%@' — adopting additively",
            expected, mid);
  }

  NSString *target =
    [[self enilRootPath] stringByAppendingPathComponent:mid];

  /* If that mid is the live account, drop its engine before the file changes
   * underneath it (its token is now dead). */
  if ([target isEqualToString:self.activeAccountPath]) {
    [self teardownActiveEngine];
  }

  if (![ENILAccount activateQRLoginAtPath:stagingDir accountPath:target]) {
    ENILLog(@"ENILRootCoordinator.qrLoginDidFinish",
            @"FAILED to place session.json at %@ — staging kept: %@",
            target, stagingDir);
    return NO;
  }
  [fm XP_removeItemAtPath:stagingDir error:NULL];

  if (![self activateAccountAtPath:target]) {
    ENILLog(@"ENILRootCoordinator.qrLoginDidFinish",
            @"session placed but unusable: %@", target);
    return NO;
  }
  return YES;
}

/* Success: the account is active and the nav root already swapped. Tear down
 * the whole presented stack (QR + any Preferences modal underneath) to reveal
 * the new chat-list root. */
- (void)qrLoginViewControllerDidComplete:(QRLoginViewController *)c
{
  (void)c;
  self.qrLogin = nil;
  [self.nav enil_dismissModalViewController];
}

/* Cancel or post-success local failure: discard the staging dir (only ever
 * resumable QR/key state, never a usable session) and dismiss just the QR
 * modal, leaving any Preferences modal beneath it in place. */
- (void)qrLoginViewControllerDidFail:(QRLoginViewController *)c
{
  NSString *staging = [c accountDir];
  if ([staging length]) {
    [[NSFileManager defaultManager] XP_removeItemAtPath:staging error:NULL];
  }
  self.qrLogin = nil;
  [c enil_dismissModalViewController];
}

- (void)dealloc
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
}

@end
