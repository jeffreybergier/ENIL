#import "AppDelegate.h"

#import "UI/MainViewController.h"

@implementation AppDelegate

@synthesize window = window_;

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions;
{
  UIWindow *window;
  MainViewController *controller;

  (void)application;
  (void)launchOptions;

  window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
  [self setWindow:window];

  controller = [[MainViewController alloc] init];
  [[self window] setRootViewController:controller];
  [[self window] makeKeyAndVisible];

  return YES;
}

- (BOOL)application:(UIApplication *)application openURL:(NSURL *)url
    sourceApplication:(NSString *)source annotation:(id)annotation;
{
  (void)application;
  (void)source;
  (void)annotation;
  if (![[url scheme] isEqualToString:@"enil-bulletin-probe"]) return NO;
  MainViewController *controller =
      (MainViewController *)[[self window] rootViewController];
  [controller runCommand:[url host] ?: @"inspect"];
  return YES;
}

@end
