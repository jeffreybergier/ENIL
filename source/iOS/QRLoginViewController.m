//
//  QRLoginViewController.m
//  ENIL
//

#import "QRLoginViewController.h"
#import "ENILQRImage.h"
#import "ENILAccount.h"
#import "XPFoundation.h"

static const int kQRPixels = 256;

/* NSTextAlignmentCenter is iOS 6.0+ (trips -Wunguarded-availability on the 4.3
 * floor) and its predecessor UITextAlignmentCenter is -Wdeprecated against the
 * 8.4 SDK — the same both-ways trap as the modal API, but for a constant, so
 * there is no respondsToSelector: gate. Even the NSTextAlignment *type* is
 * 6.0+, so we never name it: the value is ABI-stable at 1 (== both
 * NSTextAlignmentCenter and UITextAlignmentCenter), bound as a plain NSInteger
 * that the .textAlignment setter converts implicitly. */
static const NSInteger kENILTextAlignCenter = 1;

/* The QR-login C bridging (callback trampolines + main-thread marshalling)
 * lives behind +[ENILAccount runQRLoginAtPath:observer:cancelFlag:]; this
 * controller is the observer and receives every callback on the main thread. */
@interface QRLoginViewController () <ENILQRLoginObserver>
@property (nonatomic, copy)   NSString *accountDir;
@property (nonatomic, copy)   NSString *expectedMid;
@property (nonatomic, assign) id <QRLoginViewControllerDelegate> delegate; /* back-pointer (4.3 floor: no zeroing weak) */
@property (nonatomic, strong) UIImageView *qrImageView;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UILabel *pinLabel;
@property (nonatomic, assign) BOOL running;
@property (nonatomic, assign) BOOL done;     /* terminal handling reached */
@end

@implementation QRLoginViewController
{
  volatile int cancelFlag_;  /* read by the C flow; set when the user cancels */
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
  if ((self = [super initWithNibName:nil bundle:nil])) {
    _accountDir = [accountDir copy];
    _expectedMid = [expectedMid copy];  /* nil-safe: copy of nil is nil */
    _delegate = delegate;
    self.navigationItem.title = NSLocalizedString(@"Add Account", nil);
    self.navigationItem.leftBarButtonItem =
      [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                                                    target:self
                                                    action:@selector(cancelAction)];
  }
  return self;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor whiteColor];
  CGFloat w = self.view.bounds.size.width;

  UIImageView *qr = [[UIImageView alloc] initWithFrame:
    CGRectMake((w - kQRPixels) / 2.0f, 40.0f, kQRPixels, kQRPixels)];
  qr.autoresizingMask =
    UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin;
  qr.contentMode = UIViewContentModeScaleAspectFit;
  [self.view addSubview:qr];
  self.qrImageView = qr;

  UILabel *status = [[UILabel alloc] initWithFrame:
    CGRectMake(16.0f, 40.0f + kQRPixels + 16.0f, w - 32.0f, 22.0f)];
  status.autoresizingMask = UIViewAutoresizingFlexibleWidth;
  status.textAlignment = kENILTextAlignCenter;
  status.font = [UIFont systemFontOfSize:15.0];
  status.textColor = [UIColor darkGrayColor];
  status.text = NSLocalizedString(@"Starting…", nil);
  [self.view addSubview:status];
  self.statusLabel = status;

  UILabel *pin = [[UILabel alloc] initWithFrame:
    CGRectMake(16.0f, 40.0f + kQRPixels + 44.0f, w - 32.0f, 40.0f)];
  pin.autoresizingMask = UIViewAutoresizingFlexibleWidth;
  pin.textAlignment = kENILTextAlignCenter;
  pin.font = [UIFont boldSystemFontOfSize:30.0];
  pin.hidden = YES;
  [self.view addSubview:pin];
  self.pinLabel = pin;
}

- (void)viewDidAppear:(BOOL)animated
{
  [super viewDidAppear:animated];
  [self startLogin];
}

- (void)startLogin
{
  if (self.running) return;
  self.running = YES;
  /* The detached thread retains self for its duration, so &cancelFlag_ and the
   * observer callbacks stay valid until runQRLoginAtPath: returns — even if the
   * modal is dismissed first. */
  [NSThread detachNewThreadSelector:@selector(loginThreadMain)
                           toTarget:self
                         withObject:nil];
}

- (void)loginThreadMain
{
  @autoreleasepool {
    BOOL ok = [ENILAccount runQRLoginAtPath:self.accountDir
                                   observer:self
                                 cancelFlag:&cancelFlag_];
    [self performSelectorOnMainThread:@selector(finishWithResult:)
                           withObject:[NSNumber numberWithBool:ok]
                        waitUntilDone:NO];
  }
}

#pragma mark - ENILQRLoginObserver (main thread)

- (void)qrLoginDidEmitURL:(NSString *)url
{
  if (![url length]) return;
  self.qrImageView.image = [ENILQRImage imageForString:url pixelSize:kQRPixels];
  self.statusLabel.text =
    NSLocalizedString(@"Scan with LINE on your phone", nil);
}

- (void)qrLoginDidEmitPIN:(NSString *)pin
{
  if (![pin length]) return;
  self.pinLabel.text = pin;
  self.pinLabel.hidden = NO;
}

/* status_cb hands us the raw C status key; translate at the UI boundary so the
 * C side stays free of localization. The poll loop pre-formats "waiting for
 * scan (N/M)" in C — that would be a new key per tick, so pivot to a stable
 * format key with %d/%d placeholders the strings file owns. */
- (void)qrLoginDidEmitStatus:(NSString *)status
{
  if (![status length]) return;
  int cur = 0, total = 0;
  if (sscanf([status UTF8String], "waiting for scan (%d/%d)", &cur, &total) == 2) {
    self.statusLabel.text = [NSString stringWithFormat:
      NSLocalizedString(@"waiting for scan (%d/%d)", nil), cur, total];
    return;
  }
  self.statusLabel.text = NSLocalizedString(status, nil);
}

#pragma mark - Terminal handling

- (void)finishWithResult:(NSNumber *)result
{
  self.running = NO;
  /* The background thread retains us, so this can fire after the user already
   * cancelled — done_ (set first in -cancelAction) makes that a no-op. */
  if (self.done) return;
  self.done = YES;

  if ([result boolValue]) {
    /* Sign-in succeeded on the phone. Hand off: the delegate relocates the
     * staged session and activates the account, reshaping the nav root behind
     * the modal. On YES it owns the dismissal (-didComplete); on NO (a local
     * validate/copy/start failure) surface it inline and leave the modal up so
     * the user can Cancel and retry. */
    if ([self.delegate qrLoginViewController:self
                     didFinishWithAccountDir:self.accountDir]) {
      [self.delegate qrLoginViewControllerDidComplete:self];
      return;
    }
    self.statusLabel.text =
      NSLocalizedString(@"Couldn't finish adding the account", nil);
    [self.delegate qrLoginViewControllerDidFail:self];
    return;
  }

  self.statusLabel.text = NSLocalizedString(@"Login cancelled", nil);
  [self.delegate qrLoginViewControllerDidFail:self];
}

/* User cancelled. Flip cancelFlag_ to both abandon the flow at the next step
 * boundary AND abort the in-flight long-poll (the C flow threads it into
 * libcurl's transfer-info callback, tearing the socket down within ~1s). done_
 * claims terminal handling so the late background -finishWithResult: no-ops. */
- (void)cancelAction
{
  if (self.done) return;
  self.done = YES;
  cancelFlag_ = 1;
  [self.delegate qrLoginViewControllerDidFail:self];
}

@end
