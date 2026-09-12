#import "QRLoginWindowController.h"
#import "ENILQRImage.h"
#import "ENILAccount.h"

static const int kQRPixels = 256;

/* The QR-login C bridging (callback trampolines + main-thread marshalling)
 * now lives behind +[ENILAccount runQRLoginAtPath:observer:cancelFlag:]; this
 * controller is the observer and receives all callbacks on the main thread. */
@interface QRLoginWindowController () <ENILQRLoginObserver>
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
    initWithContentRect:NSMakeRect(0, 0, 320, 400)
              styleMask:mask
                backing:NSBackingStoreBuffered
                  defer:NO] autorelease];
  [window setTitle:NSLocalizedString(@"Add Account", nil)];
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
  return f;
}

- (void)windowDidLoad;
{
  [super windowDidLoad];
  NSView *cv = [[self window] contentView];

  qrImageView_ = [[NSImageView alloc]
    initWithFrame:NSMakeRect(32, 120, kQRPixels, kQRPixels)];
  [qrImageView_ setImageScaling:XPImageScaleAxesIndependently];
  [cv addSubview:qrImageView_];

  statusField_ = [[self labelInRect:NSMakeRect(8, 80, 304, 20)] retain];
  [statusField_ setStringValue:NSLocalizedString(@"Starting…", nil)];
  [cv addSubview:statusField_];

  pinField_ = [[self labelInRect:NSMakeRect(8, 36, 304, 36)] retain];
  [pinField_ setFont:[NSFont boldSystemFontOfSize:28]];
  [pinField_ setStringValue:@""];
  [pinField_ setHidden:YES];
  [cv addSubview:pinField_];
}

- (void)start;
{
  if (running_) return;
  running_ = YES;
  [self showWindow:self];
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
  if (![s length]) return;
  [qrImageView_ setImage:[ENILQRImage imageForString:s pixelSize:kQRPixels]];
  [statusField_ setStringValue:NSLocalizedString(@"Scan with LINE on your phone", nil)];
}

- (void)qrLoginDidEmitPIN:(NSString *)s;
{
  if (![s length]) return;
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
  if (![s length]) return;
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
  [accountDir_ release];
  [expectedMid_ release];
  [qrImageView_ release];
  [statusField_ release];
  [pinField_ release];
  [super dealloc];
}

@end
