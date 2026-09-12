#import <Foundation/Foundation.h>

/* ENILKeychain — single source of truth for the Cloudflare worker URL and
 * shared secret. Implements the read cascade:
 *
 *   1. ENIL_WORKER_URL / ENIL_WORKER_SECRET environment variables (both
 *      must be set for the env-var path to win). Lets a developer point
 *      the app at a local worker without ever touching the keychain.
 *   2. A single SecKeychain internet-password entry with account name
 *      "shared-secret" — written by Preferences, read everywhere else.
 *
 * Values are cached after the first -reload (or first accessor call). Call
 * -reload after a Preferences save to refresh from the keychain.
 *
 * Posts ENILKeychainDidChangeNotification on the main thread after a
 * successful -saveWorkerURL:secret:. */

extern NSString *const ENILKeychainDidChangeNotification;

@interface ENILKeychain : NSObject {
 @private
  NSString *cachedURL_;
  NSString *cachedSecret_;
  BOOL      loaded_;
}

+ (ENILKeychain *)sharedKeychain;

- (NSString *)workerURL;        /* env → keychain → nil */
- (NSString *)workerSecret;     /* env → keychain → nil */
- (BOOL)hasCredentials;         /* YES iff both workerURL and workerSecret resolve */

/* Writes both fields to the keychain (one internet-password entry) and
 * triggers a -reload + change notification. Returns NO on parse/keychain
 * failure. */
- (BOOL)saveWorkerURL:(NSString *)url secret:(NSString *)secret;

/* Drop the cache and re-read on next accessor call. */
- (void)reload;

@end
