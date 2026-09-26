//
//  QRLoginViewController.m
//  ENIL
//

#import "QRLoginViewController.h"
#import "ENILQRImage.h"
#import "ENILAccount.h"
#import "XPFoundation.h"
#import "XPUIKit.h"
#import <QuartzCore/QuartzCore.h>

static const int kQRPixels = 256;
static NSString * const kClientProfiles[] = { @"chrome", @"desktopwin", @"android" };
static NSString * const kClientLabels[] = {
  @"Web (Chrome)", @"Desktop (Windows)", @"Tablet (Android)"
};

/* NSTextAlignmentCenter is iOS 6.0+ (trips -Wunguarded-availability on the 4.3
 * floor) and its predecessor UITextAlignmentCenter is -Wdeprecated against the
 * 8.4 SDK — the same both-ways trap as the modal API, but for a constant, so
 * there is no respondsToSelector: gate. Even the NSTextAlignment *type* is
 * 6.0+, so we never name it: the value is ABI-stable at 1 (== both
 * NSTextAlignmentCenter and UITextAlignmentCenter), bound as a plain NSInteger
 * that the .textAlignment setter converts implicitly. */
static const NSInteger kENILTextAlignCenter = 1;

typedef enum {
  ENILLoginChoosingClient,
  ENILLoginRunning,
  ENILLoginRecovery,
  ENILLoginFinished
} ENILLoginState;

typedef enum {
  ENILLoginClientPage,
  ENILLoginQRPage,
  ENILLoginVerificationPage,
  ENILLoginRecoveryPage
} ENILLoginPage;

/* Login data belongs to the controller, not reusable cells: callbacks can
 * arrive while the QR or PIN row is offscreen. All callbacks use the main thread. */
@interface QRLoginViewController () <ENILQRLoginObserver, UINavigationControllerDelegate>
@property (nonatomic, copy) NSString *accountDir;
@property (nonatomic, copy) NSString *expectedMid;
@property (nonatomic, assign) id <QRLoginViewControllerDelegate> delegate;
@property (nonatomic, copy) NSString *selectedProfile;
@property (nonatomic, copy) NSString *statusMessage;
@property (nonatomic, copy) NSString *pin;
@property (nonatomic, strong) UIImage *qrImage;
@property (nonatomic, assign) ENILLoginState loginState;
@property (nonatomic, assign) ENILLoginPage page;
@property (nonatomic, assign) BOOL goingBack;
@property (nonatomic, strong) NSMutableDictionary *preparedPaths;
@property (nonatomic, strong) NSMutableArray *stagingPaths;
@property (nonatomic, strong) UITableViewController *loginPageController;
@property (nonatomic, assign) ENILLoginPage contentPage;
@property (nonatomic, assign) ENILLoginPage requestedPage;
@property (nonatomic, assign) BOOL navigationTransition;
@property (nonatomic, strong) UIBarButtonItem *nextButton;
@end

@implementation QRLoginViewController
{
  volatile int cancelFlag_;
}

- (instancetype)initWithAccountDir:(NSString *)accountDir
                       expectedMid:(NSString *)expectedMid
                          delegate:(id <QRLoginViewControllerDelegate>)delegate
{
  if ([accountDir length] == 0) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException
                                  reason:@"accountDir empty"
                                userInfo:nil];
  }
  if ((self = [super initWithStyle:UITableViewStyleGrouped])) {
    [self XP_layoutBelowBars];
    _accountDir = [accountDir copy];
    _expectedMid = [expectedMid copy];
    _delegate = delegate;  /* assign back-pointer: iOS 4.3 has no zeroing weak */
    _selectedProfile = @"chrome";
    _loginState = ENILLoginChoosingClient;
    _page = ENILLoginClientPage;
    _preparedPaths = [[NSMutableDictionary alloc] init];
    _stagingPaths = [[NSMutableArray alloc] initWithObjects:accountDir, nil];
    [[self navigationItem] setTitle:NSLocalizedString(@"Client", nil)];
    [[self navigationItem] setLeftBarButtonItem:[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                                                 target:self action:@selector(cancelAction)]];
  }
  return self;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  [self setNextButton:[[UIBarButtonItem alloc] initWithTitle:NSLocalizedString(@"Next", nil)
    style:UIBarButtonItemStyleDone target:self action:@selector(nextAction:)]];
  [[self navigationItem] setRightBarButtonItem:[self nextButton]];
  [self updateNavigation];
}

- (void)viewWillAppear:(BOOL)animated
{
  [super viewWillAppear:animated];
  [[self navigationController] setDelegate:self];
  [[self navigationController] setToolbarHidden:YES animated:NO];
}

- (BOOL)running { return [self loginState] == ENILLoginRunning; }
- (BOOL)done { return [self loginState] == ENILLoginFinished; }

- (UITableView *)currentTableView
{
  return [self page] == ENILLoginClientPage ? [self tableView] : [[self loginPageController] tableView];
}

- (ENILLoginPage)pageForTableView:(UITableView *)tableView
{
  return tableView == [self tableView] ? ENILLoginClientPage : [self contentPage];
}

- (void)updateNavigation
{
  [[self nextButton] setEnabled:![self running] && ![self done] && ![self navigationTransition]];
}

/* Client selection is the navigation root; the active login is a pushed page.
 * This gives UIKit ownership of the native Back button and its animation.
 * QR/PIN/recovery transitions stay within the pushed controller, so Back
 * always returns to client selection rather than to an already-consumed QR. */
- (void)showPage:(ENILLoginPage)page
{
  [self setRequestedPage:page];
  if ([self navigationTransition] || [self done]) return;
  BOOL changed = [self page] != page;
  [self setPage:page];
  if (page == ENILLoginClientPage) {
    [[self tableView] reloadData];
    if ([[self navigationController] topViewController] != self) {
      [self setNavigationTransition:YES];
      [[self navigationController] popToViewController:self animated:YES];
    }
  } else {
    if (![self loginPageController]) {
      [self setLoginPageController:[[UITableViewController alloc] initWithStyle:UITableViewStyleGrouped]];
      [[self loginPageController] XP_layoutBelowBars];
      [[[self loginPageController] tableView] setDataSource:self];
      [[[self loginPageController] tableView] setDelegate:self];
    }
    UITableView *table = [[self loginPageController] tableView];
    if (changed && [table window]) {
      CATransition *transition = [CATransition animation];
      [transition setType:kCATransitionPush];
      [transition setSubtype:kCATransitionFromRight];
      [transition setDuration:0.25];
      [transition setTimingFunction:[CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut]];
      [[table layer] addAnimation:transition forKey:@"ENILLoginPage"];
    }
    [self setContentPage:page];
    [[self loginPageController] setTitle:page == ENILLoginVerificationPage
      ? NSLocalizedString(@"PIN Code", nil)
      : (page == ENILLoginRecoveryPage ? NSLocalizedString(@"Recovery", nil)
                                       : NSLocalizedString(@"QR Code", nil))];
    [table reloadData];
    if (changed) [table setContentOffset:CGPointZero animated:NO];
    if ([[self navigationController] topViewController] != [self loginPageController]) {
      [self setNavigationTransition:YES];
      [[self navigationController] pushViewController:[self loginPageController] animated:YES];
    }
  }
  [self updateNavigation];
  if (changed) UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, nil);
}

- (void)navigationController:(UINavigationController *)navigationController
     willShowViewController:(UIViewController *)viewController animated:(BOOL)animated
{
  (void)navigationController; (void)animated;
  [self setNavigationTransition:YES];
  if (viewController != self || [self done]) return;
  [self setPage:ENILLoginClientPage];
  [self setRequestedPage:ENILLoginClientPage];
  if ([self running]) {
    /* Native Back can reveal the choices immediately, but they stay disabled
     * until the old worker has acknowledged cancellation. Cancel stays usable. */
    [self setGoingBack:YES];
    cancelFlag_ = 1;
    [self setQrImage:nil];
    [self setPin:nil];
    [self setStatusMessage:NSLocalizedString(@"Stopping login…", nil)];
  } else {
    [self setLoginState:ENILLoginChoosingClient];
    [self setStatusMessage:nil];
  }
  [[self tableView] reloadData];
  [self updateNavigation];
}

- (void)navigationController:(UINavigationController *)navigationController
      didShowViewController:(UIViewController *)viewController animated:(BOOL)animated
{
  (void)navigationController; (void)viewController; (void)animated;
  [self setNavigationTransition:NO];
  /* A PIN can arrive during the QR push. Present it once that push finishes. */
  if (![self done]) [self showPage:[self requestedPage]];
}

#pragma mark - Grouped pages

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
  (void)tableView;
  return 1;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
  (void)tableView; (void)section;
  if ([self pageForTableView:tableView] == ENILLoginClientPage)
    return [self expectedMid] ? 1 : sizeof(kClientProfiles) / sizeof(kClientProfiles[0]);
  return [self pageForTableView:tableView] == ENILLoginVerificationPage ? 3 : 2;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section
{
  (void)tableView; (void)section;
  if ([self pageForTableView:tableView] == ENILLoginClientPage) return [self statusMessage];
  if ([self pageForTableView:tableView] == ENILLoginRecoveryPage)
    return [NSString stringWithFormat:@"%@\n%@", [self statusMessage] ? [self statusMessage] : @"",
      NSLocalizedString(@"Retry resumes the saved login. Restart creates a new QR code.", nil)];
  return nil;
}

- (NSString *)textForRow:(NSInteger)row page:(ENILLoginPage)page
{
  if (page == ENILLoginClientPage)
    return NSLocalizedString(@"This account's saved client identity will be used.", nil);
  if (page == ENILLoginVerificationPage) {
    if (row == 0) return NSLocalizedString(@"Enter this code in LINE on your phone.", nil);
    if (row == 1) return [self pin];
  }
  return [self statusMessage];
}

- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath
{
  if ([self pageForTableView:tableView] == ENILLoginRecoveryPage || ([self pageForTableView:tableView] == ENILLoginClientPage && ![self expectedMid]))
    return 44.0f;
  if ([self pageForTableView:tableView] == ENILLoginQRPage && [indexPath row] == 0)
    return [self qrImage] ? kQRPixels + 24.0f : 88.0f;
  if ([self pageForTableView:tableView] == ENILLoginVerificationPage && [indexPath row] == 1) return 80.0f;
  UILabel *label = [[UILabel alloc] initWithFrame:CGRectZero];
  [label setFont:[UIFont systemFontOfSize:15.0f]];
  [label setNumberOfLines:0];
  [label setText:[self textForRow:[indexPath row] page:[self pageForTableView:tableView]]];
  CGSize size = [label sizeThatFits:CGSizeMake(MAX(1.0f, [tableView bounds].size.width - 64.0f), CGFLOAT_MAX)];
  return MAX(60.0f, size.height + 24.0f);
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
  /* Data lives on the controller, so PIN/status callbacks remain valid even
   * when their rows are offscreen on a small phone or in landscape. */
  UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                              reuseIdentifier:nil];
  if ([self pageForTableView:tableView] == ENILLoginClientPage && ![self expectedMid]) {
    NSString *profile = kClientProfiles[[indexPath row]];
    [[cell textLabel] setText:NSLocalizedString(kClientLabels[[indexPath row]], nil)];
    [cell setAccessoryType:[[self selectedProfile] isEqualToString:profile]
      ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone];
    [[cell textLabel] setEnabled:![self running] && ![self done]];
    [cell setSelectionStyle:[[cell textLabel] isEnabled]
      ? UITableViewCellSelectionStyleBlue : UITableViewCellSelectionStyleNone];
    return cell;
  }
  if ([self pageForTableView:tableView] == ENILLoginRecoveryPage) {
    [[cell textLabel] setText:[indexPath row] == 0 ? NSLocalizedString(@"Retry saved login", nil)
                                            : NSLocalizedString(@"Restart Login", nil)];
    [[cell textLabel] setTextColor:[UIColor blueColor]];
    [[cell textLabel] setEnabled:![self running] && ![self done]];
    return cell;
  }
  [cell setSelectionStyle:UITableViewCellSelectionStyleNone];
  if ([self pageForTableView:tableView] == ENILLoginQRPage && [indexPath row] == 0) {
    CGFloat height = [self tableView:tableView heightForRowAtIndexPath:indexPath];
    [cell setFrame:CGRectMake(0.0f, 0.0f, [tableView bounds].size.width, height)];
    [cell layoutIfNeeded];
    if ([self qrImage]) {
      CGFloat side = MIN((CGFloat)kQRPixels, [[cell contentView] bounds].size.width - 32.0f);
      UIImageView *qr = [[UIImageView alloc] initWithImage:[self qrImage]];
      [qr setFrame:CGRectMake(([[cell contentView] bounds].size.width - side) / 2.0f,
                            (height - side) / 2.0f, side, side)];
      [qr setAutoresizingMask:UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin];
      [qr setContentMode:UIViewContentModeScaleAspectFit];
      [qr setIsAccessibilityElement:YES];
      [qr setAccessibilityLabel:NSLocalizedString(@"Scan with LINE on your phone", nil)];
      [[cell contentView] addSubview:qr];
    } else {
      UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleGray];
      [spinner setCenter:CGPointMake([[cell contentView] bounds].size.width / 2.0f, height / 2.0f)];
      [spinner setAutoresizingMask:UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin];
      [[cell contentView] addSubview:spinner];
      [spinner startAnimating];
    }
  } else {
    BOOL isPIN = [self pageForTableView:tableView] == ENILLoginVerificationPage && [indexPath row] == 1;
    [[cell textLabel] setTextAlignment:kENILTextAlignCenter];
    [[cell textLabel] setNumberOfLines:0];
    [[cell textLabel] setFont:isPIN ? [UIFont boldSystemFontOfSize:36.0f] : [UIFont systemFontOfSize:15.0f]];
    [[cell textLabel] setTextColor:[UIColor darkGrayColor]];
    [[cell textLabel] setText:[self textForRow:[indexPath row] page:[self pageForTableView:tableView]]];
  }
  return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if ([self running] || [self done]) return;
  if ([self pageForTableView:tableView] == ENILLoginClientPage && ![self expectedMid]) {
    [self setSelectedProfile:kClientProfiles[[indexPath row]]];
    /* Keep the tapped cell alive so UIKit can finish its selection fade.
     * Reloading the table here removes the highlight along with the cells. */
    for (NSIndexPath *visiblePath in [tableView indexPathsForVisibleRows]) {
      UITableViewCell *cell = [tableView cellForRowAtIndexPath:visiblePath];
      [cell setAccessoryType:[[self selectedProfile] isEqualToString:kClientProfiles[[visiblePath row]]]
        ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone];
    }
  } else if ([self pageForTableView:tableView] == ENILLoginRecoveryPage) {
    if ([indexPath row] == 0) [self retrySavedLogin:nil];
    else [self startNewQR:nil];
  }
}

#pragma mark - Login actions

- (void)nextAction:(id)sender
{
  (void)sender;
  if ([self page] != ENILLoginClientPage) return;
  [self startLoginWithProfile:[self expectedMid] ? nil : [self selectedProfile]];
}

- (void)startLoginWithProfile:(NSString *)profile
{
  if ([self running] || [self done]) return;
  NSString *key = [self expectedMid] ? @"reauth" : profile;
  NSString *path = [[self preparedPaths] objectForKey:key];
  if (!path) {
    /* Preserve each client's attempt while navigating, just like macOS.
     * Native credential recovery remains outside the transient staging dirs. */
    path = [self accountDir];
    if ([[self preparedPaths] count]) {
      path = [[[self accountDir] stringByDeletingLastPathComponent] stringByAppendingPathComponent:
        [@".staging-" stringByAppendingString:[[NSProcessInfo processInfo] globallyUniqueString]]];
      if (![[NSFileManager defaultManager] XP_createDirectoryAtPath:path
          withIntermediateDirectories:NO attributes:nil error:NULL]) {
        [self setStatusMessage:NSLocalizedString(@"Could not save client identity", nil)];
        [[self currentTableView] reloadData];
        return;
      }
      [[self stagingPaths] addObject:path];
    }
    if (![ENILAccount prepareQRLoginAtPath:path clientProfile:profile
                      reauthenticatingMid:[self expectedMid]]) {
      [self setStatusMessage:NSLocalizedString(@"Could not save client identity", nil)];
      [[self currentTableView] reloadData];
      return;
    }
    [[self preparedPaths] setObject:path forKey:key];
  }
  [self setAccountDir:path];
  [self retrySavedLogin:nil];
}

- (void)startNewQR:(id)sender
{
  (void)sender;
  if ([self running] || [self done]) return;
  if (![ENILAccount restartQRLoginAtPath:[self accountDir]]) {
    [self setStatusMessage:NSLocalizedString(@"Could not save login recovery", nil)];
    [[self currentTableView] reloadData];
    return;
  }
  [self retrySavedLogin:nil];
}

- (void)retrySavedLogin:(id)sender
{
  (void)sender;
  if ([self running] || [self done]) return;
  cancelFlag_ = 0;
  [self setLoginState:ENILLoginRunning];
  [self setQrImage:nil];
  [self setPin:nil];
  [self setStatusMessage:NSLocalizedString(@"Starting…", nil)];
  [self showPage:ENILLoginQRPage];
  /* The thread retains self so callbacks and cancelFlag_ remain valid even
   * after dismissal. Starting a second attempt is blocked until it returns. */
  [NSThread detachNewThreadSelector:@selector(loginThreadMain) toTarget:self withObject:nil];
}

- (void)loginThreadMain
{
  @autoreleasepool {
    BOOL ok;
    ok = [ENILAccount runQRLoginAtPath:[self accountDir]
                            observer:self cancelFlag:&cancelFlag_];
    [self performSelectorOnMainThread:@selector(finishWithResult:)
                           withObject:[NSNumber numberWithBool:ok]
                        waitUntilDone:NO];
  }
}

#pragma mark - ENILQRLoginObserver (main thread)

- (void)qrLoginDidEmitURL:(NSString *)url
{
  if (![self running] || [self goingBack] || ![url length]) return;
  [self setQrImage:[ENILQRImage imageForString:url pixelSize:kQRPixels]];
  [self setStatusMessage:NSLocalizedString(@"Scan with LINE on your phone", nil)];
  [[self currentTableView] reloadData];
}

- (void)qrLoginDidEmitPIN:(NSString *)pin
{
  if (![self running] || [self goingBack] || ![pin length]) return;
  [self setPin:pin];
  [self setStatusMessage:NSLocalizedString(@"Waiting for confirmation…", nil)];
  /* Advance only when LINE actually supplies a PIN; certificate-approved
   * attempts can finish without ever visiting this page. */
  [self showPage:ENILLoginVerificationPage];
}

/* status_cb hands us the raw C status key; translate at the UI boundary so the
 * C side stays free of localization. The poll loop pre-formats "waiting for
 * scan (N/M)" in C — that would be a new key per tick, so pivot to a stable
 * format key with %d/%d placeholders the strings file owns. */
- (void)qrLoginDidEmitStatus:(NSString *)status
{
  if (![self running] || [self goingBack] || ![status length]) return;
  int cur = 0, total = 0;
  if (sscanf([status UTF8String], "waiting for scan (%d/%d)", &cur, &total) == 2) {
    [self setStatusMessage:[NSString stringWithFormat:
      NSLocalizedString(@"waiting for scan (%d/%d)", nil), cur, total]];
  } else {
    [self setStatusMessage:NSLocalizedString(status, nil)];
  }
  /* A page change can be queued behind a native push/pop animation. Reload
   * the current table's own shape instead of assuming a QR/PIN row exists. */
  [[self currentTableView] reloadData];
}

#pragma mark - Terminal handling

- (void)finishWithResult:(NSNumber *)result
{
  /* The thread retains us after dismissal. The finished state makes a late
   * completion after -cancelAction a no-op. */
  if ([self done]) return;
  if ([self goingBack]) {
    [self setGoingBack:NO];
    cancelFlag_ = 0;
    if (![result boolValue]) {
      [self setLoginState:ENILLoginChoosingClient];
      [self setStatusMessage:nil];
      [self showPage:ENILLoginClientPage];
      return;
    }
    /* A login that completed before cancellation still activates normally. */
  }
  if (![result boolValue] && [ENILAccount canRestartQRLoginAtPath:[self accountDir]]) {
    [self setLoginState:ENILLoginRecovery];
    [self setQrImage:nil];
    [self setPin:nil];
    [self showPage:ENILLoginRecoveryPage];
    return;
  }
  [self setLoginState:ENILLoginFinished];

  if ([result boolValue]) {
    /* The delegate activates the staged account and dismisses the modal on
     * success. Local activation failure follows its existing cleanup path. */
    if ([[self delegate] qrLoginViewController:self
                     didFinishWithAccountDir:[self accountDir]]) {
      [[self delegate] qrLoginViewControllerDidComplete:self];
      return;
    }
    [self setStatusMessage:NSLocalizedString(@"Couldn't finish adding the account", nil)];
    [[self delegate] qrLoginViewControllerDidFail:self];
    return;
  }

  [self setStatusMessage:NSLocalizedString(@"Login cancelled", nil)];
  [[self delegate] qrLoginViewControllerDidFail:self];
}

/* User cancelled. Flip cancelFlag_ to both abandon the flow at the next step
 * boundary AND abort the in-flight long-poll (the C flow threads it into
 * libcurl's transfer-info callback, tearing the socket down within ~1s).
 * The finished state makes the late background -finishWithResult: a no-op. */
- (void)cancelAction
{
  if ([self done]) return;
  [self setLoginState:ENILLoginFinished];
  cancelFlag_ = 1;
  [[self delegate] qrLoginViewControllerDidFail:self];
}

- (void)dealloc
{
  if ([_loginPageController isViewLoaded]) {
    [[_loginPageController tableView] setDataSource:nil];
    [[_loginPageController tableView] setDelegate:nil];
  }
  /* The coordinator cleans the active path. Saved native attempts are siblings
   * of these transient directories and must survive this cleanup. */
  for (NSString *path in _stagingPaths) {
    if (![path isEqualToString:_accountDir])
      [[NSFileManager defaultManager] XP_removeItemAtPath:path error:NULL];
  }
}

@end
