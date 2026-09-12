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

/* ============================================================================
 * XPKeychain — internet-password storage with one entry per account name.
 * The URL is stored in the item's server/protocol/port/path attributes so
 * Keychain Access.app shows it as a normal "internet password" entry.
 * ==========================================================================*/

#if TARGET_OS_IPHONE

static NSString *xpkc_iosSchemeForProto(NSString *proto) {
  return ([proto isEqual:(id)kSecAttrProtocolHTTP]) ? @"http" : @"https";
}

static NSString *xpkc_iosProtoForScheme(NSString *scheme) {
  return [scheme isEqualToString:@"http"]
    ? (NSString *)kSecAttrProtocolHTTP
    : (NSString *)kSecAttrProtocolHTTPS;
}

@implementation XPKeychain

+ (BOOL)findInternetPasswordForAccount:(NSString *)account
                                outURL:(NSString **)outURL
                           outPassword:(NSString **)outPassword;
{
  if (![account length]) return NO;
  NSDictionary *q = [NSDictionary dictionaryWithObjectsAndKeys:
    (id)kSecClassInternetPassword, (id)kSecClass,
    account,                       (id)kSecAttrAccount,
    (id)kCFBooleanTrue,            (id)kSecReturnAttributes,
    (id)kCFBooleanTrue,            (id)kSecReturnData,
    (id)kSecMatchLimitOne,         (id)kSecMatchLimit, nil];
  CFTypeRef cf = NULL;
  OSStatus s = SecItemCopyMatching((CFDictionaryRef)q, &cf);
  if (s != errSecSuccess || !cf) {
    if (s != errSecItemNotFound)
      ENILLog(@"XPKeychain.findInternetPasswordForAccount",
              @"SecItemCopyMatching status=%d account=%@", (int)s, account);
    return NO;
  }
  NSDictionary *r = (NSDictionary *)cf;
  NSString *server = [r objectForKey:(id)kSecAttrServer];
  NSString *path   = [r objectForKey:(id)kSecAttrPath];
  NSNumber *port   = [r objectForKey:(id)kSecAttrPort];
  NSString *proto  = [r objectForKey:(id)kSecAttrProtocol];
  NSData   *pwData = [r objectForKey:(id)kSecValueData];
  if (outURL) {
    NSMutableString *u = [NSMutableString stringWithFormat:@"%@://%@",
                          xpkc_iosSchemeForProto(proto), server ? server : @""];
    if ([port intValue] > 0) [u appendFormat:@":%d", [port intValue]];
    if ([path length])       [u appendString:path];
    *outURL = u;
  }
  if (outPassword) {
    *outPassword = [[[NSString alloc] initWithData:pwData
                                          encoding:NSUTF8StringEncoding] autorelease];
  }
  CFRelease(cf);
  return YES;
}

+ (BOOL)deleteInternetPasswordForAccount:(NSString *)account;
{
  if (![account length]) return NO;
  NSDictionary *q = [NSDictionary dictionaryWithObjectsAndKeys:
    (id)kSecClassInternetPassword, (id)kSecClass,
    account,                       (id)kSecAttrAccount, nil];
  OSStatus s = SecItemDelete((CFDictionaryRef)q);
  return (s == errSecSuccess || s == errSecItemNotFound);
}

+ (BOOL)setInternetPasswordForAccount:(NSString *)account
                                  URL:(NSString *)url
                             password:(NSString *)password;
{
  if (![account length] || ![url length] || !password) return NO;
  NSURL *parsed = [NSURL URLWithString:url];
  if (!parsed || ![[parsed host] length]) {
    ENILLog(@"XPKeychain.setInternetPasswordForAccount",
            @"invalid URL: %@", url);
    return NO;
  }
  [self deleteInternetPasswordForAccount:account];
  NSMutableDictionary *q = [NSMutableDictionary dictionary];
  [q setObject:(id)kSecClassInternetPassword forKey:(id)kSecClass];
  [q setObject:account                       forKey:(id)kSecAttrAccount];
  [q setObject:xpkc_iosProtoForScheme([parsed scheme]) forKey:(id)kSecAttrProtocol];
  [q setObject:[parsed host]                 forKey:(id)kSecAttrServer];
  if ([[parsed path] length]) [q setObject:[parsed path] forKey:(id)kSecAttrPath];
  if ([parsed port])          [q setObject:[parsed port] forKey:(id)kSecAttrPort];
  [q setObject:[password dataUsingEncoding:NSUTF8StringEncoding]
        forKey:(id)kSecValueData];
  OSStatus s = SecItemAdd((CFDictionaryRef)q, NULL);
  if (s != errSecSuccess) {
    ENILLog(@"XPKeychain.setInternetPasswordForAccount",
            @"SecItemAdd status=%d account=%@", (int)s, account);
    return NO;
  }
  return YES;
}

@end

#else /* !TARGET_OS_IPHONE — Mac path, Tiger+ via SecKeychain* */

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

static SecProtocolType xpkc_macProtoForScheme(NSString *scheme) {
  if ([scheme isEqualToString:@"http"]) return kSecProtocolTypeHTTP;
  return kSecProtocolTypeHTTPS;
}

static NSString *xpkc_macSchemeForProto(SecProtocolType p) {
  return (p == kSecProtocolTypeHTTP) ? @"http" : @"https";
}

static NSString *xpkc_macAttrString(SecKeychainAttribute *a) {
  if (!a || !a->data || a->length == 0) return @"";
  return [[[NSString alloc] initWithBytes:a->data
                                   length:a->length
                                 encoding:NSUTF8StringEncoding] autorelease];
}

static NSString *xpkc_macReadURLForItem(SecKeychainItemRef item) {
  UInt32 tags[4] = { kSecServerItemAttr, kSecPathItemAttr,
                     kSecPortItemAttr,   kSecProtocolItemAttr };
  UInt32 fmts[4] = { CSSM_DB_ATTRIBUTE_FORMAT_STRING,
                     CSSM_DB_ATTRIBUTE_FORMAT_STRING,
                     CSSM_DB_ATTRIBUTE_FORMAT_UINT32,
                     CSSM_DB_ATTRIBUTE_FORMAT_UINT32 };
  SecKeychainAttributeInfo info;
  SecKeychainAttributeList *attrs = NULL;
  NSString *server = @"", *path = @"";
  UInt16 port = 0;
  SecProtocolType proto = kSecProtocolTypeHTTPS;
  UInt32 i;

  info.count = 4; info.tag = tags; info.format = fmts;
  if (SecKeychainItemCopyAttributesAndData(item, &info, NULL, &attrs,
                                            NULL, NULL) != errSecSuccess) {
    return nil;
  }
  for (i = 0; i < attrs->count; i++) {
    SecKeychainAttribute *a = &attrs->attr[i];
    if (a->tag == kSecServerItemAttr)   server = xpkc_macAttrString(a);
    else if (a->tag == kSecPathItemAttr) path  = xpkc_macAttrString(a);
    else if (a->tag == kSecPortItemAttr && a->length >= sizeof(UInt32))
      port = (UInt16)(*(UInt32 *)a->data);
    else if (a->tag == kSecProtocolItemAttr && a->length >= sizeof(SecProtocolType))
      proto = *(SecProtocolType *)a->data;
  }
  SecKeychainItemFreeAttributesAndData(attrs, NULL);

  NSMutableString *u = [NSMutableString stringWithFormat:@"%@://%@",
                        xpkc_macSchemeForProto(proto), server];
  if (port > 0)      [u appendFormat:@":%d", (int)port];
  if ([path length]) [u appendString:path];
  return u;
}

@implementation XPKeychain

+ (BOOL)findInternetPasswordForAccount:(NSString *)account
                                outURL:(NSString **)outURL
                           outPassword:(NSString **)outPassword;
{
  const char *acct;
  UInt32 acctLen, pwLen = 0;
  void *pwData = NULL;
  SecKeychainItemRef item = NULL;
  OSStatus s;

  if (![account length]) return NO;
  acct    = [account UTF8String];
  acctLen = (UInt32)strlen(acct);
  s = SecKeychainFindInternetPassword(NULL, 0, NULL, 0, NULL,
                                       acctLen, acct, 0, NULL, 0, 0, 0,
                                       &pwLen, &pwData, &item);
  if (s != errSecSuccess) {
    if (s != errSecItemNotFound)
      ENILLog(@"XPKeychain.findInternetPasswordForAccount",
              @"status=%d account=%@", (int)s, account);
    return NO;
  }
  if (outPassword) {
    *outPassword = [[[NSString alloc] initWithBytes:pwData
                                             length:pwLen
                                           encoding:NSUTF8StringEncoding] autorelease];
  }
  SecKeychainItemFreeContent(NULL, pwData);
  if (outURL && item) *outURL = xpkc_macReadURLForItem(item);
  if (item) CFRelease(item);
  return YES;
}

+ (BOOL)deleteInternetPasswordForAccount:(NSString *)account;
{
  const char *acct;
  UInt32 acctLen;
  SecKeychainItemRef item = NULL;
  OSStatus s;

  if (![account length]) return NO;
  acct    = [account UTF8String];
  acctLen = (UInt32)strlen(acct);
  s = SecKeychainFindInternetPassword(NULL, 0, NULL, 0, NULL,
                                       acctLen, acct, 0, NULL, 0, 0, 0,
                                       NULL, NULL, &item);
  if (s == errSecItemNotFound) return YES;
  if (s != errSecSuccess || !item) {
    ENILLog(@"XPKeychain.deleteInternetPasswordForAccount",
            @"find status=%d account=%@", (int)s, account);
    return NO;
  }
  s = SecKeychainItemDelete(item);
  CFRelease(item);
  if (s != errSecSuccess) {
    ENILLog(@"XPKeychain.deleteInternetPasswordForAccount",
            @"delete status=%d account=%@", (int)s, account);
    return NO;
  }
  return YES;
}

+ (BOOL)setInternetPasswordForAccount:(NSString *)account
                                  URL:(NSString *)url
                             password:(NSString *)password;
{
  NSURL *parsed;
  const char *acct, *server, *path, *pw;
  UInt32 acctLen, serverLen, pathLen, pwLen;
  UInt16 port;
  SecProtocolType proto;
  OSStatus s;

  if (![account length] || ![url length] || !password) return NO;
  parsed = [NSURL URLWithString:url];
  if (!parsed || ![[parsed host] length]) {
    ENILLog(@"XPKeychain.setInternetPasswordForAccount",
            @"invalid URL: %@", url);
    return NO;
  }
  [self deleteInternetPasswordForAccount:account];

  acct      = [account UTF8String];
  server    = [[parsed host] UTF8String];
  path      = [[parsed path] length] ? [[parsed path] UTF8String] : "";
  pw        = [password UTF8String];
  acctLen   = (UInt32)strlen(acct);
  serverLen = (UInt32)strlen(server);
  pathLen   = (UInt32)strlen(path);
  pwLen     = (UInt32)strlen(pw);
  port      = [[parsed port] unsignedShortValue];
  proto     = xpkc_macProtoForScheme([parsed scheme]);

  s = SecKeychainAddInternetPassword(NULL,
        serverLen, server,
        0, NULL,
        acctLen, acct,
        pathLen, path,
        port, proto, 0,
        pwLen, pw, NULL);
  if (s != errSecSuccess) {
    ENILLog(@"XPKeychain.setInternetPasswordForAccount",
            @"SecKeychainAddInternetPassword status=%d account=%@",
            (int)s, account);
    return NO;
  }
  return YES;
}

@end

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif /* TARGET_OS_IPHONE */
