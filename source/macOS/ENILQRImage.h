#import <AppKit/AppKit.h>

/* Renders a string as a QR code NSImage using the bundled nayuki qrcodegen
 * (local rendering only — clean-room safe). Drawn as filled rects with a
 * 4-module quiet zone; no Core Image. Returns nil on encode failure. */
@interface ENILQRImage : NSObject
+ (NSImage *)imageForString:(NSString *)string pixelSize:(int)pixelSize;
@end
