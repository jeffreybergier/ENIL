#import "MainViewController.h"
#import <objc/runtime.h>
#import <dlfcn.h>
#import <unistd.h>

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

@implementation MainViewController

- (void)record:(NSString *)message;
{
  NSString *line = [NSString stringWithFormat:@"%@ %@\n", [NSDate date], message];
  [log_ appendString:line];
  [output_ setText:log_];
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
    UIApplication *app = [UIApplication sharedApplication];
    if ([app respondsToSelector:@selector(registerUserNotificationSettings:)]) {
      [app registerUserNotificationSettings:[UIUserNotificationSettings
          settingsForTypes:(UIUserNotificationTypeAlert |
              UIUserNotificationTypeSound | UIUserNotificationTypeBadge)
                categories:nil]];
    }
    UILocalNotification *note = [[UILocalNotification alloc] init];
    [note setAlertBody:@"Public control: synthetic UILocalNotification"];
    [note setSoundName:UILocalNotificationDefaultSoundName];
    [note setApplicationIconBadgeNumber:1];
    [note setFireDate:[NSDate dateWithTimeIntervalSinceNow:15]];
    [app scheduleLocalNotification:note];
    [self record:@"Public control scheduled +15s; allow alerts and press Home"];
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
      @"publish", @"withdraw", @"clear", nil];
  [self runCommand:[commands objectAtIndex:(NSUInteger)[sender tag]]];
}

- (void)viewDidLoad;
{
  [super viewDidLoad];
  task_ = UIBackgroundTaskInvalid;
  log_ = [[NSMutableString alloc] init];
  [[self view] setBackgroundColor:[UIColor whiteColor]];
  NSArray *titles = [NSArray arrayWithObjects:@"Inspect", @"Public +15s",
      @"Private +8s", @"Withdraw", @"Clear public", nil];
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
  output_ = [[UITextView alloc] initWithFrame:
      CGRectMake(8, 120, bounds.size.width - 16, bounds.size.height - 128)];
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
  [self finishTask];
}
@end
