//
//  MediaViewerViewController.h
//  ENIL
//

#import <UIKit/UIKit.h>

/* Full-screen, pinch-to-zoom viewer for a downloaded media image. Pushed onto
 * the thread's navigation stack when a chat bubble image is tapped (the
 * enil-media:// scheme). The macOS peer opens the file in Preview via
 * NSWorkspace; iOS keeps it in-app. */
@interface MediaViewerViewController : UIViewController
- (instancetype)initWithImagePath:(NSString *)imagePath;
@end
