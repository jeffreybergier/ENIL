/* ============================================================================
 * Apple-frameworks bridge. The single translation unit in source/shared where
 * Mac framework code is allowed to live; the matching pure-C headers (the
 * enil_cocoa_*.h family below) stay framework-free so the portable
 * libcurl/sqlite/cJSON core never pulls in a framework. A non-Mac port
 * reimplements only this file.
 *
 * Five responsibilities, each behind its own narrow pure-C header — all named
 * enil_cocoa_* so the include line advertises that this is where they live:
 *   - enil_cocoa_image.h    : ImageIO JPEG transcode (thumbnails + full-res)
 *   - enil_cocoa_date.h     : CFDateFormatter localized timestamps
 *   - enil_cocoa_collate.h  : locale-aware sqlite collation (CFStringCompare)
 *   - enil_cocoa_log.h      : enil_log() — printf-style logging through NSLog
 *   - enil_cocoa_progress.h : enil_progress_post()/enil_status_post() — sync
 *                             status fanned out as NSNotifications (main thread)
 *
 * Compiled as Objective-C (.m) because the logging/notification halves need
 * NSLog and NSNotificationCenter; the image/date/collation half is plain
 * CoreFoundation C and would compile either way. This is why no .c file in
 * the tree ever imports a Mac framework — that code all lives here.
 * ==========================================================================*/

#import <Foundation/Foundation.h>
/* ImageIO is a sub-framework of ApplicationServices on 10.5 SDK.
   The -F flag in the Makefile points the compiler to it. */
#include <ImageIO/ImageIO.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#import  "XPFoundation.h"   /* ENILLog macro (used by the progress logger below) */
#include "enil_cocoa_image.h"
#include "enil_cocoa_date.h"
#include "enil_cocoa_collate.h"
#include "enil_cocoa_log.h"
#include "enil_cocoa_progress.h"

#define THUMB_MAX_PX 600

/* ============================================================================
 * Build a file:// CFURL from a UTF-8 POSIX path. Returns NULL on failure.
 * ==========================================================================*/
static CFURLRef url_from_path(const char *path) {
  CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
  if (!s) return NULL;
  CFURLRef u = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, false);
  CFRelease(s);
  return u;
}

/* ============================================================================
 * Read an int value from a CFDictionary; 0 if the key is missing.
 * ==========================================================================*/
static int cf_int(CFDictionaryRef d, CFStringRef key) {
  CFNumberRef n = (CFNumberRef)CFDictionaryGetValue(d, key);
  int v = 0;
  if (n) CFNumberGetValue(n, kCFNumberIntType, &v);
  return v;
}

/* ============================================================================
 * Build the CGImage to write — either a thumbnail bounded by max_px or the
 * full-resolution image at index 0.
 * ==========================================================================*/
static CGImageRef build_source_image(CGImageSourceRef isrc, int max_px) {
  if (max_px <= 0) return CGImageSourceCreateImageAtIndex(isrc, 0, NULL);
  {
    CFStringRef keys[]   = { kCGImageSourceThumbnailMaxPixelSize,
                              kCGImageSourceCreateThumbnailFromImageAlways };
    int maxPx = max_px;
    CFNumberRef maxNum = CFNumberCreate(NULL, kCFNumberIntType, &maxPx);
    CFTypeRef   values[] = { maxNum, kCFBooleanTrue };
    CFDictionaryRef topts = CFDictionaryCreate(NULL,
        (const void **)keys, (const void **)values, 2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CGImageRef img;
    CFRelease(maxNum);
    img = CGImageSourceCreateThumbnailAtIndex(isrc, 0, topts);
    CFRelease(topts);
    return img;
  }
}

/* ============================================================================
 * Write a CGImage to dst_url as JPEG at 0.85 quality. Returns 0 on success.
 * ==========================================================================*/
static int write_jpeg(CGImageRef img, CFURLRef dst_url) {
  CGImageDestinationRef dst = NULL;
  CFDictionaryRef dopts = NULL;
  float quality = 0.85f;
  CFNumberRef qNum;
  CFStringRef dkeys[]   = { kCGImageDestinationLossyCompressionQuality };
  CFTypeRef   dvalues[2];
  int rc = -1;

  dst = CGImageDestinationCreateWithURL(dst_url, CFSTR("public.jpeg"), 1, NULL);
  if (!dst) return -1;
  qNum = CFNumberCreate(NULL, kCFNumberFloatType, &quality);
  dvalues[0] = qNum;
  dopts = CFDictionaryCreate(NULL, (const void **)dkeys, (const void **)dvalues, 1,
                             &kCFTypeDictionaryKeyCallBacks,
                             &kCFTypeDictionaryValueCallBacks);
  CFRelease(qNum);

  CGImageDestinationAddImage(dst, img, dopts);
  if (CGImageDestinationFinalize(dst)) rc = 0;

  CFRelease(dopts);
  CFRelease(dst);
  return rc;
}

/* ============================================================================
 * Transcode src_path to JPEG at dst_path. Behavior of max_px:
 *   max_px > 0 → thumbnail bounded by max_px on longest edge
 *   max_px = 0 → full-resolution copy
 * Fills info with both original and output dimensions.
 * ==========================================================================*/
int enil_image_jpeg_create(const char *src_path, const char *dst_path,
                           int max_px, ENILThumbInfo *info) {
  int result = -1;
  CFURLRef src_url = NULL;
  CFURLRef dst_url = NULL;
  CGImageSourceRef isrc = NULL;
  CGImageRef img = NULL;
  CFDictionaryRef props = NULL;

  if (!src_path || !dst_path || !info) return -1;
  memset(info, 0, sizeof(*info));

  src_url = url_from_path(src_path);
  dst_url = url_from_path(dst_path);
  if (!src_url || !dst_url) goto done;

  isrc = CGImageSourceCreateWithURL(src_url, NULL);
  if (!isrc) goto done;

  props = CGImageSourceCopyPropertiesAtIndex(isrc, 0, NULL);
  if (props) {
    info->orig_width  = cf_int(props, kCGImagePropertyPixelWidth);
    info->orig_height = cf_int(props, kCGImagePropertyPixelHeight);
  }

  img = build_source_image(isrc, max_px);
  if (!img) goto done;

  info->thumb_width  = (int)CGImageGetWidth(img);
  info->thumb_height = (int)CGImageGetHeight(img);

  if (write_jpeg(img, dst_url) == 0) result = 0;

done:
  if (img)     CGImageRelease(img);
  if (props)   CFRelease(props);
  if (isrc)    CFRelease(isrc);
  if (dst_url) CFRelease(dst_url);
  if (src_url) CFRelease(src_url);
  return result;
}

/* ============================================================================
 * Backwards-compatible thumbnail wrapper around enil_image_jpeg_create.
 * ==========================================================================*/
int enil_thumb_generate(const char *src_path, const char *thumb_path,
                        ENILThumbInfo *info) {
  return enil_image_jpeg_create(src_path, thumb_path, THUMB_MAX_PX, info);
}

/* ============================================================================
 * Localized timestamp formatting. Behaviour is identical to the CFDateFormatter
 * machinery formerly inlined in enil_html.c (current locale, medium date /
 * short time); only its home moved here so the renderer stays portable.
 * ==========================================================================*/
int enil_format_date_string(long long epoch_ms, char *out, size_t out_size) {
  CFLocaleRef locale;
  CFDateFormatterRef fmt;
  CFAbsoluteTime seconds;
  CFDateRef date;
  CFStringRef text;
  Boolean ok;

  if (!out || out_size == 0 || epoch_ms <= 0) return -1;
  locale = CFLocaleCopyCurrent();
  fmt = CFDateFormatterCreate(NULL, locale,
    kCFDateFormatterMediumStyle, kCFDateFormatterShortStyle);
  if (locale) CFRelease(locale);
  if (!fmt) return -1;

  seconds = ((CFAbsoluteTime)epoch_ms / 1000.0) -
    kCFAbsoluteTimeIntervalSince1970;
  date = CFDateCreate(NULL, seconds);
  if (!date) { CFRelease(fmt); return -1; }
  text = CFDateFormatterCreateStringWithDate(NULL, fmt, date);
  CFRelease(date);
  CFRelease(fmt);
  if (!text) return -1;

  ok = CFStringGetCString(text, out, (CFIndex)out_size, kCFStringEncodingUTF8);
  CFRelease(text);
  return ok ? 0 : -1;
}

/* ============================================================================
 * Byte-order fallback so the collation stays a total order even when a buffer
 * is not valid UTF-8 (CFStringCreateWithBytes would return NULL otherwise).
 * ==========================================================================*/
static int byte_collate(int la, const void *a, int lb, const void *b) {
  int n = la < lb ? la : lb;
  int r = n > 0 ? memcmp(a, b, (size_t)n) : 0;
  return r != 0 ? r : la - lb;
}

/* ============================================================================
 * Locale-aware, case-insensitive, numeric-aware UTF-8 collation for sqlite.
 * Registered via sqlite3_create_collation and called by the query engine
 * during ORDER BY; buffers are length-delimited and NOT NUL-terminated.
 * ==========================================================================*/
int enil_localized_collate(void *ctx, int la, const void *a, int lb, const void *b) {
  CFStringCompareFlags flags = kCFCompareCaseInsensitive | kCFCompareLocalized
                             | kCFCompareNonliteral | kCFCompareNumerically;
  CFStringRef sa, sb;
  CFComparisonResult r;
  (void)ctx;
  sa = CFStringCreateWithBytes(NULL, (const UInt8 *)a, (CFIndex)la,
                               kCFStringEncodingUTF8, false);
  sb = CFStringCreateWithBytes(NULL, (const UInt8 *)b, (CFIndex)lb,
                               kCFStringEncodingUTF8, false);
  if (!sa || !sb) {
    if (sa) CFRelease(sa);
    if (sb) CFRelease(sb);
    return byte_collate(la, a, lb, b);
  }
  r = CFStringCompare(sa, sb, flags);
  CFRelease(sa);
  CFRelease(sb);
  return (int)r;
}

/* ============================================================================
 * Logging (formerly enil_log.m) — C-callable shim around NSLog. One log
 * channel for both C and Objective-C; every line is formatted as
 * "[<tag>] <message>". Thread-safe because NSLog itself takes a global lock.
 * ==========================================================================*/

/* Stack buffer sized to comfortably hold a network URL + status line.
 * Anything longer is truncated with a trailing "…" so we never spill or
 * heap-alloc on a log call. */
#define ENIL_LOG_BUF 1024

void enil_log_v(const char *tag, const char *fmt, va_list ap)
{
  char buf[ENIL_LOG_BUF];
  int n;
  if (!fmt) return;
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n >= (int)sizeof(buf)) {
    /* truncated — replace tail with an ellipsis marker */
    buf[sizeof(buf) - 4] = '.';
    buf[sizeof(buf) - 3] = '.';
    buf[sizeof(buf) - 2] = '.';
    buf[sizeof(buf) - 1] = '\0';
  }
  NSLog(@"[%s] %s", tag ? tag : "?", buf);
}

void enil_log(const char *tag, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  enil_log_v(tag, fmt, ap);
  va_end(ap);
}

/* ============================================================================
 * Sync status / progress (formerly enil_progress.m) — C-callable bridge that
 * fans sync state out as ENILSyncStatusNotification on the main thread.
 * ==========================================================================*/

extern NSString * const ENILSyncStatusNotification;
extern NSString * const ENILSyncDidFinishNotification;

@interface _ENILProgressHelper : NSObject
+ (void)dispatch:(NSDictionary *)info;
+ (void)dispatchStatusOnly:(NSDictionary *)info;
@end

/* Single phase-transition log funnel. Runs only on the main thread (every
 * caller below is performSelectorOnMainThread:), so the static is implicitly
 * serialized. Logs one line per key change, on error, and on the terminal
 * step==total post — mirroring the same coalesce semantics
 * SyncMiniViewController uses to render the queue. Per-iteration posts that
 * share a key with the previous one stay silent. */
static NSString *s_last_log_key = nil;

static void log_if_key_changed(NSDictionary *info, BOOL with_finish)
{
  NSString *key = [info objectForKey:@"key"];
  NSString *msg = [info objectForKey:@"message"];
  int step  = [[info objectForKey:@"step"]      intValue];
  int total = [[info objectForKey:@"total"]     intValue];
  int uc    = [[info objectForKey:@"unitCount"] intValue];
  int ut    = [[info objectForKey:@"unitTotal"] intValue];
  int error = [[info objectForKey:@"error"]     intValue];
  BOOL terminal = with_finish && (error || step == total);
  BOOL key_changed = (key != s_last_log_key) &&
                     !(key && s_last_log_key && [key isEqualToString:s_last_log_key]);
  if (!key_changed && !terminal) return;
  NSString *short_key = key;
  if (short_key && [short_key hasPrefix:@"sync."])
    short_key = [short_key substringFromIndex:5];
  NSString *tag = short_key
    ? [NSString stringWithFormat:@"Sync.%@", short_key]
    : @"Sync.status";
  if (ut > 0 && uc >= ut)
    ENILLog(tag, @"%@ — done (%d)", msg ? msg : @"", ut);
  else if (ut > 0)
    ENILLog(tag, @"%@ — %d pending", msg ? msg : @"", ut);
  else
    ENILLog(tag, @"%@", msg ? msg : @"");
  [s_last_log_key release];
  s_last_log_key = [key retain];
}

@implementation _ENILProgressHelper

+ (void)dispatch:(NSDictionary *)info;
{
  int step  = [[info objectForKey:@"step"]  intValue];
  int total = [[info objectForKey:@"total"] intValue];
  int error = [[info objectForKey:@"error"] intValue];

  log_if_key_changed(info, YES);

  [[NSNotificationCenter defaultCenter]
    postNotificationName:ENILSyncStatusNotification
                  object:nil
                userInfo:info];

  if (step == total || error) {
    NSDictionary *finish = [NSDictionary dictionaryWithObject:
      [NSNumber numberWithBool:(step == total && !error)] forKey:@"ok"];
    [[NSNotificationCenter defaultCenter]
      postNotificationName:ENILSyncDidFinishNotification
                    object:nil
                  userInfo:finish];
    [s_last_log_key release];
    s_last_log_key = nil;
  }
}

+ (void)dispatchStatusOnly:(NSDictionary *)info;
{
  log_if_key_changed(info, NO);
  [[NSNotificationCenter defaultCenter]
    postNotificationName:ENILSyncStatusNotification
                  object:nil
                userInfo:info];
}

@end

void enil_progress_post(int step, int total, int unit_count, int unit_total,
                        const char *message, int error,
                        const char *key, int indeterminate)
{
  NSString *msgStr = [NSString stringWithUTF8String:message ? message : ""];
  NSString *keyStr = key ? [NSString stringWithUTF8String:key] : nil;
  NSMutableDictionary *info = [NSMutableDictionary dictionary];
  [info setObject:msgStr                            forKey:@"message"];
  [info setObject:[NSNumber numberWithInt:step]     forKey:@"step"];
  [info setObject:[NSNumber numberWithInt:total]    forKey:@"total"];
  [info setObject:[NSNumber numberWithInt:unit_count] forKey:@"unitCount"];
  [info setObject:[NSNumber numberWithInt:unit_total] forKey:@"unitTotal"];
  [info setObject:[NSNumber numberWithInt:error]    forKey:@"error"];
  [info setObject:[NSNumber numberWithBool:indeterminate ? YES : NO]
           forKey:@"indeterminate"];
  if (keyStr) [info setObject:keyStr forKey:@"key"];
  [_ENILProgressHelper performSelectorOnMainThread:@selector(dispatch:)
                                        withObject:info
                                     waitUntilDone:NO];
}

/* Transient out-of-band status — same userInfo shape but pinned to a
 * 1/0 step/total so the dispatcher's "is this the final post?" rule never
 * fires the Finish notification. Used by SSE callbacks etc. */
static void post_status(const char *key, const char *message,
                        int indeterminate, int error)
{
  NSString *msgStr = [NSString stringWithUTF8String:message ? message : ""];
  NSString *keyStr = key ? [NSString stringWithUTF8String:key] : nil;
  NSMutableDictionary *info = [NSMutableDictionary dictionary];
  [info setObject:msgStr                          forKey:@"message"];
  [info setObject:[NSNumber numberWithInt:1]      forKey:@"step"];
  [info setObject:[NSNumber numberWithInt:0]      forKey:@"total"];
  [info setObject:[NSNumber numberWithInt:0]      forKey:@"unitCount"];
  [info setObject:[NSNumber numberWithInt:0]      forKey:@"unitTotal"];
  [info setObject:[NSNumber numberWithInt:error]  forKey:@"error"];
  [info setObject:[NSNumber numberWithBool:indeterminate ? YES : NO]
           forKey:@"indeterminate"];
  if (keyStr) [info setObject:keyStr forKey:@"key"];
  [_ENILProgressHelper performSelectorOnMainThread:@selector(dispatchStatusOnly:)
                                        withObject:info
                                     waitUntilDone:NO];
}

void enil_status_post(const char *key, const char *message, int indeterminate)
{
  post_status(key, message, indeterminate, 0);
}

void enil_status_post_error(const char *key, const char *message)
{
  /* Always indeterminate — an error has no progress percentage to render. */
  post_status(key, message, 1, 1);
}
