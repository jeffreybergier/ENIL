//
//  StickerViewController.h
//  ENIL
//

#import <UIKit/UIKit.h>

@class ENILAccount;
@class StickerViewController;

/* Pick callback for the modal sticker grid. The host (MessageListViewController)
 * owns the engine + chatId, so the picker stays UI-only: it reports the tapped
 * item's identity and lets the host act. Two tabs switch between modes:
 * stickers (left) send one-shot via -stickerPicker:didPickPackageId:stickerId:,
 * while sticons (right) are inlined into the compose draft via
 * -stickerPicker:didPickSticonPackageId:sticonId:altText: (the iOS analog of the
 * macOS inspector's sticker/sticon tabs).
 *
 * pickerDelegate is `assign` (unsafe_unretained under ARC) per the 4.3-floor
 * rule — zeroing __weak is unavailable; the host nils it after dismissing. */
@protocol StickerPickerDelegate <NSObject>
- (void)stickerPicker:(StickerViewController *)picker
     didPickPackageId:(NSString *)packageId
            stickerId:(NSString *)stickerId;
/* The user tapped a sticon. The host dismisses and splices it into the compose
 * bar's attributed draft at the insertion point. altText may be nil. */
- (void)stickerPicker:(StickerViewController *)picker
didPickSticonPackageId:(NSString *)packageId
             sticonId:(NSString *)sticonId
              altText:(NSString *)altText;
- (void)stickerPickerDidCancel:(StickerViewController *)picker;
@end

/* Modal sticker picker: a UITabBarController whose two tabs are private
 * StickerWebViewControllers (defined in the .m) — one for stickers, one for
 * sticons — each a UIWebView loading the SAME rendered picker HTML the macOS
 * inspector uses
 * (-writeStickerPickerHTMLForSticonMode:), each wrapped in its own
 * UINavigationController for the title + Done button. Because each tab keeps
 * its own resident web view, switching tabs is instant and preserves scroll —
 * no re-render, no white flash. Presented modally by the host directly (a tab
 * bar controller is the modal root, never nested inside a nav controller).
 * Named for macOS parity (source/macOS/StickerViewController), though there it
 * is an inspector pane and here it is a presented screen.
 *
 * Note the property is `pickerDelegate`, not `delegate` — UITabBarController
 * already owns a `delegate` (id<UITabBarControllerDelegate>). */
@interface StickerViewController : UITabBarController
- (instancetype)initWithEngine:(ENILAccount *)engine;
@property (nonatomic, assign) id<StickerPickerDelegate> pickerDelegate;

/* Mark the rendered grids stale. The owner (ENILRootCoordinator) caches one
 * picker per account and keeps its web views resident, so a finished sync that
 * pulled new sticker/sticon packs must invalidate it or the grid goes stale.
 * Re-renders the already-loaded tabs immediately when on screen, else defers to
 * the next presentation; never-loaded tabs render fresh on first appearance, so
 * they need no invalidation. */
- (void)setNeedsReload;
@end
