//
//  QRLoginViewController.m
//  ENIL
//

#import "QRLoginViewController.h"
#import "ENILQRImage.h"
#import "ENILAccount.h"
#import "XPFoundation.h"
#import "XPUIKit.h"

static const int kQRPixels = 256;

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

/* Login data belongs to the controller, not reusable cells: callbacks can
 * arrive while the QR or PIN row is offscreen. All callbacks use the main thread. */
@interface QRLoginViewController () <ENILQRLoginObserver>
@property (nonatomic, copy) NSString *accountDir;
@property (nonatomic, copy) NSString *expectedMid;
@property (nonatomic, assign) id <QRLoginViewControllerDelegate> delegate;
@property (nonatomic, copy) NSString *selectedProfile;
@property (nonatomic, copy) NSString *statusMessage;
@property (nonatomic, copy) NSString *pin;
@property (nonatomic, strong) UIImage *qrImage;
@property (nonatomic, assign) ENILLoginState loginState;
@property (nonatomic, assign) BOOL prepared;
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
    self.navigationItem.title = expectedMid
      ? NSLocalizedString(@"Reauthenticate", nil) : NSLocalizedString(@"Add Account", nil);
    self.navigationItem.leftBarButtonItem =
      [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                                                 target:self action:@selector(cancelAction)];
  }
  return self;
}

- (void)viewDidAppear:(BOOL)animated
{
  [super viewDidAppear:animated];
  /* Reauthentication retains the saved identity and existing automatic start. */
  if (self.expectedMid && !self.prepared) [self startLoginWithProfile:nil];
}

- (NSInteger)loginSection { return self.expectedMid ? 0 : 1; }
- (BOOL)running { return self.loginState == ENILLoginRunning; }
- (BOOL)done { return self.loginState == ENILLoginFinished; }

#pragma mark - Grouped table

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
  (void)tableView;
  return self.loginSection + 1;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
  (void)tableView;
  if (section != self.loginSection) return 2;
  if (self.running) return [self.pin length] ? 3 : 2;
  return self.loginState == ENILLoginRecovery ? 2 : 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
  (void)tableView;
  if (section != self.loginSection) return NSLocalizedString(@"Client", nil);
  return self.running ? NSLocalizedString(@"QR Code", nil) : nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section
{
  (void)tableView;
  if (section != self.loginSection) return nil;
  if (self.running) return nil;
  if (self.loginState == ENILLoginRecovery) {
    return [NSString stringWithFormat:@"%@\n%@", self.statusMessage,
      NSLocalizedString(@"Retry resumes the saved login. Restart creates a new QR code.", nil)];
  }
  return self.statusMessage;
}

- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath
{
  if (indexPath.section != self.loginSection || !self.running) return 44.0f;
  if (indexPath.row == 0) return self.qrImage ? kQRPixels + 24.0f : 88.0f;
  if (indexPath.row == 2) return 60.0f;
  UILabel *label = [[UILabel alloc] initWithFrame:CGRectZero];
  label.font = [UIFont systemFontOfSize:15.0f];
  label.numberOfLines = 0;
  label.text = self.statusMessage;
  CGSize size = [label sizeThatFits:CGSizeMake(MAX(1.0f, tableView.bounds.size.width - 64.0f), CGFLOAT_MAX)];
  return MAX(60.0f, size.height + 24.0f);
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
  /* This small form uses separate cells for each role, so changing state never
   * carries a checkmark, spinner, or old PIN into an unrelated row. */
  UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                              reuseIdentifier:nil];
  if (indexPath.section != self.loginSection) {
    NSString *profile = indexPath.row == 0 ? @"desktopwin" : @"chrome";
    cell.textLabel.text = indexPath.row == 0
      ? NSLocalizedString(@"Windows", nil) : NSLocalizedString(@"Chrome", nil);
    cell.accessoryType = [self.selectedProfile isEqualToString:profile]
      ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.textLabel.enabled = !self.prepared && !self.done;
    cell.selectionStyle = cell.textLabel.enabled
      ? UITableViewCellSelectionStyleBlue : UITableViewCellSelectionStyleNone;
    return cell;
  }
  if (!self.running) {
    cell.textLabel.text = self.loginState == ENILLoginRecovery
      ? (indexPath.row == 0 ? NSLocalizedString(@"Retry saved login", nil)
                            : NSLocalizedString(@"Restart Login", nil))
      : NSLocalizedString(@"Start Login", nil);
    cell.textLabel.textColor = [UIColor blueColor];
    cell.textLabel.enabled = !self.done;
    return cell;
  }
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  if (indexPath.row == 0) {
    CGFloat height = [self tableView:tableView heightForRowAtIndexPath:indexPath];
    cell.frame = CGRectMake(0.0f, 0.0f, tableView.bounds.size.width, height);
    [cell layoutIfNeeded];
    if (self.qrImage) {
      CGFloat side = MIN((CGFloat)kQRPixels, cell.contentView.bounds.size.width - 32.0f);
      UIImageView *qr = [[UIImageView alloc] initWithImage:self.qrImage];
      qr.frame = CGRectMake((cell.contentView.bounds.size.width - side) / 2.0f,
                            (height - side) / 2.0f, side, side);
      qr.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
      qr.contentMode = UIViewContentModeScaleAspectFit;
      qr.isAccessibilityElement = YES;
      qr.accessibilityLabel = NSLocalizedString(@"Scan with LINE on your phone", nil);
      [cell.contentView addSubview:qr];
    } else {
      UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleGray];
      spinner.center = CGPointMake(cell.contentView.bounds.size.width / 2.0f, height / 2.0f);
      spinner.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
      [cell.contentView addSubview:spinner];
      [spinner startAnimating];
    }
  } else {
    cell.textLabel.textAlignment = kENILTextAlignCenter;
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.font = indexPath.row == 2
      ? [UIFont boldSystemFontOfSize:30.0f] : [UIFont systemFontOfSize:15.0f];
    cell.textLabel.textColor = [UIColor darkGrayColor];
    cell.textLabel.text = indexPath.row == 2 ? self.pin : self.statusMessage;
  }
  return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (self.running || self.done) return;
  if (indexPath.section != self.loginSection) {
    if (self.prepared) return;
    self.selectedProfile = indexPath.row == 0 ? @"desktopwin" : @"chrome";
    [tableView reloadSections:[NSIndexSet indexSetWithIndex:0] withRowAnimation:UITableViewRowAnimationNone];
  } else if (self.loginState == ENILLoginRecovery) {
    if (indexPath.row == 0) [self retrySavedLogin:nil];
    else [self startNewQR:nil];
  } else {
    [self startLoginWithProfile:self.expectedMid ? nil : self.selectedProfile];
  }
}

- (void)reloadLoginSection
{
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:(NSUInteger)self.loginSection]
               withRowAnimation:UITableViewRowAnimationNone];
  /* Resolve the new row heights before scrolling. Animated section updates
   * still expose the old content size on older UIKit, clipping the new PIN. */
  [self.tableView layoutIfNeeded];
}

- (void)showLoginRow:(NSInteger)row
{
  [self.tableView scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:row inSection:self.loginSection]
                      atScrollPosition:row == 0 ? UITableViewScrollPositionTop : UITableViewScrollPositionBottom
                              animated:NO];
}

#pragma mark - Login actions

- (void)startLoginWithProfile:(NSString *)profile
{
  if (self.running || self.done) return;
  if (![ENILAccount prepareQRLoginAtPath:self.accountDir clientProfile:profile
                    reauthenticatingMid:self.expectedMid]) {
    self.statusMessage = NSLocalizedString(@"Could not save client identity", nil);
    [self reloadLoginSection];
    return;
  }
  self.prepared = YES;
  [self retrySavedLogin:nil];
}

- (void)startNewQR:(id)sender
{
  (void)sender;
  if (self.running || self.done) return;
  if (![ENILAccount restartQRLoginAtPath:self.accountDir]) {
    self.statusMessage = NSLocalizedString(@"Could not save login recovery", nil);
    [self reloadLoginSection];
    return;
  }
  [self retrySavedLogin:nil];
}

- (void)retrySavedLogin:(id)sender
{
  (void)sender;
  if (self.running || self.done) return;
  self.loginState = ENILLoginRunning;
  self.qrImage = nil;
  self.pin = nil;
  self.statusMessage = NSLocalizedString(@"Starting…", nil);
  [self.tableView reloadData];
  /* The thread retains self so callbacks and cancelFlag_ remain valid even
   * after dismissal. Starting a second attempt is blocked until it returns. */
  [NSThread detachNewThreadSelector:@selector(loginThreadMain) toTarget:self withObject:nil];
}

- (void)loginThreadMain
{
  @autoreleasepool {
    BOOL ok;
    ok = [ENILAccount runQRLoginAtPath:self.accountDir
                            observer:self cancelFlag:&cancelFlag_];
    [self performSelectorOnMainThread:@selector(finishWithResult:)
                           withObject:[NSNumber numberWithBool:ok]
                        waitUntilDone:NO];
  }
}

#pragma mark - ENILQRLoginObserver (main thread)

- (void)qrLoginDidEmitURL:(NSString *)url
{
  if (!self.running || ![url length]) return;
  self.qrImage = [ENILQRImage imageForString:url pixelSize:kQRPixels];
  self.statusMessage = NSLocalizedString(@"Scan with LINE on your phone", nil);
  [self reloadLoginSection];
  [self showLoginRow:0];
}

- (void)qrLoginDidEmitPIN:(NSString *)pin
{
  if (!self.running || ![pin length]) return;
  self.pin = pin;
  [self reloadLoginSection];
  [self showLoginRow:2];
}

/* status_cb hands us the raw C status key; translate at the UI boundary so the
 * C side stays free of localization. The poll loop pre-formats "waiting for
 * scan (N/M)" in C — that would be a new key per tick, so pivot to a stable
 * format key with %d/%d placeholders the strings file owns. */
- (void)qrLoginDidEmitStatus:(NSString *)status
{
  if (!self.running || ![status length]) return;
  int cur = 0, total = 0;
  if (sscanf([status UTF8String], "waiting for scan (%d/%d)", &cur, &total) == 2) {
    self.statusMessage = [NSString stringWithFormat:
      NSLocalizedString(@"waiting for scan (%d/%d)", nil), cur, total];
  } else {
    self.statusMessage = NSLocalizedString(status, nil);
  }
  [self.tableView reloadRowsAtIndexPaths:[NSArray arrayWithObject:
    [NSIndexPath indexPathForRow:1 inSection:self.loginSection]]
                        withRowAnimation:UITableViewRowAnimationNone];
}

#pragma mark - Terminal handling

- (void)finishWithResult:(NSNumber *)result
{
  /* The thread retains us after dismissal. The finished state makes a late
   * completion after -cancelAction a no-op. */
  if (self.done) return;
  if (![result boolValue] && [ENILAccount canRestartQRLoginAtPath:self.accountDir]) {
    NSString *failure = self.statusMessage;
    self.loginState = ENILLoginRecovery;
    self.qrImage = nil;
    self.pin = nil;
    self.statusMessage = NSLocalizedString(@"Sign-in needs recovery", nil);
    [self reloadLoginSection];
    [self showLoginRow:0];
    [self XP_showAlertWithTitle:NSLocalizedString(@"Sign-in needs recovery", nil)
                       message:failure
                  dismissTitle:NSLocalizedString(@"Dismiss", nil)];
    return;
  }
  self.loginState = ENILLoginFinished;

  if ([result boolValue]) {
    /* The delegate activates the staged account and dismisses the modal on
     * success. Local activation failure follows its existing cleanup path. */
    if ([self.delegate qrLoginViewController:self
                     didFinishWithAccountDir:self.accountDir]) {
      [self.delegate qrLoginViewControllerDidComplete:self];
      return;
    }
    self.statusMessage =
      NSLocalizedString(@"Couldn't finish adding the account", nil);
    [self.delegate qrLoginViewControllerDidFail:self];
    return;
  }

  self.statusMessage = NSLocalizedString(@"Login cancelled", nil);
  [self.delegate qrLoginViewControllerDidFail:self];
}

/* User cancelled. Flip cancelFlag_ to both abandon the flow at the next step
 * boundary AND abort the in-flight long-poll (the C flow threads it into
 * libcurl's transfer-info callback, tearing the socket down within ~1s).
 * The finished state makes the late background -finishWithResult: a no-op. */
- (void)cancelAction
{
  if (self.done) return;
  self.loginState = ENILLoginFinished;
  cancelFlag_ = 1;
  [self.delegate qrLoginViewControllerDidFail:self];
}

@end
