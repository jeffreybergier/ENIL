#import "ENILKeychain.h"
#import "XPFoundation.h"
#include <stdlib.h>
#include <string.h>

NSString *const ENILKeychainDidChangeNotification =
  @"ENILKeychainDidChangeNotification";

static NSString *const kAccount = @"shared-secret";
static NSString *const kEnvURL  = @"ENIL_WORKER_URL";
static NSString *const kEnvPw   = @"ENIL_WORKER_SECRET";

static NSString *envOrNil(NSString *name) {
  const char *v = getenv([name UTF8String]);
  if (!v || !*v) return nil;
  return [NSString stringWithUTF8String:v];
}

@implementation ENILKeychain

+ (ENILKeychain *)sharedKeychain;
{
  static ENILKeychain *instance = nil;
  if (!instance) instance = [[ENILKeychain alloc] init];
  return instance;
}

- (void)loadIfNeeded;
{
  if (loaded_) return;
  NSString *url = nil, *pw = nil;
  if ([XPKeychain findInternetPasswordForAccount:kAccount
                                          outURL:&url
                                     outPassword:&pw]) {
    cachedURL_    = [url retain];
    cachedSecret_ = [pw retain];
    ENILLog(@"ENILKeychain.loadIfNeeded",
            @"loaded credentials from keychain (url=%@)", url);
  } else {
    ENILLog(@"ENILKeychain.loadIfNeeded",
            @"no credentials in keychain (account=%@)", kAccount);
  }
  loaded_ = YES;
}

- (NSString *)workerURL;
{
  NSString *env = envOrNil(kEnvURL);
  if ([env length]) return env;
  [self loadIfNeeded];
  return cachedURL_;
}

- (NSString *)workerSecret;
{
  NSString *env = envOrNil(kEnvPw);
  if ([env length]) return env;
  [self loadIfNeeded];
  return cachedSecret_;
}

- (BOOL)hasCredentials;
{
  return [[self workerURL] length] > 0 && [[self workerSecret] length] > 0;
}

- (BOOL)saveWorkerURL:(NSString *)url secret:(NSString *)secret;
{
  if (![url length] || ![secret length]) {
    ENILLog(@"ENILKeychain.saveWorkerURL", @"refusing empty url or secret");
    return NO;
  }
  if (![XPKeychain setInternetPasswordForAccount:kAccount
                                              URL:url
                                         password:secret]) {
    return NO;
  }
  [self reload];
  [[NSNotificationCenter defaultCenter]
    postNotificationName:ENILKeychainDidChangeNotification object:self];
  return YES;
}

- (void)reload;
{
  [cachedURL_ release];    cachedURL_    = nil;
  [cachedSecret_ release]; cachedSecret_ = nil;
  loaded_ = NO;
}

- (void)dealloc;
{
  [cachedURL_ release];
  [cachedSecret_ release];
  [super dealloc];
}

@end
