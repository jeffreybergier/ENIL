/* Actual Settings UI with synthetic accounts and a fake keychain. No account
 * initialization, network traffic, credentials, or production data access. */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import "../iOS/PreferencesViewController.h"
#import "../shared/ENILKeychain.h"
#include <assert.h>
static void Result(NSString *text)
{
  [text writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:
      @"Documents/settings-result.txt"] atomically:YES
      encoding:NSUTF8StringEncoding error:NULL];
}
static void Failed(NSException *exception)
{ Result([@"FAIL: " stringByAppendingString:[exception name]]); }
@interface FakeKeychain : NSObject
+ (id)sharedKeychain;
@end
@implementation FakeKeychain
+ (id)sharedKeychain { return [[self alloc] init]; }
- (NSString *)workerURL { return @"https://fixture.invalid"; }
- (NSString *)workerSecret { return @"synthetic"; }
@end
@interface FakeCoordinator : NSObject
@property(nonatomic, copy) NSArray *summaries;
@end
@implementation FakeCoordinator
- (NSArray *)accountSummaries { return [self summaries]; }
@end
@interface SettingsFixture : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) UINavigationController *navigation;
@property(nonatomic, strong) PreferencesViewController *settings;
@property(nonatomic, strong) FakeCoordinator *coordinator;
@property(nonatomic, assign) NSUInteger stage;
@end
@implementation SettingsFixture
- (void)push;
{
  [self setSettings:[[PreferencesViewController alloc] init]];
  [[self settings] setCoordinator:(id)[self coordinator]];
  [[self navigation] pushViewController:[self settings] animated:YES];
  [self performSelector:@selector(check) withObject:nil afterDelay:1];
}
- (void)check;
{
  UITableView *table = [[self settings] tableView];
  NSUInteger expected = [self stage] == 0 ? 3 : ([self stage] == 1 ? 1 : 4);
  assert((NSUInteger)[table numberOfRowsInSection:0] == expected);
  assert([[[[self settings] collectionTableAdapter] snapshot] countInSection:0] == expected);
  assert((NSUInteger)[[self settings] tableView:table numberOfRowsInSection:0] == expected);
  if ([self stage] == 2) {
    Result(@"PASS: active, empty, and multiple accounts match table and snapshot after appearance\n");
    return;
  }
  [self setStage:[self stage] + 1];
  [[self coordinator] setSummaries:[self stage] == 1 ? @[] : @[
      @{@"path":@"fixture-a", @"displayName":@"A", @"active":@YES},
      @{@"path":@"fixture-b", @"displayName":@"B", @"active":@NO}]];
  /* Exercise returning to an existing settings controller after account changes. */
  [[self settings] viewWillAppear:NO];
  [self performSelector:@selector(check) withObject:nil afterDelay:0.5];
}
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options;
{
  (void)app; (void)options;
  Result(@"RUNNING\n");
  NSSetUncaughtExceptionHandler(Failed);
  method_setImplementation(class_getClassMethod([ENILKeychain class], @selector(sharedKeychain)),
      method_getImplementation(class_getClassMethod([FakeKeychain class], @selector(sharedKeychain))));
  [self setCoordinator:[[FakeCoordinator alloc] init]];
  [[self coordinator] setSummaries:@[@{@"path":@"fixture-a", @"displayName":@"A", @"active":@YES}]];
  [self setWindow:[[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]]];
  [self setNavigation:[[UINavigationController alloc]
      initWithRootViewController:[[UIViewController alloc] init]]];
  [[self window] setRootViewController:[self navigation]];
  [[self window] makeKeyAndVisible];
  [self performSelector:@selector(push) withObject:nil afterDelay:0.3];
  return YES;
}
@end
int main(int argc, char **argv)
{ @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"SettingsFixture"); } }
