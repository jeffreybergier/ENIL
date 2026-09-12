#import <AppKit/AppKit.h>
#import "AICookieCutterWindowController.h"
#import "XPAppKit.h"

@class ENILAccount;
@class MessageSendViewController;

@protocol MessageSendDelegate <NSObject>
- (void)messageSendViewControllerDidChangeHeight:
    (MessageSendViewController *)vc;
@end

/* Subclass of AIViewController so the parent (MessageListViewController)
 * can adopt us via AI_addChildViewController: — on Middle/Modern that
 * routes through AppKit's NSViewController parent/child and the responder
 * chain reaches ChatWindowController without any manual setNextResponder:
 * wiring; on Legacy the same call wires the chain by hand. */
@interface MessageSendViewController : AIViewController <XPTextViewDelegate> {
 @private
  ENILAccount              *engine_;
  /* Cached typed alias for [self view]. The base owns the retain (set in
   * -setView:); this pointer is non-owning. Cleared in -dealloc only
   * implicitly when the base releases view_. */
  NSView                   *barView_;
  /* Recessed-bezel container for the text input. Non-owning alias —
   * subview of barView_, retained by it. Needed by -viewDidLayout to
   * recompute the bezel's frame from the bar's current bounds. */
  NSView                   *bezelView_;
  NSTextView               *inputView_;
  /* Three momentary segments: Close Chat (0), Mark as Seen (1), Send (2).
   * The Send segment carries both an icon and a "Send" label; the others
   * are icon-only with tooltips. */
  NSSegmentedControl       *actionsSegmented_;
  NSString                 *chatId_;
  NSString                 *pendingMarkSeenChatId_; /* in-memory disabled state
                                                       while awaiting SSE
                                                       confirmation */
  id<MessageSendDelegate>   delegate_;
  BOOL                      enabled_;
  BOOL                      expanded_;
}

- (id)initWithEngine:(ENILAccount *)engine;

- (CGFloat)preferredHeight;

- (void)setEnabled:(BOOL)enabled;

- (void)setChatId:(NSString *)chatId;
- (NSString *)chatId;

- (void)setDelegate:(id<MessageSendDelegate>)delegate;
- (id<MessageSendDelegate>)delegate;

- (void)insertSticonWithPackageId:(NSString *)packageId
                         sticonId:(NSString *)sticonId
                          altText:(NSString *)altText;

/* Send the current input. Shared by the Send button and the Chat menu's
   "Send Message" (Cmd+Return) item. */
- (BOOL)canSendCurrentMessage;
- (void)performSend:(id)sender;

/* Open the image-picker sheet. Public so callers outside the bottom-bar UI
 * (e.g. ChatWindowController's toolbar Photo segment) can trigger it. The
 * sheet's end fires enilImagePickerSheetDidEnd: up the responder chain. */
- (void)presentImagePicker;

@end
