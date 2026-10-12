#import <AILocalNotificationCenter.h>
//
//  AppDelegate.m
//  ENIL
//

#import "AppDelegate.h"
#import "ENILRootCoordinator.h"
#import "ENILAccount.h"
#import "XPUIKit.h"
#import "AIFontAwesome.h"
#import "XPFoundation.h"
#import <AltivecCore/AltivecCore.h>

@interface AppDelegate ()
@property (nonatomic, strong) ENILRootCoordinator *coordinator;
@property (nonatomic, assign) UIBackgroundTaskIdentifier bgTask;
@property (nonatomic, strong) NSDate *launchDate;
@property (nonatomic, assign) NSUInteger heartbeatSeq;
- (void)syncDidFinishForBadge:(NSNotification *)note;
- (void)refreshBadgeCount;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
  // Window first so a bootstrap failure still leaves something on screen.
  [self setWindow:[[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]]];
  [self setBgTask:UIBackgroundTaskInvalid];
  [self setLaunchDate:[NSDate date]];

  @try {
    [self bootstrapProcess];
  } @catch (NSException *exception) {
    ENILLog(@"AppDelegate.didFinishLaunching", @"bootstrap failed: %@", exception);
  }

  [self setCoordinator:[[ENILRootCoordinator alloc] initWithWindow:[self window]]];
  [[self coordinator] start];
  [[self window] makeKeyAndVisible];

  [self setupNotifications];
  UILocalNotification *launchNote =
      [launchOptions objectForKey:UIApplicationLaunchOptionsLocalNotificationKey];
  [[AILocalNotificationCenter defaultCenter]
      activateWithUserInfo:[launchNote userInfo]];
  [self registerVoIPKeepAlive];
  ENILLog(@"AppDelegate.didFinishLaunching", @"launched cause=%@",
          ([application applicationState] == UIApplicationStateBackground)
              ? @"background-relaunch" : @"user");
  return YES;
}

// Register the desired notification types each launch. iOS shows its permission
// prompt only for the first request and remembers the answer. On iOS 8, empty
// current settings cannot distinguish an unasked user from a denied request,
// so they must not gate registration. The wrapper is a no-op before iOS 8.
- (void)setupNotifications
{
  AILocalNotificationCenter *center = [AILocalNotificationCenter defaultCenter];
  AINotificationAuthorizationStatus status = [center authorizationStatus];
  ENILLog(@"AppDelegate.setupNotifications", @"authorization status: %d", (int)status);
  [center requestAuthorization];
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
- (void)sseEvent:(NSNotification *)note
{
  @try {
    NSDictionary *info = [note userInfo];
    if (![[info objectForKey:@"type"] isEqualToString:@"message"]) return;

    ENILAccount *account = [note object];
    if (![account isKindOfClass:[ENILAccount class]]) return;

    AILocalNotificationCenter *center = [AILocalNotificationCenter defaultCenter];
    NSDictionary *payload = [account notificationForSSEMessageData:[info objectForKey:@"data"]];
    if (payload != nil) {
      ENILLog(@"AppDelegate.sseEvent", @"posting notification: %@",
              [payload objectForKey:@"title"]);
      [center postNotificationWithTitle:[payload objectForKey:@"title"]
                                   body:[payload objectForKey:@"body"] userInfo:nil];
    }
    [center setBadgeCount:[account totalUnreadCount]];
  } @catch (NSException *exception) {
    ENILLog(@"AppDelegate.sseEvent", @"exception: %@", exception);
  }
}

- (void)syncDidFinishForBadge:(NSNotification *)note
{
  (void)note;
  [self refreshBadgeCount];
}

- (void)refreshBadgeCount
{
  [[AILocalNotificationCenter defaultCenter] setBadgeCount:[[self coordinator] activeUnreadCount]];
}

- (void)dealloc
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
}

// One-time process configuration (fonts, curl CA bundle, LINE language) shared
// with the macOS app delegate.
- (void)bootstrapProcess
{
  NSString *cacert = [AltivecCore certPath];
  if (cacert == nil) {
    @throw [NSException exceptionWithName:NSInternalInconsistencyException
                                  reason:@"cacert.pem missing from app bundle"
                                userInfo:nil];
  }
  [ENILAccount bootstrapProcessWithCACertPath:cacert];

  /* Register the bundled FA OTFs so the native UIFont/UIImage path resolves
   * them by PostScript name (the WebView @font-face path needs no registration,
   * just the files in <bundle>/Fonts). macOS does this via Info.plist. */
  [AIFontAwesome registerBundledFonts];
}

// VoIP keep-alive: the system calls this handler periodically (min 600s) so we
// can re-establish the push connection. UIBackgroundModes=voip in Info.plist is
// required. NOTE: continuous background residency only applies while a socket
// is open with kCFStreamNetworkServiceTypeVoIP set — the eventual SSE client
// must open its connection as a CFStream/NSStream (not libcurl) to qualify.
- (void)registerVoIPKeepAlive
{
  @try {
    UIApplication *app = [UIApplication sharedApplication];
    BOOL ok = [app setKeepAliveTimeout:600.0 handler:^{
      [self logHeartbeat];
      [self beginBackgroundAssertion];
    }];
    ENILLog(@"AppDelegate.registerVoIPKeepAlive", @"registered: %@",
            ok ? @"YES" : @"NO");
  } @catch (NSException *exception) {
    ENILLog(@"AppDelegate.registerVoIPKeepAlive", @"exception: %@", exception);
  }
}

// One liveness line per keep-alive wake (no extra timer). seq/uptime reset on
// relaunch, so a reset across the log marks a prior kill (reason in crash report).
- (void)logHeartbeat
{
  [self setHeartbeatSeq:[self heartbeatSeq] + 1];
  long up = [self launchDate] ? (long)(-[[self launchDate] timeIntervalSinceNow]) : 0;
  NSString *task = ([self bgTask] == UIBackgroundTaskInvalid)
      ? @"none" : [NSString stringWithFormat:@"%lu", (unsigned long)[self bgTask]];
  ENILLog(@"Bg.alive", @"seq=%lu up=%lds task=%@",
          (unsigned long)[self heartbeatSeq], up, task);
}

- (void)applicationDidEnterBackground:(UIApplication *)application
{
  ENILLog(@"AppDelegate.applicationDidEnterBackground", @"entered background");
  [self beginBackgroundAssertion];
}

- (void)applicationWillEnterForeground:(UIApplication *)application
{
  ENILLog(@"AppDelegate.applicationWillEnterForeground", @"entered foreground");
  [self endBackgroundAssertion];
}

- (void)application:(UIApplication *)application
    didReceiveLocalNotification:(UILocalNotification *)notification
{
  if ([application applicationState] == UIApplicationStateInactive)
    [[AILocalNotificationCenter defaultCenter]
        activateWithUserInfo:[notification userInfo]];
}

- (void)applicationWillTerminate:(UIApplication *)application
{
  ENILLog(@"AppDelegate.applicationWillTerminate", @"will terminate");
  [self endBackgroundAssertion];
}

// The unlimitedassertions entitlement removes the time cap on this assertion,
// so the SSE loop (wired in a later milestone) keeps running while
// backgrounded. Without the entitlement the expiration handler fires after
// ~600s and we end cleanly.
- (void)beginBackgroundAssertion
{
  // Smart residency: hold the assertion only to keep the SSE link warm. No link
  // wanted -> drop any assertion and let the system suspend us.
  if (![[self coordinator] wantsSSELink]) {
    [self endBackgroundAssertion];
    return;
  }
  if ([self bgTask] != UIBackgroundTaskInvalid) { return; }  /* heartbeat reports task state */
  @try {
    UIApplication *app = [UIApplication sharedApplication];
    [self setBgTask:[app beginBackgroundTaskWithExpirationHandler:^{
      // Do NOT renew here. The ~600s background budget is per-process, shared
      // across all tasks; the handler fires because it is exhausted. A new task
      // inherits zero budget and its handler fires instantly, spinning into
      // thousands of assertions until backboardd SIGKILLs us. End cleanly and
      // let the system suspend us. Continuous background execution requires the
      // unlimitedassertions entitlement to actually take effect in the signed
      // binary (it currently does not: permittedBackgroundDuration == 600).
      ENILLog(@"AppDelegate.beginBackgroundAssertion", @"expiration fired; ending");
      [self endBackgroundAssertion];
    }]];
    ENILLog(@"AppDelegate.beginBackgroundAssertion", @"assertion %lu",
            (unsigned long)[self bgTask]);
  } @catch (NSException *exception) {
    ENILLog(@"AppDelegate.beginBackgroundAssertion", @"exception: %@", exception);
  }
}

- (void)endBackgroundAssertion
{
  if ([self bgTask] == UIBackgroundTaskInvalid) { return; }
  [[UIApplication sharedApplication] endBackgroundTask:[self bgTask]];
  [self setBgTask:UIBackgroundTaskInvalid];
}

@end
