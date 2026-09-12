#import "ENILQRImage.h"
#import "ENILAccount.h"

@implementation ENILQRImage

+ (NSImage *)imageForString:(NSString *)string pixelSize:(int)pixelSize;
{
  if (![string isKindOfClass:[NSString class]] || ![string length]) return nil;
  if (pixelSize < 32) pixelSize = 32;

  /* Module grid comes from the facade; rendering (quiet zone, scale, colors,
   * Y-flip) stays here — the C↔ObjC seam runs along the AppKit boundary. */
  int modules = 0;
  NSData *grid = [ENILAccount qrModulesForString:string size:&modules];
  if (!grid || modules <= 0) return nil;
  const uint8_t *m = (const uint8_t *)[grid bytes];

  int quiet   = 4;                       /* spec-mandated quiet zone */
  int total   = modules + quiet * 2;
  CGFloat scale = (CGFloat)pixelSize / (CGFloat)total;

  NSImage *image = [[[NSImage alloc] initWithSize:NSMakeSize(pixelSize, pixelSize)] autorelease];
  [image lockFocus];

  [[NSColor whiteColor] set];
  NSRectFill(NSMakeRect(0, 0, pixelSize, pixelSize));

  [[NSColor blackColor] set];
  int x, y;
  for (y = 0; y < modules; y++) {
    for (x = 0; x < modules; x++) {
      if (!m[y * modules + x]) continue;
      /* QR origin is top-left; AppKit origin is bottom-left — flip Y. */
      NSRect r = NSMakeRect((CGFloat)(x + quiet) * scale,
                            (CGFloat)(total - 1 - (y + quiet)) * scale,
                            scale, scale);
      NSRectFill(r);
    }
  }

  [image unlockFocus];
  return image;
}

@end
