#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
/* Temporary override; remove with source/iOS/ENILTemporaryKeychainStore.m. */
#define AIKeychainStore ENILTemporaryKeychainStore
#endif
#import <AltivecCocoa/AIKeychainStore.h>
#import "XPFoundation.h"
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <Security/Security.h>
#else
#import <CoreServices/CoreServices.h>
#import <Security/Security.h>
#endif

/* errSecSuccess is 10.6+; on Tiger only noErr is available. Both equal 0. */
#ifndef errSecSuccess
#define errSecSuccess 0
#endif

static NSError *XP_fileError(NSInteger code, NSString *description) {
  NSDictionary *info = description
    ? [NSDictionary dictionaryWithObject:description
                                  forKey:NSLocalizedDescriptionKey]
    : nil;
  return [NSError errorWithDomain:NSCocoaErrorDomain
                             code:code
                         userInfo:info];
}

@implementation NSFileManager (XPFoundation)

- (BOOL)XP_createDirectoryAtPath:(NSString *)path
     withIntermediateDirectories:(BOOL)createIntermediates
                      attributes:(NSDictionary *)attributes
                           error:(NSError **)error;
{
  if (!path) return NO;
  SEL modern = @selector(createDirectoryAtPath:withIntermediateDirectories:attributes:error:);
  if ([self respondsToSelector:modern]) {
    NSMethodSignature *sig = [self methodSignatureForSelector:modern];
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
    [inv setSelector:modern];
    [inv setTarget:self];
    [inv setArgument:&path               atIndex:2];
    [inv setArgument:&createIntermediates atIndex:3];
    [inv setArgument:&attributes         atIndex:4];
    [inv setArgument:&error              atIndex:5];
    [inv invoke];
    BOOL ok = NO;
    [inv getReturnValue:&ok];
    return ok;
  }

  /* Tiger fallback: manually walk path components. */
  BOOL isDir;
  NSArray *parts = createIntermediates ? [path pathComponents]
                                       : [NSArray arrayWithObject:path];
  NSString *cur = nil;
  NSUInteger i, n = [parts count];
  for (i = 0; i < n; i++) {
    NSString *p = [parts objectAtIndex:i];
    cur = (cur == nil) ? p : [cur stringByAppendingPathComponent:p];
    if ([self fileExistsAtPath:cur isDirectory:&isDir]) {
      if (isDir) continue;
      if (error) *error = XP_fileError(NSFileWriteUnknownError,
        [NSString stringWithFormat:@"Path exists but is not a directory: %@", cur]);
      return NO;
    }
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    BOOL ok = [self createDirectoryAtPath:cur attributes:attributes];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    if (!ok) {
      if (error) *error = XP_fileError(NSFileWriteUnknownError,
        [NSString stringWithFormat:@"Cannot create directory: %@", cur]);
      return NO;
    }
  }
  return YES;
}

- (BOOL)XP_copyItemAtPath:(NSString *)src
                   toPath:(NSString *)dst
                    error:(NSError **)error;
{
  if (!src || !dst) return NO;
  SEL modern = @selector(copyItemAtPath:toPath:error:);
  if ([self respondsToSelector:modern]) {
    NSMethodSignature *sig = [self methodSignatureForSelector:modern];
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
    [inv setSelector:modern];
    [inv setTarget:self];
    [inv setArgument:&src   atIndex:2];
    [inv setArgument:&dst   atIndex:3];
    [inv setArgument:&error atIndex:4];
    [inv invoke];
    BOOL ok = NO;
    [inv getReturnValue:&ok];
    return ok;
  }
#if TARGET_OS_IPHONE
  /* Unreachable on iOS: the modern copyItemAtPath:toPath:error: above always
     responds. The Tiger handler-based copyPath:toPath:handler: was removed
     from the iOS SDK, so even a compiled reference warns
     (-Wobjc-method-access). */
  BOOL ok = NO;
#else
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  BOOL ok = [self copyPath:src toPath:dst handler:nil];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#endif
  if (!ok && error) {
    *error = XP_fileError(NSFileWriteUnknownError,
      [NSString stringWithFormat:@"Cannot copy %@ to %@", src, dst]);
  }
  return ok;
}

- (BOOL)XP_removeItemAtPath:(NSString *)path
                      error:(NSError **)error;
{
  if (!path) return NO;
  SEL modern = @selector(removeItemAtPath:error:);
  if ([self respondsToSelector:modern]) {
    NSMethodSignature *sig = [self methodSignatureForSelector:modern];
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
    [inv setSelector:modern];
    [inv setTarget:self];
    [inv setArgument:&path  atIndex:2];
    [inv setArgument:&error atIndex:3];
    [inv invoke];
    BOOL ok = NO;
    [inv getReturnValue:&ok];
    return ok;
  }
#if TARGET_OS_IPHONE
  /* Unreachable on iOS: the modern removeItemAtPath:error: above always
     responds. The Tiger handler-based removeFileAtPath:handler: was removed
     from the iOS SDK, so even a compiled reference warns
     (-Wobjc-method-access). */
  BOOL ok = NO;
#else
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  BOOL ok = [self removeFileAtPath:path handler:nil];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#endif
  if (!ok && error) {
    *error = XP_fileError(NSFileWriteUnknownError,
      [NSString stringWithFormat:@"Cannot remove %@", path]);
  }
  return ok;
}

- (NSArray *)XP_contentsOfDirectoryAtPath:(NSString *)path
                                    error:(NSError **)error;
{
  if (!path) return nil;
  SEL modern = @selector(contentsOfDirectoryAtPath:error:);
  if ([self respondsToSelector:modern]) {
    NSMethodSignature *sig = [self methodSignatureForSelector:modern];
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
    [inv setSelector:modern];
    [inv setTarget:self];
    [inv setArgument:&path  atIndex:2];
    [inv setArgument:&error atIndex:3];
    [inv invoke];
    NSArray *result = nil;
    [inv getReturnValue:&result];
    return result;
  }
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  NSArray *result = [self directoryContentsAtPath:path];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
  if (!result && error) {
    *error = XP_fileError(NSFileReadUnknownError,
      [NSString stringWithFormat:@"Cannot list directory: %@", path]);
  }
  return result;
}

- (BOOL)XP_trashItemAtPath:(NSString *)path
                      error:(NSError **)error;
{
  if (!path) return NO;
#if TARGET_OS_IPHONE
  /* No Trash on iOS. Log Out's intent is "this data is gone", so a hard
     delete is the correct behaviour here, not an error. */
  return [self XP_removeItemAtPath:path error:error];
#else
  /* Tiger-safe Trash: FSMoveObjectToTrashSync is 10.5+, but the PPC slice
     runs on 10.4 — FSFindFolder(kTrashFolderType)+FSMoveObject exist there.
     Any failure (no Trash on the volume, a name clash with a prior logout of
     the same mid, …) falls back to a hard delete: the user already confirmed
     permanent deletion, so the account must end up gone either way. */
  {
    FSRef src, trash;
    OSErr e;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (FSPathMakeRef((const UInt8 *)[path fileSystemRepresentation],
                      &src, NULL) != noErr) {
      e = noErr; /* path already absent — idempotent success */
    } else {
      e = FSFindFolder(kUserDomain, kTrashFolderType, kCreateFolder, &trash);
      if (e == noErr) e = FSMoveObject(&src, &trash, NULL);
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    if (e == noErr) return YES;
    ENILLog(@"XPFoundation.XP_trashItemAtPath", @"trash failed (OSErr %d) - "
          @"hard-deleting %@", (int)e, path);
  }
  return [self XP_removeItemAtPath:path error:error];
#endif
}

@end

@implementation NSString (XPByteCount)

+ (NSString *)XP_stringFromByteCount:(long long)bytes;
{
  if (bytes < 1024) return [NSString stringWithFormat:@"%lld B", bytes];
  double count = (double)bytes;
  NSArray *units = [NSArray arrayWithObjects:@"B", @"KB", @"MB", @"GB", nil];
  int i = 0;
  while (count >= 1024 && (NSUInteger)i < [units count] - 1) {
    count /= 1024.0;
    i++;
  }
  return [NSString stringWithFormat:@"%.2f %@", count, [units objectAtIndex:(NSUInteger)i]];
}

@end

@implementation NSString (XPPercentEncoding)

- (NSString *)XP_stringByRemovingPercentEncoding;
{
  SEL sel = @selector(stringByRemovingPercentEncoding);

  if ([self respondsToSelector:sel])
    return [self performSelector:sel];

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  return [self stringByReplacingPercentEscapesUsingEncoding:NSUTF8StringEncoding];
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

@end

NSString *const XPKeychainValidationErrorDomain = @"ENILKeychainValidation";

static BOOL XPKeychainInputError(NSError **error, int code, NSString *message)
{
  if (error) *error = [NSError errorWithDomain:XPKeychainValidationErrorDomain
      code:code userInfo:[NSDictionary dictionaryWithObject:message
      forKey:NSLocalizedDescriptionKey]];
  return NO;
}

/* App-owned account-only discovery and URL replacement policy. */
@implementation XPKeychain

+ (BOOL)findInternetPasswordForAccount:(NSString *)account
                              outURL:(NSString **)outURL
                         outPassword:(NSString **)outPassword;
{
  AIKeychainStore *store = [AIKeychainStore internetPasswordStoreForKey:account
                                                              error:NULL];
  NSString *password;
  if (outURL) *outURL = nil;
  if (outPassword) *outPassword = nil;
  if (!store) return NO;
  password = [store stringForKey:account error:NULL];
  if (!password) return NO;
  if (outURL) *outURL = [store serverURLString];
  if (outPassword) *outPassword = password;
  return YES;
}

+ (BOOL)deleteInternetPasswordForAccount:(NSString *)account;
{
  return [self deleteInternetPasswordForAccount:account error:NULL];
}

+ (BOOL)deleteInternetPasswordForAccount:(NSString *)account
                                 error:(NSError **)outError;
{
  unsigned int attempt;
  if (outError) *outError = nil;
  for (attempt = 0; attempt < 128; attempt++) {
    NSError *error = nil;
    AIKeychainStore *store = [AIKeychainStore internetPasswordStoreForKey:account
                                                                error:&error];
    if (!store) {
      if ([error code] == errSecItemNotFound) return YES;
      if (outError) *outError = error;
      return NO;
    }
    if (![store removeItemForKey:account error:&error] &&
        [error code] != errSecItemNotFound) {
      if (outError) *outError = error;
      return NO;
    }
#if !TARGET_OS_IPHONE
    /* Preserve the old Mac first-match policy; iOS removed all account matches. */
    return YES;
#endif
  }
  if (outError) *outError = [NSError errorWithDomain:NSOSStatusErrorDomain
      code:errSecDuplicateItem userInfo:nil];
  return NO; /* Bound concurrent delete/recreate races. */
}

+ (BOOL)setInternetPasswordForAccount:(NSString *)account
                                URL:(NSString *)url
                           password:(NSString *)password;
{
  return [self setInternetPasswordForAccount:account URL:url password:password
                                      error:NULL];
}

+ (BOOL)setInternetPasswordForAccount:(NSString *)account
                                URL:(NSString *)url
                           password:(NSString *)password
                              error:(NSError **)error;
{
  NSURL *parsed;
  NSError *lookupError = nil;
  AIKeychainAuthentication authentication;
  AIKeychainStore *store;
  if (error) *error = nil;
  if (![account length] || ![url length] || ![password length])
    return XPKeychainInputError(error, 1, NSLocalizedString(
        @"Worker URL and Shared Secret must both be set.", nil));
  parsed = [NSURL URLWithString:url];
  NSString *scheme = [[parsed scheme] lowercaseString];
  if (![[parsed host] length] ||
      (![scheme isEqualToString:@"http"] && ![scheme isEqualToString:@"https"]) ||
      [url rangeOfCharacterFromSet:[NSCharacterSet
          whitespaceAndNewlineCharacterSet]].location != NSNotFound ||
      [[parsed port] intValue] < 0 || [[parsed port] intValue] > 65535)
    return XPKeychainInputError(error, 2, NSLocalizedString(
        @"Enter a valid HTTP or HTTPS Worker URL, including its host name.", nil));
#if TARGET_OS_IPHONE
  authentication = AIKeychainAuthenticationDefault;
#else
  authentication = AIKeychainAuthenticationUnspecified;
#endif
  store = [[[AIKeychainStore alloc] initWithServer:parsed
      protocolType:[scheme isEqualToString:@"http"] ?
          AIKeychainProtocolHTTP : AIKeychainProtocolHTTPS
      authenticationType:authentication securityDomain:nil] autorelease];
  if (!store) return XPKeychainInputError(error, 2, NSLocalizedString(
      @"Enter a valid HTTP or HTTPS Worker URL, including its host name.", nil));
  /* Never delete an old entry when lookup failed due to access/lock errors. */
  if ([store containsKey:account error:&lookupError])
    return [store setString:password forKey:account error:error];
  if ([lookupError code] != errSecItemNotFound) {
    if (error) *error = lookupError;
    return NO;
  }
  if (![self deleteInternetPasswordForAccount:account error:error]) return NO;
  return [store setString:password forKey:account error:error];
}

@end
