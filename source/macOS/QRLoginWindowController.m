#import "QRLoginWindowController.h"
#import "ENILQRImage.h"
#import "ENILAccount.h"

static const int kQRPixels = 256;
/* Reserve the full QR + multiline status + PIN layout from the first frame.
 * Neither the window nor either box changes size as login progresses. */
static const CGFloat kLoginWindowWidth = 460;
static const CGFloat kLoginWindowHeight = 600;

/* The QR-login C bridging (callback trampolines + main-thread marshalling)
 * now lives behind +[ENILAccount runQRLoginAtPath:observer:cancelFlag:]; this
 * controller is the observer and receives all callbacks on the main thread. */
@interface QRLoginWindowController () <ENILQRLoginObserver>
- (void)beginLoginWithProfile:(NSString *)profile;
- (void)chooseClient:(id)sender;
- (void)startSelectedLogin:(id)sender;
- (void)showLoginActionsForRecovery:(BOOL)recovery;
- (void)retrySavedLogin:(id)sender;
- (void)startNewQR:(id)sender;
@end

@implementation QRLoginWindowController

- (id)initWithAccountDir:(NSString *)accountDir
             expectedMid:(NSString *)expectedMid
                delegate:(id <QRLoginWindowControllerDelegate>)delegate;
{
  if ((self = [super initWithWindowNibName:@"ignored"])) {
    accountDir_  = [accountDir copy];
    expectedMid_ = [expectedMid copy]; /* nil-safe: copy of nil is nil */
    delegate_    = delegate; /* weak */
    running_     = NO;
  }
  return self;
}

- (NSString *)expectedMid { return expectedMid_; }

- (void)loadWindow;
{
  XPWindowStyleMask mask = XPWindowStyleMaskTitled
                         | XPWindowStyleMaskClosable;
  NSWindow *window = [[[NSWindow alloc]
    initWithContentRect:NSMakeRect(0, 0, kLoginWindowWidth, kLoginWindowHeight)
              styleMask:mask
                backing:NSBackingStoreBuffered
                  defer:NO] autorelease];
  [window setTitle:expectedMid_ ? NSLocalizedString(@"Reauthenticate", nil)
                               : NSLocalizedString(@"Add Account", nil)];
  [window setReleasedWhenClosed:NO];
  [window setDelegate:(id)self]; /* for windowWillClose: (user-cancel);
    (id) cast — NSWindowDelegate is a 10.6+ formal protocol, absent on the
    10.5 SDK we build against */
  [window center];
  [self setWindow:window];
}

- (NSTextField *)labelInRect:(NSRect)r;
{
  NSTextField *f = [[[NSTextField alloc] initWithFrame:r] autorelease];
  [f setEditable:NO];
  [f setSelectable:NO];
  [f setBordered:NO];
  [f setBezeled:NO];
  [f setDrawsBackground:NO];
  [f setAlignment:XPTextAlignmentCenter];
  [f setFont:[NSFont systemFontOfSize:13]];
  [[f cell] setWraps:YES];
  [[f cell] setScrollable:NO];
  return f;
}

- (NSButton *)actionButtonInRect:(NSRect)frame title:(NSString *)title action:(SEL)action;
{
  NSButton *button = [[[NSButton alloc] initWithFrame:frame] autorelease];
  [button setTitle:title];
  [button setBezelStyle:XPBezelStyleRounded];
  [button setTarget:self];
  [button setAction:action];
  [button setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin];
  return button;
}

- (void)windowDidLoad;
{
  [super windowDidLoad];
  NSView *cv = [[self window] contentView];
  NSBox *clientBox = [[[NSBox alloc]
    initWithFrame:NSMakeRect(20, 484, kLoginWindowWidth - 40, 96)] autorelease];
  [clientBox setTitle:NSLocalizedString(@"Client", nil)];
  [clientBox setContentViewMargins:NSMakeSize(12, 8)];
  [cv addSubview:clientBox];
  NSView *clients = [clientBox contentView];
  CGFloat clientWidth = [clients bounds].size.width;
  CGFloat clientHeight = [clients bounds].size.height;

  /* Center the pair as one unit when NSBox settles its content bounds on
   * Tiger. Offset slightly to center the radio artwork, which sits above
   * the center of its control frames. */
  NSView *choices = [[[NSView alloc] initWithFrame:
    NSMakeRect(0, (clientHeight - 48) / 2 - 3, clientWidth, 48)] autorelease];
  [choices setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin];
  [clients addSubview:choices];
  chromeButton_ = [[NSButton alloc] initWithFrame:NSMakeRect(8, 26, clientWidth - 16, 22)];
  [chromeButton_ setTitle:NSLocalizedString(@"Chrome", nil)];
  [chromeButton_ setButtonType:XPButtonTypeRadio];
  [chromeButton_ setTarget:self];
  [chromeButton_ setAction:@selector(chooseClient:)];
  [chromeButton_ setState:XPControlStateOn];
  [chromeButton_ setAutoresizingMask:NSViewWidthSizable];
  [choices addSubview:chromeButton_];
  windowsButton_ = [[NSButton alloc] initWithFrame:NSMakeRect(8, 0, clientWidth - 16, 22)];
  [windowsButton_ setTitle:NSLocalizedString(@"Windows", nil)];
  [windowsButton_ setButtonType:XPButtonTypeRadio];
  [windowsButton_ setTarget:self];
  [windowsButton_ setAction:@selector(chooseClient:)];
  [windowsButton_ setState:XPControlStateOff];
  [windowsButton_ setAutoresizingMask:NSViewWidthSizable];
  [choices addSubview:windowsButton_];
  [chromeButton_ setHidden:expectedMid_ != nil];
  [windowsButton_ setHidden:expectedMid_ != nil];
  if (expectedMid_) {
    NSTextField *savedClient = [self labelInRect:NSMakeRect(8, 4, clientWidth - 16, clientHeight - 8)];
    [savedClient setStringValue:NSLocalizedString(@"This account's saved client identity will be used.", nil)];
    [savedClient setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [clients addSubview:savedClient];
  }

  loginBox_ = [[NSBox alloc] initWithFrame:NSMakeRect(20, 20, kLoginWindowWidth - 40, 448)];
  [loginBox_ setContentViewMargins:NSMakeSize(12, 12)];
  [cv addSubview:loginBox_];
  NSView *login = [loginBox_ contentView];
  CGFloat width = [login bounds].size.width;
  CGFloat height = [login bounds].size.height;

  qrImageView_ = [[NSImageView alloc]
    initWithFrame:NSMakeRect((width - kQRPixels) / 2, height - 12 - kQRPixels, kQRPixels, kQRPixels)];
  [qrImageView_ setImageScaling:XPImageScaleAxesIndependently];
  /* Tiger tiles NSBox's content view on first display. Anchor children to its
   * final bounds even though the outer window and boxes never resize. */
  [qrImageView_ setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin];
  [login addSubview:qrImageView_];

  statusField_ = [[self labelInRect:NSMakeRect(8, 66, width - 16, 64)] retain];
  [statusField_ setStringValue:@""];
  [statusField_ setAutoresizingMask:NSViewWidthSizable];
  [login addSubview:statusField_];
  pinField_ = [[self labelInRect:NSMakeRect(8, 8, width - 16, 40)] retain];
  [pinField_ setFont:[NSFont boldSystemFontOfSize:28]];
  [pinField_ setSelectable:YES];
  [pinField_ setStringValue:@""];
  [pinField_ setAutoresizingMask:NSViewWidthSizable];
  [login addSubview:pinField_];

  CGFloat buttonWidth = 260;
  CGFloat buttonX = (width - buttonWidth) / 2;
  startButton_ = [[self actionButtonInRect:NSMakeRect(buttonX, height - 120, buttonWidth, 32)
    title:NSLocalizedString(@"Start Login", nil) action:@selector(startSelectedLogin:)] retain];
  retryButton_ = [[self actionButtonInRect:NSMakeRect(buttonX, height - 120, buttonWidth, 32)
    title:NSLocalizedString(@"Retry saved login", nil) action:@selector(retrySavedLogin:)] retain];
  restartButton_ = [[self actionButtonInRect:NSMakeRect(buttonX, height - 164, buttonWidth, 32)
    title:NSLocalizedString(@"Restart Login", nil) action:@selector(startNewQR:)] retain];
  [login addSubview:startButton_];
  [login addSubview:retryButton_];
  [login addSubview:restartButton_];
  recoveryHelpField_ = [[self labelInRect:NSMakeRect(8, 146, width - 16, 72)] retain];
  [recoveryHelpField_ setStringValue:NSLocalizedString(
    @"Retry resumes the saved login. Restart creates a new QR code.", nil)];
  [recoveryHelpField_ setAutoresizingMask:NSViewWidthSizable];
  [login addSubview:recoveryHelpField_];
  [self showLoginActionsForRecovery:NO];
}

- (void)showLoginActionsForRecovery:(BOOL)recovery;
{
  [loginBox_ setTitle:NSLocalizedString(@"Login", nil)];
  [qrImageView_ setHidden:YES];
  [pinField_ setHidden:YES];
  [startButton_ setHidden:recovery];
  [retryButton_ setHidden:!recovery];
  [restartButton_ setHidden:!recovery];
  [recoveryHelpField_ setHidden:!recovery];
  [[self window] setDefaultButtonCell:[(recovery ? retryButton_ : startButton_) cell]];
}

- (void)start;
{
  if (running_ || done_) return;
  [self showWindow:self];
  if (expectedMid_ && !prepared_) [self beginLoginWithProfile:nil];
}

- (void)chooseClient:(id)sender;
{
  if (running_ || done_ || prepared_ || expectedMid_) return;
  [windowsButton_ setState:sender == windowsButton_ ? XPControlStateOn : XPControlStateOff];
  [chromeButton_ setState:sender == chromeButton_ ? XPControlStateOn : XPControlStateOff];
}

- (void)startSelectedLogin:(id)sender;
{
  (void)sender;
  [self beginLoginWithProfile:expectedMid_ ? nil :
    ([windowsButton_ state] == XPControlStateOn ? @"desktopwin" : @"chrome")];
}

- (void)beginLoginWithProfile:(NSString *)profile;
{
  if (running_ || done_) return;
  if (![ENILAccount prepareQRLoginAtPath:accountDir_
                          clientProfile:profile
                    reauthenticatingMid:expectedMid_]) {
    [statusField_ setStringValue:NSLocalizedString(@"Could not save client identity", nil)];
    return;
  }
  prepared_ = YES;
  [chromeButton_ setEnabled:NO];
  [windowsButton_ setEnabled:NO];
  [self retrySavedLogin:nil];
}

- (void)startNewQR:(id)sender;
{
  (void)sender;
  if (running_ || done_) return;
  if (![ENILAccount restartQRLoginAtPath:accountDir_]) {
    [statusField_ setStringValue:NSLocalizedString(@"Could not save login recovery", nil)];
    return;
  }
  [self retrySavedLogin:nil];
}

- (void)retrySavedLogin:(id)sender;
{
  (void)sender;
  if (running_ || done_) return;
  running_ = YES;
  [qrImageView_ setImage:nil];
  [pinField_ setHidden:YES];
  [pinField_ setStringValue:@""];
  [startButton_ setHidden:YES];
  [retryButton_ setHidden:YES];
  [restartButton_ setHidden:YES];
  [recoveryHelpField_ setHidden:YES];
  [loginBox_ setTitle:NSLocalizedString(@"QR Code", nil)];
  [[self window] setDefaultButtonCell:nil];
  [qrImageView_ setHidden:NO];
  [statusField_ setStringValue:NSLocalizedString(@"Starting…", nil)];
  [NSThread detachNewThreadSelector:@selector(loginThreadMain)
                           toTarget:self
                         withObject:nil];
}

- (void)loginThreadMain;
{
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  /* self is retained by this thread for the duration, so &cancelFlag_ and the
   * observer callbacks stay valid until runQRLoginAtPath: returns. */
  BOOL ok = [ENILAccount runQRLoginAtPath:accountDir_
                                 observer:self
                               cancelFlag:&cancelFlag_];

  [self performSelectorOnMainThread:@selector(finishWithResult:)
                         withObject:[NSNumber numberWithBool:ok]
                      waitUntilDone:NO];
  [pool release];
}

/* ---- ENILQRLoginObserver — all delivered on the main thread ------------- */

- (void)qrLoginDidEmitURL:(NSString *)s;
{
  if (!running_ || done_ || ![s length]) return;
  [qrImageView_ setImage:[ENILQRImage imageForString:s pixelSize:kQRPixels]];
  [statusField_ setStringValue:NSLocalizedString(@"Scan with LINE on your phone", nil)];
}

- (void)qrLoginDidEmitPIN:(NSString *)s;
{
  if (!running_ || done_ || ![s length]) return;
  [pinField_ setStringValue:s];
  [pinField_ setHidden:NO];
}

- (void)qrLoginDidEmitStatus:(NSString *)s;
{
  /* status_cb hands us the raw C status key (e.g. "creating QR session").
   * Translate at the UI boundary — keeps the C side free of any Cocoa
   * localization concerns. Unknown keys fall back to the key string itself.
   *
   * Special case: the poll loop pre-formats "waiting for scan (N/M)" in C.
   * That would produce a new key per tick, so detect it here and pivot to
   * a stable format key with %d/%d placeholders that the strings file owns. */
  if (!running_ || done_ || ![s length]) return;
  int cur = 0, total = 0;
  if (sscanf([s UTF8String], "waiting for scan (%d/%d)", &cur, &total) == 2) {
    [statusField_ setStringValue:
      [NSString stringWithFormat:
        NSLocalizedString(@"waiting for scan (%d/%d)", nil), cur, total]];
    return;
  }
  [statusField_ setStringValue:NSLocalizedString(s, nil)];
}

- (NSString *)accountDir { return accountDir_; }

- (void)finishWithResult:(NSNumber *)result;
{
  running_ = NO;
  /* The background thread retains us, so this still fires after the user
     closed the window — but windowWillClose: already tore everything down.
     Bail before touching the (closed) window, or we'd resurrect it with a
     sheet. Otherwise claim terminal handling now so the programmatic
     [window close] below doesn't re-enter the windowWillClose: cancel path. */
  if (done_) return;
  if (![result boolValue] && [ENILAccount canRestartQRLoginAtPath:accountDir_]) {
    [qrImageView_ setImage:nil];
    [pinField_ setStringValue:@""];
    [statusField_ setStringValue:NSLocalizedString(@"Sign-in needs recovery", nil)];
    [self showLoginActionsForRecovery:YES];
    return;
  }
  done_ = YES;
  if ([result intValue] != 0) {
    /* Sign-in succeeded on the phone. Hand off: the delegate relocates the
       session and opens the chat window *behind* us. On success we just
       close (silent handoff — the move is sub-second). On a local failure
       (validate/copy/engine-start) surface it, since the success path shows
       no UI of its own. */
    if ([delegate_ qrLoginWindowController:self didFinishWithAccountDir:accountDir_]) {
      [[self window] close];
      [delegate_ qrLoginWindowControllerDidComplete:self];
      return;
    }
    [statusField_ setStringValue:NSLocalizedString(@"Couldn't finish adding the account", nil)];
    {
      NSAlert *alert = [[[NSAlert alloc] init] autorelease];
      [alert setMessageText:NSLocalizedString(@"Couldn't Add Account", nil)];
      [alert setInformativeText:NSLocalizedString(
        @"Sign-in worked, but the account couldn't be saved on this Mac. "
         "Please try Add Account again.", nil)];
      [alert addButtonWithTitle:NSLocalizedString(@"Dismiss", nil)];
      [alert XP_beginSheetModalForWindow:[self window]
                           modalDelegate:self
                          didEndSelector:
        @selector(failAlertDidEnd:returnCode:contextInfo:)
                             contextInfo:NULL];
    }
    return;
  }

  [statusField_ setStringValue:NSLocalizedString(@"Login cancelled", nil)];
  {
    NSAlert *alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:NSLocalizedString(@"Login Cancelled", nil)];
    [alert setInformativeText:NSLocalizedString(
      @"The QR sign-in was cancelled or timed out. No account was added.", nil)];
    [alert addButtonWithTitle:NSLocalizedString(@"Dismiss", nil)];
    [alert XP_beginSheetModalForWindow:[self window]
                         modalDelegate:self
                        didEndSelector:
      @selector(failAlertDidEnd:returnCode:contextInfo:)
                           contextInfo:NULL];
  }
}

/* Dismiss closes the sheet AND the login window, then hands control back to
   the delegate (which cleans up the staging dir + releases us). Nothing
   touches `self` after the delegate call — safe with the delegate's
   autorelease. */
- (void)failAlertDidEnd:(NSAlert *)alert
             returnCode:(NSInteger)returnCode
            contextInfo:(void *)contextInfo;
{
  (void)returnCode;
  (void)contextInfo;
  [[alert window] orderOut:nil];
  [[self window] close];
  [delegate_ qrLoginWindowControllerDidFail:self];
}

/* User closed the login window = cancel. Distinguish from the programmatic
   [window close] in the normal success/fail/cancel paths via done_ (set
   first there). Flipping cancelFlag_ both abandons the flow at the next
   step boundary AND aborts the in-flight long-poll connection: the C flow
   threads cancelFlag_ into libcurl's transfer-info callback, so the open
   checkQrCodeVerified / checkPinCodeVerified request tears down within ~1s
   instead of holding the socket until the server answers. We then hand to
   the delegate's fail cleanup (discards staging, drops its ref via
   autorelease — safe: the background thread still retains us until
   runQRLoginAtPath: returns, at which point finishWithResult: no-ops on
   done_). */
- (void)windowWillClose:(NSNotification *)note;
{
  (void)note;
  if (done_) return;
  done_ = YES;
  cancelFlag_ = 1;
  [delegate_ qrLoginWindowControllerDidFail:self];
}

- (void)dealloc;
{
  [loginBox_ release];
  [startButton_ release];
  [retryButton_ release];
  [restartButton_ release];
  [recoveryHelpField_ release];
  [accountDir_ release];
  [expectedMid_ release];
  [qrImageView_ release];
  [statusField_ release];
  [pinField_ release];
  [chromeButton_ release];
  [windowsButton_ release];
  [super dealloc];
}

@end
