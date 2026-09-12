//
//  ENILQRImage.h
//  ENIL
//

#import <UIKit/UIKit.h>

/* Renders a string as a QR code UIImage using the bundled nayuki qrcodegen
 * (local rendering only — clean-room safe). Drawn as filled rects with a
 * 4-module quiet zone; no Core Image. Returns nil on encode failure. The
 * UIKit peer of source/macOS/ENILQRImage (which returns an NSImage). */
@interface ENILQRImage : NSObject
+ (UIImage *)imageForString:(NSString *)string pixelSize:(int)pixelSize;
@end
