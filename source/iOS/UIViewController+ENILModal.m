//
//  UIViewController+ENILModal.m
//  ENIL
//

#import "UIViewController+ENILModal.h"

static BOOL ENILInvokePresentViewController(UIViewController *target,
                                            SEL selector,
                                            UIViewController *viewController,
                                            BOOL animated,
                                            BOOL hasCompletion)
{
  NSMethodSignature *signature;
  NSInvocation *invocation;
  __unsafe_unretained id viewControllerArgument;

  if ((target == nil) || (viewController == nil) ||
      ![target respondsToSelector:selector]) {
    return NO;
  }
  signature = [target methodSignatureForSelector:selector];
  if ((signature == nil) ||
      ([signature numberOfArguments] != (hasCompletion ? 5U : 4U))) {
    return NO;
  }

  invocation = [NSInvocation invocationWithMethodSignature:signature];
  [invocation setTarget:target];
  [invocation setSelector:selector];
  viewControllerArgument = viewController;
  [invocation setArgument:&viewControllerArgument atIndex:2];
  [invocation setArgument:&animated atIndex:3];
  if (hasCompletion) {
    __unsafe_unretained id completionArgument;

    completionArgument = nil;
    [invocation setArgument:&completionArgument atIndex:4];
  }
  [invocation invoke];
  return YES;
}

static BOOL ENILInvokeDismissViewController(UIViewController *target,
                                            SEL selector,
                                            BOOL animated,
                                            BOOL hasCompletion)
{
  NSMethodSignature *signature;
  NSInvocation *invocation;

  if ((target == nil) || ![target respondsToSelector:selector]) {
    return NO;
  }
  signature = [target methodSignatureForSelector:selector];
  if ((signature == nil) ||
      ([signature numberOfArguments] != (hasCompletion ? 4U : 3U))) {
    return NO;
  }

  invocation = [NSInvocation invocationWithMethodSignature:signature];
  [invocation setTarget:target];
  [invocation setSelector:selector];
  [invocation setArgument:&animated atIndex:2];
  if (hasCompletion) {
    __unsafe_unretained id completionArgument;

    completionArgument = nil;
    [invocation setArgument:&completionArgument atIndex:3];
  }
  [invocation invoke];
  return YES;
}

@implementation UIViewController (ENILModal)

- (void)enil_presentModalViewController:(UIViewController *)vc
{
  if (vc == nil) return;
  SEL modern = @selector(presentViewController:animated:completion:);
  if (ENILInvokePresentViewController(self, modern, vc, YES, YES)) {
    return;
  }
  ENILInvokePresentViewController(
    self, @selector(presentModalViewController:animated:), vc, YES, NO);
}

- (void)enil_dismissModalViewController
{
  SEL modern = @selector(dismissViewControllerAnimated:completion:);
  if (ENILInvokeDismissViewController(self, modern, YES, YES)) {
    return;
  }
  ENILInvokeDismissViewController(
    self, @selector(dismissModalViewControllerAnimated:), YES, NO);
}

@end
