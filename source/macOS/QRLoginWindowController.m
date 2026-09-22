#import "QRLoginWindowController.h"
#import "ENILQRImage.h"
#import "ENILAccount.h"
#import "XPFoundation.h"

static const int kQRPixels = 256;
/* Each step gets only the space it needs; PIN verification replaces the QR. */
static const CGFloat kLoginWindowWidth = 460;
static const CGFloat kClientPageHeight = 244;
static const CGFloat kLoginPageHeight = 466;
static const CGFloat kVerificationPageHeight = 300;

/* The QR-login C bridging (callback trampolines + main-thread marshalling)
 * now lives behind +[ENILAccount runQRLoginAtPath:observer:cancelFlag:]; this
 * controller is the observer and receives all callbacks on the main thread. */
@interface QRLoginWindowController () <ENILQRLoginObserver>
- (void)beginLoginWithProfile:(NSString *)profile;
- (void)chooseClient:(id)sender;
- (void)startSelectedLogin:(id)sender;
- (void)showRecoveryActions;
- (void)showPage:(NSView *)page;
- (void)setLoginStatus:(NSString *)status;
- (void)goBack:(id)sender;
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
    preparedPaths_ = [[NSMutableDictionary alloc] init];
    stagingPaths_ = [[NSMutableArray alloc] initWithObjects:accountDir, nil];
  }
  return self;
}

- (NSString *)expectedMid { return expectedMid_; }

- (void)loadWindow;
{
  XPWindowStyleMask mask = XPWindowStyleMaskTitled
                         | XPWindowStyleMaskClosable;
  NSWindow *window = [[[NSWindow alloc]
    initWithContentRect:NSMakeRect(0, 0, kLoginWindowWidth, kClientPageHeight)
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
  clientPage_ = [[NSView alloc] initWithFrame:
    NSMakeRect(0, 0, kLoginWindowWidth, kClientPageHeight)];
  loginPage_ = [[NSView alloc] initWithFrame:
    NSMakeRect(0, 0, kLoginWindowWidth, kLoginPageHeight)];
  verificationPage_ = [[NSView alloc] initWithFrame:
    NSMakeRect(0, 0, kLoginWindowWidth, kVerificationPageHeight)];
  NSBox *clientBox = [[[NSBox alloc]
    initWithFrame:NSMakeRect(20, 102, kLoginWindowWidth - 40, 122)] autorelease];
  [clientBox setTitle:NSLocalizedString(@"Client", nil)];
  [clientBox setContentViewMargins:NSMakeSize(12, 8)];
  [clientBox setAutoresizingMask:NSViewMinYMargin];
  [clientPage_ addSubview:clientBox];
  selectionStatusField_ = [[self labelInRect:NSMakeRect(28, 58, kLoginWindowWidth - 56, 40)] retain];
  [clientPage_ addSubview:selectionStatusField_];
  NSView *clients = [clientBox contentView];
  CGFloat clientWidth = [clients bounds].size.width;
  CGFloat clientHeight = [clients bounds].size.height;

  /* Center the choices as one unit when NSBox settles its content bounds on
   * Tiger. Offset slightly to center the radio artwork, which sits above
   * the center of its control frames. */
  NSView *choices = [[[NSView alloc] initWithFrame:
    NSMakeRect(0, (clientHeight - 74) / 2 - 3, clientWidth, 74)] autorelease];
  [choices setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin];
  [clients addSubview:choices];
  chromeButton_ = [[NSButton alloc] initWithFrame:NSMakeRect(8, 52, clientWidth - 16, 22)];
  [chromeButton_ setTitle:NSLocalizedString(@"Web (Chrome)", nil)];
  [chromeButton_ setButtonType:XPButtonTypeRadio];
  [chromeButton_ setTarget:self];
  [chromeButton_ setAction:@selector(chooseClient:)];
  [chromeButton_ setState:XPControlStateOn];
  [chromeButton_ setAutoresizingMask:NSViewWidthSizable];
  [choices addSubview:chromeButton_];
  windowsButton_ = [[NSButton alloc] initWithFrame:NSMakeRect(8, 26, clientWidth - 16, 22)];
  [windowsButton_ setTitle:NSLocalizedString(@"Desktop (Windows)", nil)];
  [windowsButton_ setButtonType:XPButtonTypeRadio];
  [windowsButton_ setTarget:self];
  [windowsButton_ setAction:@selector(chooseClient:)];
  [windowsButton_ setState:XPControlStateOff];
  [windowsButton_ setAutoresizingMask:NSViewWidthSizable];
  [choices addSubview:windowsButton_];
  androidButton_ = [[NSButton alloc] initWithFrame:NSMakeRect(8, 0, clientWidth - 16, 22)];
  [androidButton_ setTitle:NSLocalizedString(@"Tablet (Android)", nil)];
  [androidButton_ setButtonType:XPButtonTypeRadio];
  [androidButton_ setTarget:self];
  [androidButton_ setAction:@selector(chooseClient:)];
  [androidButton_ setState:XPControlStateOff];
  [androidButton_ setAutoresizingMask:NSViewWidthSizable];
  [choices addSubview:androidButton_];
  [chromeButton_ setHidden:expectedMid_ != nil];
  [windowsButton_ setHidden:expectedMid_ != nil];
  [androidButton_ setHidden:expectedMid_ != nil];
  if (expectedMid_) {
    NSTextField *savedClient = [self labelInRect:NSMakeRect(8, 4, clientWidth - 16, clientHeight - 8)];
    [savedClient setStringValue:NSLocalizedString(@"This account's saved client identity will be used.", nil)];
    [savedClient setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [clients addSubview:savedClient];
  }

  loginBox_ = [[NSBox alloc] initWithFrame:NSMakeRect(20, 56, kLoginWindowWidth - 40, 390)];
  [loginBox_ setContentViewMargins:NSMakeSize(12, 12)];
  [loginBox_ setAutoresizingMask:NSViewMinYMargin];
  [loginPage_ addSubview:loginBox_];
  NSView *login = [loginBox_ contentView];
  CGFloat width = [login bounds].size.width;
  CGFloat height = [login bounds].size.height;

  qrImageView_ = [[NSImageView alloc]
    initWithFrame:NSMakeRect((width - kQRPixels) / 2, height - 12 - kQRPixels, kQRPixels, kQRPixels)];
  [qrImageView_ setImageScaling:XPImageScaleAxesIndependently];
  /* Tiger tiles NSBox's content view on first display. Anchor children to its
   * final bounds when a page is installed or the window animates. */
  [qrImageView_ setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin];
  [login addSubview:qrImageView_];

  statusField_ = [[self labelInRect:NSMakeRect(8, 8, width - 16, 64)] retain];
  [self setLoginStatus:@""];
  [statusField_ setAutoresizingMask:NSViewWidthSizable];
  [login addSubview:statusField_];
  NSBox *verificationBox = [[[NSBox alloc]
    initWithFrame:NSMakeRect(20, 56, kLoginWindowWidth - 40, 224)] autorelease];
  [verificationBox setTitle:NSLocalizedString(@"Verify on your phone", nil)];
  [verificationBox setContentViewMargins:NSMakeSize(12, 12)];
  [verificationBox setAutoresizingMask:NSViewMinYMargin];
  [verificationPage_ addSubview:verificationBox];
  NSView *verification = [verificationBox contentView];
  CGFloat verificationWidth = [verification bounds].size.width;
  CGFloat verificationHeight = [verification bounds].size.height;
  NSTextField *instructions = [self labelInRect:
    NSMakeRect(8, verificationHeight - 48, verificationWidth - 16, 36)];
  [instructions setStringValue:NSLocalizedString(@"Enter this code in LINE on your phone.", nil)];
  [instructions setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
  [verification addSubview:instructions];
  pinField_ = [[self labelInRect:
    NSMakeRect(8, verificationHeight - 112, verificationWidth - 16, 56)] retain];
  [pinField_ setFont:[NSFont boldSystemFontOfSize:36]];
  [pinField_ setSelectable:YES];
  [pinField_ setStringValue:@""];
  [pinField_ setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
  [verification addSubview:pinField_];
  verificationStatusField_ = [[self labelInRect:
    NSMakeRect(8, 8, verificationWidth - 16, 48)] retain];
  [verificationStatusField_ setAutoresizingMask:NSViewWidthSizable];
  [verification addSubview:verificationStatusField_];

  CGFloat buttonWidth = 260;
  CGFloat buttonX = (width - buttonWidth) / 2;
  nextButton_ = [[self actionButtonInRect:NSMakeRect(kLoginWindowWidth - 120, 14, 100, 32)
    title:NSLocalizedString(@"Next", nil) action:@selector(startSelectedLogin:)] retain];
  backButton_ = [[self actionButtonInRect:NSMakeRect(20, 14, 100, 32)
    title:NSLocalizedString(@"Back", nil) action:@selector(goBack:)] retain];
  [nextButton_ setAutoresizingMask:NSViewMinXMargin | NSViewMaxYMargin];
  [backButton_ setAutoresizingMask:NSViewMaxXMargin | NSViewMaxYMargin];
  retryButton_ = [[self actionButtonInRect:NSMakeRect(buttonX, height - 120, buttonWidth, 32)
    title:NSLocalizedString(@"Retry saved login", nil) action:@selector(retrySavedLogin:)] retain];
  restartButton_ = [[self actionButtonInRect:NSMakeRect(buttonX, height - 164, buttonWidth, 32)
    title:NSLocalizedString(@"Restart Login", nil) action:@selector(startNewQR:)] retain];
  [login addSubview:retryButton_];
  [login addSubview:restartButton_];
  recoveryHelpField_ = [[self labelInRect:NSMakeRect(8, 90, width - 16, 72)] retain];
  [recoveryHelpField_ setStringValue:NSLocalizedString(
    @"Retry resumes the saved login. Restart creates a new QR code.", nil)];
  [recoveryHelpField_ setAutoresizingMask:NSViewWidthSizable];
  [login addSubview:recoveryHelpField_];
  [self showPage:clientPage_];
}

/* Retained pages make a small wizard. Pin the window's top edge during
 * page changes, using the same Tiger-compatible animation as Preferences. */
- (void)showPage:(NSView *)page;
{
  if (currentPage_ == page) return;
  currentPage_ = page;
  BOOL login = page != clientPage_;
  NSWindow *window = [self window];
  CGFloat height = page == clientPage_ ? kClientPageHeight :
    (page == verificationPage_ ? kVerificationPageHeight : kLoginPageHeight);
  NSRect frame = [window frameRectForContentRect:NSMakeRect(0, 0, kLoginWindowWidth, height)];
  NSRect oldFrame = [window frame];
  frame.origin = NSMakePoint(oldFrame.origin.x, NSMaxY(oldFrame) - frame.size.height);
  /* A short, centered first page can otherwise grow below a small display. */
  NSScreen *screen = [window screen];
  if (screen) {
    NSRect visible = [screen visibleFrame];
    if (frame.size.height <= visible.size.height)
      frame.origin.y = MAX(NSMinY(visible), MIN(frame.origin.y, NSMaxY(visible) - frame.size.height));
  }
  [window makeFirstResponder:nil];
  [nextButton_ removeFromSuperview];
  [backButton_ removeFromSuperview];
  [page addSubview:nextButton_];
  [page addSubview:backButton_];
  [nextButton_ setHidden:login];
  [backButton_ setEnabled:login && !goingBack_];
  [window setDefaultButtonCell:login ? nil : [nextButton_ cell]];
  [window setContentView:page];
  [window setFrame:frame display:YES animate:[window isVisible]];
  if (!login) [window makeFirstResponder:expectedMid_ ? nextButton_ : chromeButton_];
}

/* Both sign-in pages show the same progress/error stream, including errors
 * that arrive after the QR has been replaced by phone verification. */
- (void)setLoginStatus:(NSString *)status;
{
  [statusField_ setStringValue:status];
  [verificationStatusField_ setStringValue:status];
}

- (void)showRecoveryActions;
{
  [loginBox_ setTitle:NSLocalizedString(@"Login", nil)];
  [qrImageView_ setHidden:YES];
  [pinField_ setHidden:YES];
  [retryButton_ setHidden:NO];
  [restartButton_ setHidden:NO];
  [recoveryHelpField_ setHidden:NO];
  [self showPage:loginPage_];
  [[self window] setDefaultButtonCell:[retryButton_ cell]];
}

- (void)goBack:(id)sender;
{
  (void)sender;
  if (done_ || goingBack_ || currentPage_ == clientPage_) return;
  if (running_) {
    /* Wait for finishWithResult: before resetting cancellation or changing
     * staging. A late success still follows the normal activation path. */
    goingBack_ = YES;
    cancelFlag_ = 1;
    [backButton_ setEnabled:NO];
    [qrImageView_ setImage:nil];
    [pinField_ setHidden:YES];
    [self setLoginStatus:NSLocalizedString(@"Stopping login…", nil)];
    return;
  }
  [selectionStatusField_ setStringValue:@""];
  [self showPage:clientPage_];
}

- (void)start;
{
  if (running_ || done_) return;
  [self showWindow:self];
}

- (void)chooseClient:(id)sender;
{
  if (running_ || done_ || currentPage_ != clientPage_ || expectedMid_) return;
  [windowsButton_ setState:sender == windowsButton_ ? XPControlStateOn : XPControlStateOff];
  [chromeButton_ setState:sender == chromeButton_ ? XPControlStateOn : XPControlStateOff];
  [androidButton_ setState:sender == androidButton_ ? XPControlStateOn : XPControlStateOff];
}

- (void)startSelectedLogin:(id)sender;
{
  (void)sender;
  if (currentPage_ != clientPage_) return;
  [self beginLoginWithProfile:expectedMid_ ? nil :
    ([androidButton_ state] == XPControlStateOn ? @"android" :
     ([windowsButton_ state] == XPControlStateOn ? @"desktopwin" : @"chrome"))];
}

- (void)beginLoginWithProfile:(NSString *)profile;
{
  if (running_ || done_) return;
  NSString *key = expectedMid_ ? @"reauth" : profile;
  NSString *path = [preparedPaths_ objectForKey:key];
  if (!path) {
    /* Keep separate staging for each client. Back must not rewrite a saved
     * identity or retire an uncertain token request. Returning to a client
     * resumes its attempt. Durable native recovery lives outside staging. */
    path = accountDir_;
    if ([preparedPaths_ count]) {
      path = [[accountDir_ stringByDeletingLastPathComponent] stringByAppendingPathComponent:
        [@".staging-" stringByAppendingString:[[NSProcessInfo processInfo] globallyUniqueString]]];
      if (![[NSFileManager defaultManager] XP_createDirectoryAtPath:path
          withIntermediateDirectories:NO attributes:nil error:NULL]) {
        [selectionStatusField_ setStringValue:NSLocalizedString(@"Could not save client identity", nil)];
        return;
      }
      [stagingPaths_ addObject:path];
    }
    if (![ENILAccount prepareQRLoginAtPath:path clientProfile:profile
                      reauthenticatingMid:expectedMid_]) {
      [selectionStatusField_ setStringValue:NSLocalizedString(@"Could not save client identity", nil)];
      return;
    }
    [preparedPaths_ setObject:path forKey:key];
  }
  [path retain];
  [accountDir_ release];
  accountDir_ = path;
  [selectionStatusField_ setStringValue:@""];
  [self retrySavedLogin:nil];
}

- (void)startNewQR:(id)sender;
{
  (void)sender;
  if (running_ || done_) return;
  if (![ENILAccount restartQRLoginAtPath:accountDir_]) {
    [self setLoginStatus:NSLocalizedString(@"Could not save login recovery", nil)];
    return;
  }
  [self retrySavedLogin:nil];
}

- (void)retrySavedLogin:(id)sender;
{
  (void)sender;
  if (running_ || done_) return;
  cancelFlag_ = 0;
  running_ = YES;
  [qrImageView_ setImage:nil];
  [pinField_ setHidden:YES];
  [pinField_ setStringValue:@""];
  [retryButton_ setHidden:YES];
  [restartButton_ setHidden:YES];
  [recoveryHelpField_ setHidden:YES];
  [loginBox_ setTitle:NSLocalizedString(@"QR Code", nil)];
  [[self window] setDefaultButtonCell:nil];
  [qrImageView_ setHidden:NO];
  [self setLoginStatus:NSLocalizedString(@"Starting…", nil)];
  [self showPage:loginPage_];
  if (done_) { running_ = NO; return; }
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
  if (!running_ || done_ || goingBack_ || ![s length]) return;
  [qrImageView_ setImage:[ENILQRImage imageForString:s pixelSize:kQRPixels]];
  [self setLoginStatus:NSLocalizedString(@"Scan with LINE on your phone", nil)];
}

- (void)qrLoginDidEmitPIN:(NSString *)s;
{
  if (!running_ || done_ || goingBack_ || ![s length]) return;
  [pinField_ setStringValue:s];
  [pinField_ setHidden:NO];
  [self setLoginStatus:NSLocalizedString(@"Waiting for confirmation…", nil)];
  /* The PIN callback, not a timer or a guessed status string, advances the
   * wizard. Certificate-approved logins can finish without this step. */
  [self showPage:verificationPage_];
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
  if (!running_ || done_ || goingBack_ || ![s length]) return;
  int cur = 0, total = 0;
  if (sscanf([s UTF8String], "waiting for scan (%d/%d)", &cur, &total) == 2) {
    [self setLoginStatus:
      [NSString stringWithFormat:
        NSLocalizedString(@"waiting for scan (%d/%d)", nil), cur, total]];
    return;
  }
  [self setLoginStatus:NSLocalizedString(s, nil)];
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
  if (goingBack_) {
    goingBack_ = NO;
    cancelFlag_ = 0;
    if (![result boolValue]) {
      [selectionStatusField_ setStringValue:@""];
      [self showPage:clientPage_];
      return;
    }
  }
  if (![result boolValue] && [ENILAccount canRestartQRLoginAtPath:accountDir_]) {
    NSAlert *alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:NSLocalizedString(@"Sign-in needs recovery", nil)];
    [alert setInformativeText:[statusField_ stringValue]];
    [alert addButtonWithTitle:NSLocalizedString(@"Dismiss", nil)];
    [qrImageView_ setImage:nil];
    [pinField_ setStringValue:@""];
    [self setLoginStatus:NSLocalizedString(@"Sign-in needs recovery", nil)];
    [self showRecoveryActions];
    [alert XP_beginSheetModalForWindow:[self window]
                         modalDelegate:self
                        didEndSelector:@selector(recoveryAlertDidEnd:returnCode:contextInfo:)
                           contextInfo:NULL];
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
    [self setLoginStatus:NSLocalizedString(@"Couldn't finish adding the account", nil)];
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

  [self setLoginStatus:NSLocalizedString(@"Login cancelled", nil)];
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

/* Dismissing a recoverable error leaves the saved login and its actions open. */
- (void)recoveryAlertDidEnd:(NSAlert *)alert
                 returnCode:(NSInteger)returnCode
                contextInfo:(void *)contextInfo;
{
  (void)returnCode;
  (void)contextInfo;
  [[alert window] orderOut:nil];
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
  /* The delegate cleans the active path; release the other transient choices.
   * Durable native recovery directories are not in this array. */
  NSFileManager *fm = [NSFileManager defaultManager];
  NSEnumerator *paths = [stagingPaths_ objectEnumerator];
  NSString *path;
  while ((path = [paths nextObject])) {
    if (![path isEqualToString:accountDir_]) [fm XP_removeItemAtPath:path error:NULL];
  }
  [preparedPaths_ release];
  [stagingPaths_ release];
  [clientPage_ release];
  [loginPage_ release];
  [verificationPage_ release];
  [verificationStatusField_ release];
  [selectionStatusField_ release];
  [nextButton_ release];
  [backButton_ release];
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
  [androidButton_ release];
  [super dealloc];
}

@end
