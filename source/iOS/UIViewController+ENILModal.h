//
//  UIViewController+ENILModal.h
//  ENIL
//

#import <UIKit/UIKit.h>

/* 4.3-floor modal present/dismiss. -presentViewController:animated:completion:
 * is iOS 5.0+; the 4.3 fallback -presentModalViewController:animated: is
 * -Wdeprecated against the 8.4 SDK. Both selectors go through NSInvocation:
 * respondsToSelector: is the runtime gate, and signature-checked invocation
 * keeps the BOOL and block arguments ABI-correct without function casts. */
@interface UIViewController (ENILModal)
- (void)enil_presentModalViewController:(UIViewController *)vc;
- (void)enil_dismissModalViewController;
@end
