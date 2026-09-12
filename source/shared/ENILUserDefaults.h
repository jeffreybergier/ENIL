#import <Foundation/Foundation.h>

@interface ENILUserDefaults : NSObject

/* Structural/reset rules shared by every rendered page (message view +
 * sticker/sticon pickers). messageCSS and pickerCSSWithItemSize: prepend
 * this, so the C renderer bakes in no CSS of its own. */
+ (NSString *)commonCSS;
+ (NSString *)messageCSS;
+ (NSString *)pickerCSSWithItemSize:(NSInteger)itemSize;
+ (NSString *)emptyMessageCSS;

@end
