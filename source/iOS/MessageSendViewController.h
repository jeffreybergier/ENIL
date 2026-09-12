//
//  MessageSendViewController.h
//  ENIL
//

#import <UIKit/UIKit.h>

@class MessageSendViewController;

/* Submit callback for the compose bar. The host (MessageListViewController)
 * owns the engine + chatId, so the bar stays UI-only and hands the trimmed,
 * non-empty text up to the delegate to send. */
@protocol MessageSendDelegate <NSObject>
/* The user tapped Send. `text` is the trimmed LINE wire text, with each inline
 * sticon rendered as its "(altText)" marker; `sticonResources` is the ordered,
 * 1:1 list of {package_id, sticon_id, alt_text} dicts for those markers (empty
 * for a plain-text message). The host routes a non-empty list through
 * -sendInlineSticons:text:toChatId: and a plain message through
 * -sendText:toChatId:. Mirrors the macOS compose bar's send payload. */
- (void)messageSend:(MessageSendViewController *)bar
      didSubmitText:(NSString *)text
    sticonResources:(NSArray *)sticonResources;
/* The user tapped the sticker button. The host owns the engine + chatId, so it
 * presents the modal sticker picker; the bar only signals intent. */
- (void)messageSendDidTapStickers:(MessageSendViewController *)bar;
/* The user tapped the photo/image attachment button. The host presents an
 * image picker and routes the chosen file through the engine. Mirrors the
 * macOS compose bar's Photo segment; the bar only signals intent. */
- (void)messageSendDidTapImagePicker:(MessageSendViewController *)bar;
/* The compose text grew past — or shrank back to — a single line, so the bar's
 * -preferredHeight changed. The host re-lays out the bar (and the web view
 * above it) to the new height, keeping the bar's bottom edge pinned above the
 * keyboard. Mirrors macOS -messageSendViewControllerDidChangeHeight:. */
- (void)messageSendDidChangeHeight:(MessageSendViewController *)bar;
@end

/* The message-thread compose bar: a multi-line UITextView + Send button on a
 * ~44pt strip that grows taller once the text wraps past one line. Named for
 * macOS parity (source/macOS/MessageSendViewController) even though on iOS it is
 * a plain UIView, not a controller — the iOS 4.3 floor has no view-controller
 * containment (addChildViewController: is iOS 5.0). The host docks it at the
 * bottom of its view and slides it with the keyboard: the literal
 * inputAccessoryView-on-the-VC docking is iOS 8.0, so the same "bar above the
 * keyboard" UX is achieved by keyboard-frame tracking in the host.
 *
 * delegate is `assign` (unsafe_unretained under ARC) per the 4.3-floor rule —
 * zeroing __weak is unavailable; the host nils it in -dealloc. */
@interface MessageSendViewController : UIView
@property (nonatomic, assign) id<MessageSendDelegate> delegate;

/* The bar's current docked height: a collapsed single-line height at rest, and
 * a taller height once the text wraps past one line (binary, like macOS). The
 * host reads this in its keyboard-frame math and from -messageSendDidChangeHeight:
 * to size the bar and the web view above it. */
- (CGFloat)preferredHeight;

/* Compose-mode toggle, driven by the host's keyboard-frame tracking. When NO
 * (resting) the dismiss chevron + attach group are hidden, the field spans full
 * width, and the bar always uses its short collapsed layout; when YES (keyboard
 * up) the chevron + group fade in and, if the draft wraps past one line, the
 * bar grows to its tall expanded layout.
 *
 * Records state only (no layout) — call it BEFORE reading -preferredHeight so
 * the height reflects the new state, then drive the animated transition by
 * setting the bar's frame and calling -layoutIfNeeded inside the host's keyboard
 * begin/commit block, so the fade + frames ride the keyboard's curve. */
- (void)setComposing:(BOOL)composing;

/* Insert an inline sticon at the caret. The field stays plain text: this splices
 * a single placeholder marker character into the string and records the sticon
 * identity in a controller-side ordered model, realigned to the markers at send
 * time by counting the marker codepoint. No attributed text, so it works on
 * every floor (iOS 4.3+). The iOS analog of the macOS bar's
 * -insertSticonWithPackageId:sticonId:altText: (which uses an NSTextAttachment). */
- (void)insertSticonWithPackageId:(NSString *)packageId
                         sticonId:(NSString *)sticonId
                          altText:(NSString *)altText;
@end
