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
  self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
  self.bgTask = UIBackgroundTaskInvalid;
  self.launchDate = [NSDate date];

  @try {
    [self bootstrapProcess];
  } @catch (NSException *exception) {
    ENILLog(@"AppDelegate.didFinishLaunching", @"bootstrap failed: %@", exception);
  }

  self.coordinator = [[ENILRootCoordinator alloc] initWithWindow:self.window];
  [self.coordinator start];
  [self.window makeKeyAndVisible];

  [self setupNotifications];
  [self registerVoIPKeepAlive];
  ENILLog(@"AppDelegate.didFinishLaunching", @"launched cause=%@",
          ([application applicationState] == UIApplicationStateBackground)
              ? @"background-relaunch" : @"user");
  return YES;
}

// Local notifications for incoming messages. Ask permission only when it has
// never been decided (NotDetermined) so we don't re-prompt an Authorized or
// Denied user on every launch. Then observe SSE events app-wide (object:nil
// covers every account); the account decides what's notifiable, we only post.
- (void)setupNotifications
{
  XPUserNotificationCenter *center = [XPUserNotificationCenter defaultCenter];
  XPNotificationAuthStatus status = [center authorizationStatus];
  ENILLog(@"AppDelegate.setupNotifications", @"authorization status: %d", (int)status);
  if (status == XPNotificationAuthStatusNotDetermined) {
    [center requestAuthorization];
  }
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

    XPUserNotificationCenter *center = [XPUserNotificationCenter defaultCenter];
    XPNotificationAuthStatus status = [center authorizationStatus];
    NSDictionary *payload = [account notificationForSSEMessageData:[info objectForKey:@"data"]];
    if (payload != nil && status != XPNotificationAuthStatusDenied) {
      ENILLog(@"AppDelegate.sseEvent", @"posting notification: %@",
              [payload objectForKey:@"title"]);
      [center postNotificationWithTitle:[payload objectForKey:@"title"]
                                   body:[payload objectForKey:@"body"]];
    } else if (payload != nil) {
      ENILLog(@"AppDelegate.sseEvent", @"notifications denied; skipping post");
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
  [[XPUserNotificationCenter defaultCenter] setBadgeCount:[self.coordinator activeUnreadCount]];
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
  self.heartbeatSeq += 1;
  long up = self.launchDate ? (long)(-[self.launchDate timeIntervalSinceNow]) : 0;
  NSString *task = (self.bgTask == UIBackgroundTaskInvalid)
      ? @"none" : [NSString stringWithFormat:@"%lu", (unsigned long)self.bgTask];
  ENILLog(@"Bg.alive", @"seq=%lu up=%lds task=%@",
          (unsigned long)self.heartbeatSeq, up, task);
}

- (void)applicationDidEnterBackground:(UIApplication *)application
{
  ENILLog(@"AppDelegate.applicationDidEnterBackground", @"entered background");
  [[XPUserNotificationCenter defaultCenter] setBadgeCount:0];
  [self beginBackgroundAssertion];
}

- (void)applicationWillEnterForeground:(UIApplication *)application
{
  ENILLog(@"AppDelegate.applicationWillEnterForeground", @"entered foreground");
  [self endBackgroundAssertion];
}

// Best-effort notification clear when the app becomes active: cold launch (e.g.
// tapped a banner after the app was killed), return from background, and after
// any transient interruption. didBecomeActive is broader than
// willEnterForeground, which skips cold launch. -XP_cancelAllLocalNotifications
// only cancels *scheduled* notifications; setting the badge to 0 is the trick
// that also clears the *delivered* tray (and our app-icon badge), so the count
// resets whenever the user is here to read.
- (void)applicationDidBecomeActive:(UIApplication *)application
{
  ENILLog(@"AppDelegate.applicationDidBecomeActive", @"became active; clearing notifications");
  [application XP_cancelAllLocalNotifications];
  [[XPUserNotificationCenter defaultCenter] setBadgeCount:0];
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
  if (![self.coordinator wantsSSELink]) {
    [self endBackgroundAssertion];
    return;
  }
  if (self.bgTask != UIBackgroundTaskInvalid) { return; }  /* heartbeat reports task state */
  @try {
    UIApplication *app = [UIApplication sharedApplication];
    self.bgTask = [app beginBackgroundTaskWithExpirationHandler:^{
      // Do NOT renew here. The ~600s background budget is per-process, shared
      // across all tasks; the handler fires because it is exhausted. A new task
      // inherits zero budget and its handler fires instantly, spinning into
      // thousands of assertions until backboardd SIGKILLs us. End cleanly and
      // let the system suspend us. Continuous background execution requires the
      // unlimitedassertions entitlement to actually take effect in the signed
      // binary (it currently does not: permittedBackgroundDuration == 600).
      ENILLog(@"AppDelegate.beginBackgroundAssertion", @"expiration fired; ending");
      [self endBackgroundAssertion];
    }];
    ENILLog(@"AppDelegate.beginBackgroundAssertion", @"assertion %lu",
            (unsigned long)self.bgTask);
  } @catch (NSException *exception) {
    ENILLog(@"AppDelegate.beginBackgroundAssertion", @"exception: %@", exception);
  }
}

- (void)endBackgroundAssertion
{
  if (self.bgTask == UIBackgroundTaskInvalid) { return; }
  [[UIApplication sharedApplication] endBackgroundTask:self.bgTask];
  self.bgTask = UIBackgroundTaskInvalid;
}

@end
