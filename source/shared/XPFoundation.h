#import <Foundation/Foundation.h>

/* Objective-C logging convenience macro. Prefer this over NSLog at ObjC call
 * sites so every log line carries the same "[<tag>] " prefix. The tag is an
 * NSString literal; the format string may use any Cocoa specifier (%@, %d,
 * %lld, …) since this is a direct NSLog passthrough — no C bridge involved
 * (that is enil_cocoa_log.h's enil_log(), for code that can't call NSLog).
 *
 * Examples:
 *   ENILLog(@"ENILAccount.startSync",      @"engine kick");
 *   ENILLog(@"ChatWindowController.init",  @"chat=%@", chatId); */
#define ENILLog(tag, fmt, ...) NSLog((@"[%@] " fmt), (tag), ##__VA_ARGS__)

/* Integer Compatibility — Tiger 10.4 SDK predates NSInteger/NSUInteger.
 * The defined() guard keeps us on the modern branch when MAC_OS_X_VERSION_MAX_ALLOWED
 * is not declared (iOS), where NSInteger/NSUInteger have always existed. */
#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED < 1050
  #define XPInteger  int
  #define XPUInteger unsigned int
#else
  #define XPInteger  NSInteger
  #define XPUInteger NSUInteger
#endif

/* Run-loop common-modes constant — Tiger compatibility.
 *
 * NSRunLoopCommonModes is a 10.5+ Foundation symbol (NSRunLoop.h marks it
 * AVAILABLE_MAC_OS_X_VERSION_10_5_AND_LATER). Built against the 10.5 SDK with a
 * 10.4 deployment floor it weak-imports, so on Tiger dyld binds it to NULL and
 * loading its value reads address 0 -> EXC_BAD_ACCESS (KERN_PROTECTION_FAILURE
 * at 0x0). A compile-time #if can't help here: MAC_OS_X_VERSION_MAX_ALLOWED is
 * 1050 regardless of which OS the one fat binary actually runs on. The
 * toll-free-bridged CoreFoundation constant kCFRunLoopCommonModes has existed
 * since 10.0 as a hard (non-weak) symbol and is the same object the NS constant
 * points to, so route through it for a value valid on every target. This shared
 * file is MRC on both targets, so the bridge cast needs no __bridge. */
#define XPRunLoopCommonModes ((NSString *)kCFRunLoopCommonModes)

/* Foundation-only Tiger compatibility shims.
 *
 * Lives in source/shared/ so it can be imported from any shared code
 * (ENILAccount, etc.) without dragging in AppKit. The AppKit-touching
 * counterpart is source/macOS/XPAppKit.{h,m}.
 *
 * iOS 4.3 already has every modern method we wrap, so these shims are
 * macOS-only in practice — but the file imports nothing platform-specific,
 * so an iOS target can include it harmlessly. */

@interface NSFileManager (XPFoundation)

/* Wrappers for 10.5+ APIs that don't exist on Tiger 10.4. On modern systems
 * each method invokes the modern selector via NSInvocation; on Tiger each
 * falls back to the deprecated handler-based variant. Errors on the Tiger
 * fallback path are reported in NSCocoaErrorDomain. */
- (BOOL)XP_createDirectoryAtPath:(NSString *)path
     withIntermediateDirectories:(BOOL)createIntermediates
                      attributes:(NSDictionary *)attributes
                           error:(NSError **)error;
- (BOOL)XP_copyItemAtPath:(NSString *)src
                   toPath:(NSString *)dst
                    error:(NSError **)error;
- (BOOL)XP_removeItemAtPath:(NSString *)path
                      error:(NSError **)error;
/* Returns the shallow contents (names only) of a directory, or nil on error.
 * 10.5+: contentsOfDirectoryAtPath:error:; Tiger: directoryContentsAtPath:. */
- (NSArray *)XP_contentsOfDirectoryAtPath:(NSString *)path
                                    error:(NSError **)error;
/* Move to the user's Trash (macOS) so an accidental Log Out is recoverable.
 * iOS has no Trash, so it falls back to a hard recursive delete — as does
 * macOS if the volume has no Trash. Returns YES if the item is gone from
 * `path` by either route (absent already counts as success). */
- (BOOL)XP_trashItemAtPath:(NSString *)path
                      error:(NSError **)error;
@end

/* String Helpers for Human-Readable sizes */
@interface NSString (XPByteCount)
+ (NSString *)XP_stringFromByteCount:(long long)bytes;
@end

@interface NSString (XPPercentEncoding)
- (NSString *)XP_stringByRemovingPercentEncoding;
@end

/* Internet-password keychain shim. Three operations, all keyed by an opaque
 * account name (callers pick a fixed string per stored credential, e.g.
 * "shared-secret"). The URL is stored as the item's server/protocol/port/path
 * attributes so it round-trips through Keychain Access.app as a real labelled
 * "Internet password for <host>" entry — no parallel NSUserDefaults required.
 *
 * Mac (Tiger+): SecKeychainAddInternetPassword / SecKeychainFindInternetPassword.
 *               Both deprecated since 10.10 but the only API Tiger ships, so
 *               the deprecated-declarations pragma earns its keep here.
 * iOS:          SecItemAdd / SecItemCopyMatching with kSecClassInternetPassword. */
@interface XPKeychain : NSObject
+ (BOOL)findInternetPasswordForAccount:(NSString *)account
                                outURL:(NSString **)outURL
                           outPassword:(NSString **)outPassword;
+ (BOOL)setInternetPasswordForAccount:(NSString *)account
                                  URL:(NSString *)url
                             password:(NSString *)password;
+ (BOOL)deleteInternetPasswordForAccount:(NSString *)account;
@end
