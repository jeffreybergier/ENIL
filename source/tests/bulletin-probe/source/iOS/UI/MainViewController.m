#import "MainViewController.h"
#import <objc/runtime.h>
#import <dlfcn.h>
#import <unistd.h>
#import <dispatch/dispatch.h>

static NSString *const kSection = @"com.jeffburg.enil.bulletinprobe";

/* Check each argument separately: offsets differ across architectures. */
static BOOL XPMatches(id object, SEL selector, const char *result,
                      const char *argument)
{
  if (![object respondsToSelector:selector]) return NO;
  NSMethodSignature *sig = [object methodSignatureForSelector:selector];
  return sig && strcmp([sig methodReturnType], result) == 0 &&
      [sig numberOfArguments] == (argument ? 3U : 2U) &&
      (!argument || strcmp([sig getArgumentTypeAtIndex:2], argument) == 0);
}

static BOOL XPSetObject(id object, NSString *name, id value)
{
  SEL selector = NSSelectorFromString(name);
  if (!XPMatches(object, selector, "v", "@")) return NO;
  NSInvocation *call = [NSInvocation invocationWithMethodSignature:
      [object methodSignatureForSelector:selector]];
  [call setTarget:object];
  [call setSelector:selector];
  [call setArgument:&value atIndex:2];
  [call invoke];
  return YES;
}

/* This diagnostic intentionally uses inspected signatures for invocation;
 * no cast from the runtime's generic IMP type is required. Calls are immediate,
 * so NSInvocation never outlives its borrowed arguments or target. */
static void XPInvokeVoid(id object, SEL selector)
{
  NSInvocation *call = [NSInvocation invocationWithMethodSignature:
      [object methodSignatureForSelector:selector]];
  [call setTarget:object];
  [call setSelector:selector];
  [call invoke];
}

/* init ownership is expressed to ARC rather than recovered from NSInvocation.
 * Queue pointers have the same ABI on pre-OS_OBJECT and ObjC dispatch runtimes. */
@protocol XPObserverInitialization
- (id)initWithQueue:(dispatch_queue_t)queue;
@end

static id XPGetObject(id object, NSString *name)
{
  SEL selector = NSSelectorFromString(name);
  if (!XPMatches(object, selector, "@", NULL)) return nil;
  NSInvocation *call = [NSInvocation invocationWithMethodSignature:
      [object methodSignatureForSelector:selector]];
  [call setTarget:object];
  [call setSelector:selector];
  [call invoke];
  __unsafe_unretained id value = nil;
  [call getReturnValue:&value];
  return value;
}

static BOOL XPABITypeEqual(const char *a, const char *b)
{
  /* Protocol encodings can include @"ClassName" where method encodings use @.
   * Both are object pointers; blocks (@?) remain a distinct ABI contract. */
  if (a[0] == '@' && b[0] == '@' && a[1] != '?' && b[1] != '?') return YES;
  return strcmp(a, b) == 0;
}

static BOOL XPSameSignature(NSMethodSignature *a, NSMethodSignature *b)
{
  if (!a || !b || !XPABITypeEqual([a methodReturnType], [b methodReturnType]) ||
      [a numberOfArguments] != [b numberOfArguments]) return NO;
  for (NSUInteger i = 0; i < [a numberOfArguments]; i++)
    if (!XPABITypeEqual([a getArgumentTypeAtIndex:i], [b getArgumentTypeAtIndex:i]))
      return NO;
  return YES;
}

@implementation MainViewController

- (void)record:(NSString *)message;
{
  NSString *line = [NSString stringWithFormat:@"%@ %@\n", [NSDate date], message];
  [log_ appendString:line];
  [output_ setText:log_];
  [output_ scrollRangeToVisible:NSMakeRange([log_ length], 0)];
  NSString *path = [NSHomeDirectory()
      stringByAppendingPathComponent:@"Documents/bulletin-probe.log"];
  if (![[NSFileManager defaultManager] fileExistsAtPath:path])
    [[NSFileManager defaultManager] createFileAtPath:path contents:nil
                                         attributes:nil];
  NSFileHandle *file = [NSFileHandle fileHandleForWritingAtPath:path];
  [file seekToEndOfFile];
  [file writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
  [file synchronizeFile];
  [file closeFile];
  NSLog(@"%@", message);
}

- (void)inspect;
{
  [self record:[NSString stringWithFormat:@"OS=%@ uid=%d home=%@",
      [[UIDevice currentDevice] systemVersion], getuid(), NSHomeDirectory()]];
  /* Keep the image loaded for process lifetime: ObjC classes may retain IMPs. */
  static void *image;
  if (!image) image = dlopen("/System/Library/PrivateFrameworks/"
      "BulletinBoard.framework/BulletinBoard", RTLD_NOW | RTLD_LOCAL);
  if (!image) {
    const char *error = dlerror();
    [self record:[NSString stringWithFormat:@"dlopen FAILED: %s",
        error ? error : "unknown"]];
    return;
  }
  [self record:@"dlopen OK; availability does not establish IPC access"];
  NSArray *names = [NSArray arrayWithObjects:@"BBBulletinRequest", @"BBBulletin",
      @"BBServer", @"BBObserver", @"BBDataProviderConnection",
      @"BBDataProviderProxy", @"BBServerConduit", nil];
  for (NSString *name in names) {
    Class cls = NSClassFromString(name);
    [self record:[NSString stringWithFormat:@"CLASS %@: %@", name,
        cls ? @"present" : @"absent"]];
    if (!cls) continue;
    for (int meta = 0; meta < 2; meta++) {
      unsigned int count = 0;
      Method *methods = class_copyMethodList(meta ? object_getClass(cls) : cls,
                                             &count);
      for (unsigned int i = 0; i < count; i++) {
        [self record:[NSString stringWithFormat:@"%@ %c %@ %s", name,
            meta ? '+' : '-', NSStringFromSelector(method_getName(methods[i])),
            method_getTypeEncoding(methods[i])]];
      }
      free(methods);
    }
  }
  [self record:@"INSPECT COMPLETE; no server/observer instantiated"];
}

- (void)finishTask;
{
  if (task_ != UIBackgroundTaskInvalid) {
    [[UIApplication sharedApplication] endBackgroundTask:task_];
    task_ = UIBackgroundTaskInvalid;
  }
  pending_ = NO;
}

- (void)publish;
{
  @try {
    if (request_) {
      [self record:@"SKIP: withdraw existing request first"];
      return;
    }
    Class cls = NSClassFromString(@"BBBulletinRequest");
    id request = [[cls alloc] init];
    if (!XPMatches(request, NSSelectorFromString(@"publish"), "v", NULL) ||
        !XPMatches(request, NSSelectorFromString(@"withdraw"), "v", NULL)) {
      [self record:@"UNSUPPORTED: request publish/withdraw signature missing"];
      return;
    }
    NSString *identity = [[NSProcessInfo processInfo] globallyUniqueString];
    NSArray *keys = [NSArray arrayWithObjects:@"setSectionID:", @"setTitle:",
        @"setSubtitle:", @"setMessage:", @"setDate:",
        @"setPublisherBulletinID:", @"setRecordID:", nil];
    NSArray *values = [NSArray arrayWithObjects:kSection, @"Private probe title",
        @"Private probe subtitle", @"Synthetic BulletinBoard message",
        [NSDate date], identity, identity, nil];
    for (NSUInteger i = 0; i < [keys count]; i++) {
      if (!XPSetObject(request, [keys objectAtIndex:i], [values objectAtIndex:i])) {
        [self record:[@"UNSUPPORTED setter: "
            stringByAppendingString:[keys objectAtIndex:i]]];
        return;
      }
    }
    request_ = request;
    [self record:[NSString stringWithFormat:@"CALL publish id=%@ state=%ld",
        identity, (long)[[UIApplication sharedApplication] applicationState]]];
    SEL selector = NSSelectorFromString(@"publish");
    XPInvokeVoid(request_, selector);
    [self record:@"RETURN publish (delivery UNVERIFIED)"];
  } @catch (NSException *exception) {
    [self record:[NSString stringWithFormat:@"EXCEPTION %@", exception]];
  } @finally {
    [self finishTask];
  }
}

- (void)withdraw;
{
  @try {
    SEL selector = NSSelectorFromString(@"withdraw");
    if (!XPMatches(request_, selector, "v", NULL)) {
      [self record:@"SKIP: no live withdrawable request in this process"];
      return;
    }
    [self record:@"CALL withdraw (only this process's synthetic request)"];
    XPInvokeVoid(request_, selector);
    [self record:@"RETURN withdraw (removal UNVERIFIED)"];
    request_ = nil;
  } @catch (NSException *exception) {
    [self record:[NSString stringWithFormat:@"EXCEPTION %@", exception]];
  }
}


- (void)stopObserver;
{
  [NSObject cancelPreviousPerformRequestsWithTarget:self
      selector:@selector(listTimedOut) object:nil];
  id previous = observer_;
  observer_ = nil;
  if (previous) {
    XPSetObject(previous, @"setDelegate:", nil);
    if (XPMatches(previous, @selector(invalidate), "v", NULL))
      XPInvokeVoid(previous, @selector(invalidate));
  }
}

- (void)listTimedOut;
{
  if (!receivedBulletin_) {
    [status_ setText:@"No bulletin response; empty/denied unknown"];
    [self record:@"LIST TIMEOUT: no own-app bulletin callbacks in 5s. "
        @"This does not prove Notification Center is empty."];
  } else {
    [self record:@"LIST window ended; observed rows are not a completeness guarantee"];
  }
}

- (void)listDelivered;
{
  @try {
    [self stopObserver];
    [bulletins_ removeAllObjects];
    selectedID_ = nil;
    receivedBulletin_ = NO;
    [bulletinTable_ reloadData];
    [status_ setText:@"Requesting this app's delivered bulletins..."];
    Class cls = NSClassFromString(@"BBObserver");
    Method init = class_getInstanceMethod(cls, @selector(initWithQueue:));
    NSMethodSignature *sig = init ? [NSMethodSignature
        signatureWithObjCTypes:method_getTypeEncoding(init)] : nil;
    if (!sig || strcmp([sig methodReturnType], "@") ||
        [sig numberOfArguments] != 3 ||
        (strcmp([sig getArgumentTypeAtIndex:2], "@") &&
         strcmp([sig getArgumentTypeAtIndex:2], "^v"))) {
      [status_ setText:@"Observer initializer unavailable"];
      [self record:@"UNSUPPORTED observer initWithQueue: signature"];
      return;
    }
    /* Validate the callbacks against the installed protocol before registering
     * a delegate; the feed integer width changed between architectures. */
    Protocol *protocol = NSProtocolFromString(@"BBObserverDelegate");
    NSArray *callbacks = [NSArray arrayWithObjects:
        @"observer:addBulletin:forFeed:", @"observer:modifyBulletin:forFeed:",
        @"observer:removeBulletin:forFeed:", nil];
    for (NSString *name in callbacks) {
      SEL selector = NSSelectorFromString(name);
      struct objc_method_description desc = { NULL, NULL };
      if (protocol) {
        desc = protocol_getMethodDescription(protocol, selector, NO, YES);
        if (!desc.types)
          desc = protocol_getMethodDescription(protocol, selector, YES, YES);
      }
      [self record:[NSString stringWithFormat:@"CALLBACK %@ protocol=%s local=%s",
          name, desc.types ?: "unavailable", method_getTypeEncoding(
              class_getInstanceMethod([self class], selector))]];
      NSMethodSignature *expected = desc.types ? [NSMethodSignature
          signatureWithObjCTypes:desc.types] : nil;
      if (!expected) {
        /* Some runtimes strip protocol descriptions. The known object/object/
         * feed callbacks use the same integer ABI as BBObserver.observerFeed.
         * Refuse a different width rather than truncate the callback argument.
         * Source: nst/iOS-Runtime-Headers protocols/BBObserverDelegate.h. */
        Method getter = class_getInstanceMethod(cls,
            NSSelectorFromString(@"observerFeed"));
        NSMethodSignature *feedSignature = getter ? [NSMethodSignature
            signatureWithObjCTypes:method_getTypeEncoding(getter)] : nil;
        if (feedSignature && [feedSignature numberOfArguments] == 2 &&
            !strcmp([feedSignature methodReturnType], @encode(NSUInteger))) {
          [self record:@"Protocol metadata absent; using known callbacks with "
              @"runtime-verified observerFeed integer width"];
          continue;
        }
      }
      if (!XPSameSignature(expected, [self methodSignatureForSelector:selector])) {
        [status_ setText:@"Observer callback ABI unavailable"];
        [self record:[@"UNSUPPORTED protocol callback: "
            stringByAppendingString:name]];
        return;
      }
    }
    observer_ = [(id<XPObserverInitialization>)[cls alloc]
        initWithQueue:dispatch_get_main_queue()];
    if (!XPMatches(observer_, @selector(invalidate), "v", NULL) ||
        !XPMatches(observer_, NSSelectorFromString(
            @"requestListBulletinsForSectionID:"), "v", "@") ||
        !XPSetObject(observer_, @"setDelegate:", self)) {
      [self stopObserver];
      [status_ setText:@"Observer methods unavailable"];
      [self record:@"UNSUPPORTED observer methods"];
      return;
    }
    SEL feedSelector = NSSelectorFromString(@"observerFeed");
    if (XPMatches(observer_, feedSelector, @encode(NSUInteger), NULL)) {
      NSInvocation *readFeed = [NSInvocation invocationWithMethodSignature:
          [observer_ methodSignatureForSelector:feedSelector]];
      [readFeed setTarget:observer_];
      [readFeed setSelector:feedSelector];
      [readFeed invoke];
      NSUInteger feed = 0;
      [readFeed getReturnValue:&feed];
      [self record:[NSString stringWithFormat:@"OBSERVER initial feed=%lu",
          (unsigned long)feed]];
    }
    [self record:@"CALL requestListBulletinsForSectionID: (own section only)"];
    XPSetObject(observer_, @"requestListBulletinsForSectionID:", kSection);
    [self performSelector:@selector(listTimedOut) withObject:nil afterDelay:5];
  } @catch (NSException *exception) {
    [status_ setText:@"Observer exception; see log"];
    [self record:[NSString stringWithFormat:@"LIST EXCEPTION %@", exception]];
    [self stopObserver];
  }
}

- (void)observer:(id)observer addBulletin:(id)bulletin
        forFeed:(NSUInteger)feed;
{
  if (![NSThread isMainThread]) {
    __weak MainViewController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf observer:observer addBulletin:bulletin forFeed:feed];
    });
    return;
  }
  if (observer != observer_) return; // stale generation
  NSString *section = XPGetObject(bulletin, @"sectionID");
  NSString *identity = XPGetObject(bulletin, @"bulletinID");
  if (![section isEqual:kSection] || ![identity isKindOfClass:[NSString class]])
    return; // Never retain or log another app's content.
  receivedBulletin_ = YES;
  NSUInteger found = NSNotFound;
  for (NSUInteger i = 0; i < [bulletins_ count]; i++)
    if ([XPGetObject([bulletins_ objectAtIndex:i], @"bulletinID")
        isEqual:identity]) { found = i; break; }
  if (found != NSNotFound) [bulletins_ replaceObjectAtIndex:found withObject:bulletin];
  else if ([bulletins_ count] < 100) [bulletins_ addObject:bulletin];
  [status_ setText:[NSString stringWithFormat:@"Observed %lu; tap a row to select",
      (unsigned long)[bulletins_ count]]];
  [self record:[NSString stringWithFormat:@"OBSERVED id=%@ feed=%lu body=%@",
      identity, (unsigned long)feed, XPGetObject(bulletin, @"message")]];
  [bulletinTable_ reloadData];
}

- (void)observer:(id)observer modifyBulletin:(id)bulletin
        forFeed:(NSUInteger)feed;
{
  [self observer:observer addBulletin:bulletin forFeed:feed];
}

- (void)observer:(id)observer removeBulletin:(id)bulletin
        forFeed:(NSUInteger)feed;
{
  if (![NSThread isMainThread]) {
    __weak MainViewController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf observer:observer removeBulletin:bulletin forFeed:feed];
    });
    return;
  }
  if (observer != observer_ || ![XPGetObject(bulletin, @"sectionID")
      isEqual:kSection]) return;
  NSString *identity = XPGetObject(bulletin, @"bulletinID");
  for (NSInteger i = (NSInteger)[bulletins_ count] - 1; i >= 0; i--)
    if ([XPGetObject([bulletins_ objectAtIndex:(NSUInteger)i], @"bulletinID")
        isEqual:identity]) [bulletins_ removeObjectAtIndex:(NSUInteger)i];
  if ([selectedID_ isEqual:identity]) selectedID_ = nil;
  [self record:[NSString stringWithFormat:@"REMOVE callback id=%@ feed=%lu",
      identity, (unsigned long)feed]];
  [status_ setText:@"Removal callback received; verify tray visually"];
  [bulletinTable_ reloadData];
}

- (void)observer:(id)observer noteInvalidatedBulletinIDs:(NSSet *)identities;
{
  if (![NSThread isMainThread]) {
    NSSet *snapshot = [identities copy];
    __weak MainViewController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf observer:observer noteInvalidatedBulletinIDs:snapshot];
    });
    return;
  }
  if (observer != observer_) return;
  NSUInteger removed = 0;
  for (NSInteger i = (NSInteger)[bulletins_ count] - 1; i >= 0; i--) {
    NSString *identity = XPGetObject([bulletins_ objectAtIndex:(NSUInteger)i],
        @"bulletinID");
    if ([identities containsObject:identity]) {
      if ([selectedID_ isEqual:identity]) selectedID_ = nil;
      [bulletins_ removeObjectAtIndex:(NSUInteger)i];
      removed++;
    }
  }
  [self record:[NSString stringWithFormat:@"INVALIDATED %lu own observed IDs; "
      @"invalidation alone does not establish tray removal", (unsigned long)removed]];
  [bulletinTable_ reloadData];
}

- (void)observer:(id)observer noteServerConnectionStateChanged:(BOOL)connected;
{
  if (![NSThread isMainThread]) {
    __weak MainViewController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf observer:observer noteServerConnectionStateChanged:connected];
    });
    return;
  }
  if (observer != observer_) return;
  [self record:[NSString stringWithFormat:@"OBSERVER connection=%@",
      connected ? @"connected" : @"disconnected"]];
}

- (void)clearSelected;
{
  @try {
    id selected = nil;
    for (id bulletin in bulletins_)
      if ([XPGetObject(bulletin, @"bulletinID") isEqual:selectedID_] &&
          [XPGetObject(bulletin, @"sectionID") isEqual:kSection]) selected = bulletin;
    if (!selected) {
      [self record:@"SKIP: list delivered and select an observed row first"];
      return;
    }
    SEL selector = NSSelectorFromString(@"clearBulletins:inSection:");
    NSMethodSignature *sig = [observer_ methodSignatureForSelector:selector];
    if (![observer_ respondsToSelector:selector] || !sig ||
        strcmp([sig methodReturnType], "v") || [sig numberOfArguments] != 4 ||
        strcmp([sig getArgumentTypeAtIndex:2], "@") ||
        strcmp([sig getArgumentTypeAtIndex:3], "@")) {
      [self record:@"UNSUPPORTED clearBulletins:inSection: signature"];
      return;
    }
    id values = [NSArray arrayWithObject:selected];
    id section = kSection;
    NSInvocation *call = [NSInvocation invocationWithMethodSignature:sig];
    [call setTarget:observer_];
    [call setSelector:selector];
    [call setArgument:&values atIndex:2];
    [call setArgument:&section atIndex:3];
    [self record:[NSString stringWithFormat:@"CALL clear one observed id=%@",
        selectedID_]];
    [call invoke];
    [self record:@"RETURN clear one; badge unchanged by probe; removal UNVERIFIED"];
    // Keep the row until a removal callback or an explicit fresh read.
  } @catch (NSException *exception) {
    [self record:[NSString stringWithFormat:@"CLEAR EXCEPTION %@", exception]];
  }
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section;
{
  (void)tableView; (void)section;
  return (NSInteger)[bulletins_ count];
}

- (UITableViewCell *)tableView:(UITableView *)tableView
        cellForRowAtIndexPath:(NSIndexPath *)indexPath;
{
  UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"bulletin"];
  if (!cell) cell = [[UITableViewCell alloc]
      initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"bulletin"];
  id bulletin = [bulletins_ objectAtIndex:(NSUInteger)[indexPath row]];
  NSString *identity = XPGetObject(bulletin, @"bulletinID");
  [[cell textLabel] setText:XPGetObject(bulletin, @"message") ?: @"(no message)"];
  [[cell textLabel] setFont:[UIFont systemFontOfSize:12]];
  [[cell detailTextLabel] setText:identity];
  [cell setAccessoryType:[selectedID_ isEqual:identity] ?
      UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone];
  return cell;
}

- (void)tableView:(UITableView *)tableView
 didSelectRowAtIndexPath:(NSIndexPath *)indexPath;
{
  selectedID_ = [XPGetObject([bulletins_ objectAtIndex:
      (NSUInteger)[indexPath row]], @"bulletinID") copy];
  [tableView reloadData];
  [self record:[@"SELECT " stringByAppendingString:selectedID_]];
}

- (void)schedulePublicCount:(NSUInteger)count;
{
  UIApplication *app = [UIApplication sharedApplication];
  if ([app respondsToSelector:@selector(registerUserNotificationSettings:)])
    [app registerUserNotificationSettings:[UIUserNotificationSettings
        settingsForTypes:(UIUserNotificationTypeAlert |
            UIUserNotificationTypeSound | UIUserNotificationTypeBadge)
              categories:nil]];
  for (NSUInteger i = 0; i < count; i++) {
    UILocalNotification *note = [[UILocalNotification alloc] init];
    NSString *identity = [[NSProcessInfo processInfo] globallyUniqueString];
    publicSequence_++;
    [note setAlertBody:[NSString stringWithFormat:@"Public probe #%lu (%@)",
        (unsigned long)publicSequence_, [identity substringToIndex:8]]];
    [note setUserInfo:[NSDictionary dictionaryWithObject:identity forKey:@"probeID"]];
    [note setSoundName:UILocalNotificationDefaultSoundName];
    [note setApplicationIconBadgeNumber:(NSInteger)count];
    [note setFireDate:[NSDate dateWithTimeIntervalSinceNow:15 + i * 3]];
    [publicNotes_ addObject:note];
    [app scheduleLocalNotification:note];
    [self record:[NSString stringWithFormat:@"SCHEDULE %@ +%lus",
        [note alertBody], (unsigned long)(15 + i * 3)]];
  }
  [self record:@"Press Home; saved public requests are NOT a delivered inventory"];
}

- (void)runCommand:(NSString *)command;
{
  (void)[self view];
  [self record:[@"COMMAND " stringByAppendingString:command]];
  if ([command isEqualToString:@"inspect"]) {
    [self inspect];
  } else if ([command isEqualToString:@"publish"]) {
    if (pending_) return;
    pending_ = YES;
    __weak MainViewController *weakSelf = self;
    task_ = [[UIApplication sharedApplication]
        beginBackgroundTaskWithExpirationHandler:^{
          MainViewController *controller = weakSelf;
          [NSObject cancelPreviousPerformRequestsWithTarget:controller
              selector:@selector(publish) object:nil];
          [controller record:@"EXPIRED before completion"];
          [controller finishTask];
        }];
    [self record:@"Private publish in 8 seconds; press Home now"];
    [self performSelector:@selector(publish) withObject:nil afterDelay:8];
  } else if ([command isEqualToString:@"withdraw"]) {
    [self withdraw];
  } else if ([command isEqualToString:@"public"]) {
    [self schedulePublicCount:1];
  } else if ([command isEqualToString:@"batch"]) {
    [self schedulePublicCount:3];
  } else if ([command isEqualToString:@"badge"]) {
    UIApplication *app = [UIApplication sharedApplication];
    NSInteger before = [app applicationIconBadgeNumber];
    [app setApplicationIconBadgeNumber:0];
    [self record:[NSString stringWithFormat:@"BADGE ONLY %ld -> %ld; no cancellation",
        (long)before, (long)[app applicationIconBadgeNumber]]];
  } else if ([command isEqualToString:@"cancel"]) {
    [[UIApplication sharedApplication] cancelAllLocalNotifications];
    [self record:@"CANCEL ALL ONLY; no badge write"];
  } else if ([command isEqualToString:@"cancel-one"]) {
    UILocalNotification *note = ([publicNotes_ count] ? [publicNotes_ objectAtIndex:0] : nil);
    if (note) {
      [[UIApplication sharedApplication] cancelLocalNotification:note];
      [self record:[NSString stringWithFormat:@"CANCEL ONE saved public request %@; "
          @"delivery/removal unverified; no badge write", [note alertBody]]];
      [publicNotes_ removeObjectAtIndex:0];
    } else [self record:@"No saved public request in this process"];
  } else if ([command isEqualToString:@"pending"]) {
    NSArray *notes = [[UIApplication sharedApplication] scheduledLocalNotifications];
    [self record:[NSString stringWithFormat:@"PENDING count=%lu (NOT delivered)",
        (unsigned long)[notes count]]];
    for (UILocalNotification *note in notes)
      [self record:[NSString stringWithFormat:@"PENDING %@ fire=%@",
          [note alertBody], [note fireDate]]];
  } else if ([command isEqualToString:@"list"]) {
    [self listDelivered];
  } else if ([command isEqualToString:@"clear-one"]) {
    [self clearSelected];
  } else if ([command isEqualToString:@"clear"]) {
    [[UIApplication sharedApplication] cancelAllLocalNotifications];
    [[UIApplication sharedApplication] setApplicationIconBadgeNumber:0];
    [self record:@"Public cancel-all + badge zero (removal UNVERIFIED)"];
  } else {
    [self record:@"Unknown command; no action"];
  }
}

- (void)buttonPressed:(UIButton *)sender;
{
  NSArray *commands = [NSArray arrayWithObjects:@"inspect", @"public",
      @"publish", @"withdraw", @"clear", @"batch", @"badge", @"cancel",
      @"cancel-one", @"pending", @"list", @"clear-one", nil];
  [self runCommand:[commands objectAtIndex:(NSUInteger)[sender tag]]];
}

- (void)viewDidLoad;
{
  [super viewDidLoad];
  task_ = UIBackgroundTaskInvalid;
  log_ = [[NSMutableString alloc] init];
  bulletins_ = [[NSMutableArray alloc] init];
  publicNotes_ = [[NSMutableArray alloc] init];
  [[self view] setBackgroundColor:[UIColor whiteColor]];
  NSArray *titles = [NSArray arrayWithObjects:@"Inspect", @"Public +15s",
      @"Private +8s", @"Withdraw", @"Clear public", @"Public x3",
      @"Badge only", @"Cancel all", @"Cancel one", @"Pending", @"Delivered",
      @"Clear selected", nil];
  for (NSUInteger i = 0; i < [titles count]; i++) {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeRoundedRect];
    [button setFrame:CGRectMake(8 + (i % 3) * 104, 28 + (i / 3) * 44, 100, 40)];
    [button setTitle:[titles objectAtIndex:i] forState:UIControlStateNormal];
    [button setTag:(NSInteger)i];
    [button addTarget:self action:@selector(buttonPressed:)
        forControlEvents:UIControlEventTouchUpInside];
    [[self view] addSubview:button];
  }
  CGRect bounds = [[self view] bounds];
  status_ = [[UILabel alloc] initWithFrame:CGRectMake(8, 207, bounds.size.width - 16, 22)];
  [status_ setFont:[UIFont systemFontOfSize:11]];
  [status_ setText:@"Delivered: not requested; select a row to clear"];
  [[self view] addSubview:status_];
  bulletinTable_ = [[UITableView alloc] initWithFrame:
      CGRectMake(8, 230, bounds.size.width - 16, 100) style:UITableViewStylePlain];
  [bulletinTable_ setDataSource:self];
  [bulletinTable_ setDelegate:self];
  [bulletinTable_ setRowHeight:44];
  [[self view] addSubview:bulletinTable_];
  output_ = [[UITextView alloc] initWithFrame:
      CGRectMake(8, 335, bounds.size.width - 16, bounds.size.height - 343)];
  [output_ setEditable:NO];
  [output_ setFont:[UIFont systemFontOfSize:11]];
  [output_ setAutoresizingMask:UIViewAutoresizingFlexibleWidth |
      UIViewAutoresizingFlexibleHeight];
  [[self view] addSubview:output_];
  [self inspect];
}

- (void)dealloc;
{
  [NSObject cancelPreviousPerformRequestsWithTarget:self];
  [self stopObserver];
  [bulletinTable_ setDelegate:nil];
  [bulletinTable_ setDataSource:nil];
  [self finishTask];
}
@end
