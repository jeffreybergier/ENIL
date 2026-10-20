/* Isolated native iOS test of ENIL's actual XPKeychain facade.
 * Invoke with seed/read and a unique org.enil.keychain-test.* account.
 * Uses synthetic credentials only; read removes the fixture after checking. */
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import "XPFoundation.h"
#import "ENILKeychain.h"
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\n", \
  __LINE__); failed = 1; goto cleanup; } } while (0)
@interface ENILKeychainErrorStub : NSObject
- (BOOL)containsKey:(NSString *)key error:(NSError **)error;
- (BOOL)setString:(NSString *)value forKey:(NSString *)key error:(NSError **)error;
@end
@implementation ENILKeychainErrorStub
- (BOOL)containsKey:(NSString *)key error:(NSError **)error;
{
  (void)key;
  if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain
      code:errSecInteractionNotAllowed userInfo:nil];
  return NO;
}
- (BOOL)setString:(NSString *)value forKey:(NSString *)key error:(NSError **)error;
{
  (void)value; (void)key;
  if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain
      code:errSecInteractionNotAllowed userInfo:nil];
  return NO;
}
@end

static int CheckErrors(NSString *key)
{
  int failed = 0;
  NSError *error = nil;
  NSString *url = nil, *password = nil;
  Class store = NSClassFromString(@"ENILTemporaryKeychainStore");
  if (!store) store = NSClassFromString(@"AIKeychainStore");
  Method lookup = class_getInstanceMethod(store, @selector(containsKey:error:));
  Method write = class_getInstanceMethod(store, @selector(setString:forKey:error:));
  IMP oldLookup = method_getImplementation(lookup);
  IMP oldWrite = method_getImplementation(write);
  CHECK(![XPKeychain setInternetPasswordForAccount:key URL:@"invalid url"
      password:@"fixture" error:&error]);
  CHECK([[error domain] isEqual:XPKeychainValidationErrorDomain]);
  CHECK([[ENILKeychain messageForSaveError:error] rangeOfString:@"HTTP"].location
      != NSNotFound);
  CHECK(![XPKeychain setInternetPasswordForAccount:key URL:@"ftp://abc123.com"
      password:@"fixture" error:&error]);
  CHECK([[error domain] isEqual:XPKeychainValidationErrorDomain]);
  CHECK(![[ENILKeychain sharedKeychain] saveWorkerURL:@"https://abc123.com"
      secret:@"" error:&error]);
  CHECK([[error domain] isEqual:XPKeychainValidationErrorDomain]);
  CHECK([XPKeychain setInternetPasswordForAccount:key URL:@"https://abc123.com"
      password:@"original fixture" error:&error]);
  CHECK(error == nil);
  method_setImplementation(lookup, method_getImplementation(class_getInstanceMethod(
      [ENILKeychainErrorStub class], @selector(containsKey:error:))));
  CHECK(![XPKeychain setInternetPasswordForAccount:key URL:@"https://abc123.com"
      password:@"replacement" error:&error]);
  CHECK([[error domain] isEqual:NSOSStatusErrorDomain]);
  CHECK([error code] == errSecInteractionNotAllowed);
  method_setImplementation(lookup, oldLookup);
  method_setImplementation(write, method_getImplementation(class_getInstanceMethod(
      [ENILKeychainErrorStub class], @selector(setString:forKey:error:))));
  CHECK(![XPKeychain setInternetPasswordForAccount:key URL:@"https://abc123.com"
      password:@"replacement" error:&error]);
  CHECK([error code] == errSecInteractionNotAllowed);
  CHECK([[ENILKeychain messageForSaveError:error] rangeOfString:@"-25308"].location
      != NSNotFound);
  method_setImplementation(write, oldWrite);
  CHECK([XPKeychain findInternetPasswordForAccount:key outURL:&url
      outPassword:&password]);
  CHECK([password isEqual:@"original fixture"]);
  printf("PASS: validation, native error propagation, and failed-save preservation\n");
cleanup:
  method_setImplementation(lookup, oldLookup);
  method_setImplementation(write, oldWrite);
  [XPKeychain deleteInternetPasswordForAccount:key];
  return failed;
}

int main(int argc, char **argv)
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  int failed = 0;
  NSString *url = nil, *password = nil;
  NSString *key = argc == 3 ? [NSString stringWithUTF8String:argv[2]] : nil;
  BOOL seed = argc == 3 && strcmp(argv[1], "seed") == 0;
  if (![key hasPrefix:@"org.enil.keychain-test."]) {
    [pool drain]; return 2;
  }
  if (CheckErrors([key stringByAppendingString:@".errors"])) {
    [pool drain]; return 1;
  }
  NSMutableDictionary *identity = [NSMutableDictionary dictionaryWithObjectsAndKeys:
      (id)kSecClassInternetPassword, (id)kSecClass,
      key, (id)kSecAttrAccount, nil];
  if (seed) {
    NSMutableDictionary *addition = [[identity mutableCopy] autorelease];
    [addition setObject:@"abc123.com" forKey:(id)kSecAttrServer];
    [addition setObject:(id)kSecAttrProtocolHTTPS forKey:(id)kSecAttrProtocol];
    [addition setObject:[@"old" dataUsingEncoding:NSUTF8StringEncoding]
                forKey:(id)kSecValueData];
    CHECK(SecItemAdd((CFDictionaryRef)addition, NULL) == errSecSuccess);
    CHECK([XPKeychain findInternetPasswordForAccount:key outURL:&url
        outPassword:&password]);
    CHECK([url isEqual:@"https://abc123.com"] && [password isEqual:@"old"]);
    CHECK([XPKeychain setInternetPasswordForAccount:key
        URL:@"https://abc123.com" password:@"abcdefg"]);
    CHECK([XPKeychain findInternetPasswordForAccount:key outURL:&url
        outPassword:&password]);
    CHECK([password isEqual:@"abcdefg"]);
    CHECK([XPKeychain setInternetPasswordForAccount:key
        URL:@"https://replacement.invalid" password:@"updated"]);
    printf("PASS: legacy read, save, and URL replacement\n");
    [pool drain]; return 0;
  }
  CHECK([XPKeychain findInternetPasswordForAccount:key outURL:&url
      outPassword:&password]);
  CHECK([url isEqual:@"https://replacement.invalid"]);
  CHECK([password isEqual:@"updated"]);
  CHECK([XPKeychain deleteInternetPasswordForAccount:key]);
  CHECK(![XPKeychain findInternetPasswordForAccount:key outURL:&url
      outPassword:&password]);
  printf("PASS: credentials survive process exit; fixture removed\n");
cleanup:
  SecItemDelete((CFDictionaryRef)identity);
  [pool drain];
  return failed;
}
