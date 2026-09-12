//
//  ENILQRImage.m
//  ENIL
//

#import "ENILQRImage.h"
#import "ENILAccount.h"

@implementation ENILQRImage

+ (UIImage *)imageForString:(NSString *)string pixelSize:(int)pixelSize
{
  if (![string isKindOfClass:[NSString class]] || ![string length]) return nil;
  if (pixelSize < 32) pixelSize = 32;

  /* Module grid comes from the facade; rendering (quiet zone, scale, colors)
   * stays here — the C->ObjC seam runs along the UIKit boundary. UIKit's
   * origin is top-left, the same as QR's, so there is no Y-flip (the macOS
   * AppKit peer flips because its origin is bottom-left). */
  int modules = 0;
  NSData *grid = [ENILAccount qrModulesForString:string size:&modules];
  if (!grid || modules <= 0) return nil;
  const uint8_t *m = (const uint8_t *)[grid bytes];

  int quiet = 4;                         /* spec-mandated quiet zone */
  int total = modules + quiet * 2;
  CGFloat scale = (CGFloat)pixelSize / (CGFloat)total;

  CGFloat side = (CGFloat)pixelSize;
  UIGraphicsBeginImageContextWithOptions(CGSizeMake(side, side), YES, 0.0);
  CGContextRef ctx = UIGraphicsGetCurrentContext();

  CGContextSetGrayFillColor(ctx, 1.0, 1.0);
  CGContextFillRect(ctx, CGRectMake(0, 0, side, side));

  CGContextSetGrayFillColor(ctx, 0.0, 1.0);
  int x, y;
  for (y = 0; y < modules; y++) {
    for (x = 0; x < modules; x++) {
      if (!m[y * modules + x]) continue;
      CGContextFillRect(ctx, CGRectMake((CGFloat)(x + quiet) * scale,
                                        (CGFloat)(y + quiet) * scale,
                                        scale, scale));
    }
  }

  UIImage *image = UIGraphicsGetImageFromCurrentImageContext();
  UIGraphicsEndImageContext();
  return image;
}

@end
