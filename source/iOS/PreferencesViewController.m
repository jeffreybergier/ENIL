//
//  PreferencesViewController.m
//  ENIL
//

#import "PreferencesViewController.h"
#import "ENILRootCoordinator.h"
#import "UIViewController+ENILModal.h"
#import "ENILKeychain.h"
#import "XPFoundation.h"
#import "XPUIKit.h"
#import <AltivecCore/AltivecCore.h>
#include <stdlib.h>

@interface PreferencesViewController () <UITextFieldDelegate>
@property (nonatomic, strong) UITextField *urlField;
@property (nonatomic, strong) UITextField *secretField;
@property (nonatomic, assign) BOOL workerDirty;        /* worker fields user-edited */
@property (nonatomic, copy)   NSArray *accounts;       /* coordinator summaries */
@property (nonatomic, copy)   NSString *statusMessage; /* inline worker footer error */
@property (nonatomic, copy)   NSDictionary *libraryVersions;   /* lib name -> version */
@property (nonatomic, copy)   NSArray *sortedLibraryKeys;      /* display order */
@end

@implementation PreferencesViewController

- (instancetype)init
{
  if ((self = [super initWithStyle:UITableViewStyleGrouped])) {
    [[self navigationItem] setTitle:NSLocalizedString(@"Settings", nil)];
  }
  return self;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  [self setUrlField:[self makeFieldSecure:NO
                            placeholder:@"https://your-worker.workers.dev"]];
  [self setSecretField:[self makeFieldSecure:YES
                               placeholder:NSLocalizedString(@"Shared Secret",
                                                             nil)]];
  ENILKeychain *kc = [ENILKeychain sharedKeychain];
  [[self urlField] setText:[kc workerURL]];
  [[self secretField] setText:[kc workerSecret]];

  /* A single Done button: it commits any edited worker credentials, then (when
   * presented modally) closes. There is no separate Save — see -doneAction.
   * Disabled as the nav root (gate / empty-state): there is no active account to
   * return to, so Done has nothing to do until one exists. dismissesOnDone is
   * exactly that condition (modal <=> an account is active — see the header). */
  [[self navigationItem] setRightBarButtonItem:[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                  target:self
                                                  action:@selector(doneAction)]];
  [[[self navigationItem] rightBarButtonItem] setEnabled:[self dismissesOnDone]];

  [self setLibraryVersions:[self linkedLibraryVersions]];
  [self setSortedLibraryKeys:[[[self libraryVersions] allKeys] sortedArrayUsingSelector:@selector(compare:)]];
}

- (void)viewWillAppear:(BOOL)animated
{
  [super viewWillAppear:animated];
  [self reloadAccounts];   /* refresh after an add / background log out */
}

- (void)viewDidAppear:(BOOL)animated
{
  [super viewDidAppear:animated];
  if (![[[self urlField] text] length]) [[self urlField] becomeFirstResponder];
}

- (void)reloadAccounts
{
  [self setAccounts:[self coordinator] ? [[self coordinator] accountSummaries] : [NSArray array]];
}

#pragma mark - Section layout

/* The Accounts section only exists with a coordinator to drive it; without one
 * (defensive) the table is worker-only. Section roles are resolved through
 * these so the row math reads the same in both shapes. */
- (BOOL)showsAccounts { return [self coordinator] != nil; }
- (NSInteger)accountsSection { return [self showsAccounts] ? 0 : -1; }
- (NSInteger)workerSection   { return [self showsAccounts] ? 1 : 0; }
/* Linked Libraries — read-only version list, always last. The iOS port of the
 * macOS AboutWindowController key/value table. */
- (NSInteger)librariesSection { return [self workerSection] + 1; }

- (BOOL)hasActiveAccount
{
  NSUInteger i, n = [[self accounts] count];
  for (i = 0; i < n; i++) {
    if ([[[[self accounts] objectAtIndex:i] objectForKey:@"active"] boolValue]) {
      return YES;
    }
  }
  return NO;
}

/* Row index of the Reauthenticate row (NSNotFound when no account is active —
 * reauth is current-account scoped), and the Add Account row, within the
 * Accounts section. Account rows occupy [0, accounts.count). */
- (NSUInteger)reauthRow
{
  return [self hasActiveAccount] ? [[self accounts] count] : NSNotFound;
}

- (NSUInteger)addAccountRow
{
  return [[self accounts] count] + ([self hasActiveAccount] ? 1 : 0);
}

#pragma mark - Field construction

- (UITextField *)makeFieldSecure:(BOOL)secure placeholder:(NSString *)placeholder
{
  UITextField *field = [[UITextField alloc] initWithFrame:CGRectZero];
  [field setPlaceholder:placeholder];
  [field setSecureTextEntry:secure];
  [field setDelegate:self];
  [field setAutocorrectionType:UITextAutocorrectionTypeNo];
  [field setAutocapitalizationType:UITextAutocapitalizationTypeNone];
  [field setClearButtonMode:UITextFieldViewModeWhileEditing];
  [field setContentVerticalAlignment:UIControlContentVerticalAlignmentCenter];
  [field setReturnKeyType:secure ? UIReturnKeyDone : UIReturnKeyNext];
  [field setKeyboardType:secure ? UIKeyboardTypeDefault : UIKeyboardTypeURL];
  [field addTarget:self
            action:@selector(fieldChanged:)
  forControlEvents:UIControlEventEditingChanged];
  return field;
}

#pragma mark - UITableViewDataSource

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
  (void)tableView;
  return ([self showsAccounts] ? 2 : 1) + 1;  /* + the Linked Libraries section */
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section
{
  (void)tableView;
  if (section == [self workerSection]) return 2;
  if (section == [self librariesSection])
    return (NSInteger)[[self sortedLibraryKeys] count];
  return (NSInteger)[self addAccountRow] + 1;  /* accounts + (reauth) + add */
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section
{
  (void)tableView;
  if (section == [self workerSection]) {
    return NSLocalizedString(@"Cloudflare Worker", nil);
  }
  if (section == [self librariesSection]) {
    return NSLocalizedString(@"Linked Libraries", nil);
  }
  return NSLocalizedString(@"Accounts", nil);
}

/* Worker footer doubles as the inline status line: a save / gate error wins,
 * then the env-override hint (mirrors macOS), then the resting help text. The
 * Accounts footer hints at swipe-to-log-out when there are accounts. */
- (NSString *)tableView:(UITableView *)tableView
titleForFooterInSection:(NSInteger)section
{
  (void)tableView;
  if (section == [self accountsSection]) {
    return [[self accounts] count]
      ? NSLocalizedString(@"Swipe an account to log out.", nil)
      : nil;
  }
  /* Only the worker section carries a footer below; Linked Libraries has none. */
  if (section != [self workerSection]) return nil;
  if ([[self statusMessage] length]) return [self statusMessage];
  if ([self envOverrideActive]) {
    return NSLocalizedString(
      @"Environment variables override these settings while set.", nil);
  }
  return NSLocalizedString(
    @"Enter the Cloudflare Worker URL and shared secret. Both are stored in "
    @"the keychain.", nil);
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
  if ([indexPath section] == [self workerSection]) {
    return [self workerCellForRow:[indexPath row] inTableView:tableView];
  }
  if ([indexPath section] == [self librariesSection]) {
    return [self libraryCellForRow:[indexPath row] inTableView:tableView];
  }
  return [self accountCellForRow:(NSUInteger)[indexPath row] inTableView:tableView];
}

- (UITableViewCell *)workerCellForRow:(NSInteger)row
                          inTableView:(UITableView *)tableView
{
  (void)tableView;
  UITableViewCell *cell =
    [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                           reuseIdentifier:nil];
  [cell setSelectionStyle:UITableViewCellSelectionStyleNone];
  UITextField *field = (row == 0) ? [self urlField] : [self secretField];
  [field setFrame:CGRectInset([[cell contentView] bounds], 15.0, 0.0)];
  [field setAutoresizingMask:UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight];
  [[cell contentView] addSubview:field];
  return cell;
}

- (UITableViewCell *)accountCellForRow:(NSUInteger)row
                           inTableView:(UITableView *)tableView
{
  static NSString *cellId = @"AccountCell";
  UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:cellId];
  if (cell == nil) {
    cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                  reuseIdentifier:cellId];
  }
  [cell setAccessoryType:UITableViewCellAccessoryNone];
  [[cell textLabel] setTextColor:[UIColor blackColor]];

  if (row < [[self accounts] count]) {
    NSDictionary *acct = [[self accounts] objectAtIndex:row];
    [[cell textLabel] setText:[acct objectForKey:@"displayName"]];
    [cell setAccessoryType:[[acct objectForKey:@"active"] boolValue]
        ? UITableViewCellAccessoryCheckmark
        : UITableViewCellAccessoryNone];
  } else if (row == [self reauthRow]) {
    [[cell textLabel] setText:NSLocalizedString(@"Reauthenticate", nil)];
    [[cell textLabel] setTextColor:[UIColor blueColor]];
  } else {
    [[cell textLabel] setText:NSLocalizedString(@"Add Account", nil)];
    [[cell textLabel] setTextColor:[UIColor blueColor]];
  }
  return cell;
}

/* Bundled C-library versions for the Linked Libraries section. Calling
 * cJSON_Version() / sqlite3_libversion() directly here is sanctioned (see
 * AGENTS.md) — read-only version strings, same allowance as the About box. */
- (NSDictionary *)linkedLibraryVersions
{
  return @{
    @"libz":      [NSString stringWithUTF8String:zlibVersion()],
    @"libssl":    [NSString stringWithUTF8String:
                     OpenSSL_version(OPENSSL_VERSION)],
    @"libcurl":   [NSString stringWithUTF8String:curl_version()],
    @"cJSON":     [NSString stringWithUTF8String:cJSON_Version()],
    @"sqlite3":   [NSString stringWithUTF8String:sqlite3_libversion()],
    @"qrcodegen": NSLocalizedString(@"no version provided", nil)
  };
}

/* Read-only library/version row (Value1: name on the left, version on the
 * right), mirroring the macOS About box. */
- (UITableViewCell *)libraryCellForRow:(NSInteger)row
                           inTableView:(UITableView *)tableView
{
  static NSString *cellId = @"LibraryCell";
  UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:cellId];
  if (cell == nil) {
    cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                  reuseIdentifier:cellId];
  }
  [cell setSelectionStyle:UITableViewCellSelectionStyleNone];
  NSString *key = [[self sortedLibraryKeys] objectAtIndex:(NSUInteger)row];
  [[cell textLabel] setText:key];
  [[cell detailTextLabel] setText:[[self libraryVersions] objectForKey:key]];
  return cell;
}

#pragma mark - Editing (swipe-to-log-out)

- (BOOL)tableView:(UITableView *)tableView
canEditRowAtIndexPath:(NSIndexPath *)indexPath
{
  (void)tableView;
  return [indexPath section] == [self accountsSection] &&
         (NSUInteger)[indexPath row] < [[self accounts] count];
}

- (UITableViewCellEditingStyle)tableView:(UITableView *)tableView
           editingStyleForRowAtIndexPath:(NSIndexPath *)indexPath
{
  (void)tableView;
  return [self tableView:tableView canEditRowAtIndexPath:indexPath]
    ? UITableViewCellEditingStyleDelete
    : UITableViewCellEditingStyleNone;
}

- (NSString *)tableView:(UITableView *)tableView
titleForDeleteConfirmationButtonForRowAtIndexPath:(NSIndexPath *)indexPath
{
  (void)tableView; (void)indexPath;
  return NSLocalizedString(@"Log Out", nil);
}

- (void)tableView:(UITableView *)tableView
commitEditingStyle:(UITableViewCellEditingStyle)editingStyle
forRowAtIndexPath:(NSIndexPath *)indexPath
{
  if (editingStyle != UITableViewCellEditingStyleDelete) return;
  NSString *path = [self pathForAccountRow:(NSUInteger)[indexPath row]];
  if (![path length]) return;
  /* YES => the active account went away and the coordinator already reshaped the
   * root + dismissed this modal; nothing more to do. NO => a background account
   * was removed in place, so refresh the list. */
  if (![[self coordinator] logOutAccountAtPath:path]) {
    [self reloadAccounts];
    [tableView reloadData];
  }
}

#pragma mark - UITableViewDelegate

- (void)tableView:(UITableView *)tableView
didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if ([indexPath section] != [self accountsSection]) return;

  NSUInteger row = (NSUInteger)[indexPath row];
  if (row < [[self accounts] count]) {
    [self switchToAccountRow:row];
  } else if (row == [self reauthRow]) {
    [[self coordinator] presentReauthFromViewController:self];
  } else {
    [self addAccount];
  }
}

- (void)switchToAccountRow:(NSUInteger)row
{
  NSDictionary *acct = [[self accounts] objectAtIndex:row];
  if ([[acct objectForKey:@"active"] boolValue]) return;  /* already active */
  /* On success the coordinator activates the engine and dismisses this modal. */
  [[self coordinator] switchToAccountAtPath:[acct objectForKey:@"path"]];
}

- (void)addAccount
{
  /* Persist any just-typed credentials first, so the gate below reads them
   * (the QR flow needs the worker before its first call). */
  BOOL ready = [self commitWorkerCredentialsIfNeeded];
  if (ready && ![[ENILKeychain sharedKeychain] hasCredentials]) {
    [self setStatusMessageAndReload:NSLocalizedString(
      @"Set the Worker URL and shared secret first.", nil)];
    ready = NO;
  }
  if (!ready) {
    [self XP_showAlertWithTitle:NSLocalizedString(@"Couldn't Add Account", nil)
                       message:[self statusMessage]
                  dismissTitle:NSLocalizedString(@"OK", nil)];
    return;
  }
  [[self coordinator] presentAddAccountFromViewController:self];
}

- (NSString *)pathForAccountRow:(NSUInteger)row
{
  if (row >= [[self accounts] count]) return nil;
  return [[[self accounts] objectAtIndex:row] objectForKey:@"path"];
}

#pragma mark - UITextFieldDelegate

- (BOOL)textFieldShouldReturn:(UITextField *)textField
{
  if (textField == [self urlField]) {
    [[self secretField] becomeFirstResponder];
    return NO;
  }
  [textField resignFirstResponder];
  /* Keyboard Done commits the edit but stays on screen — only the nav-bar Done
   * closes (see -doneAction). */
  [self commitWorkerCredentialsIfNeeded];
  return NO;
}

#pragma mark - Actions

- (void)fieldChanged:(id)sender
{
  (void)sender;
  [self setWorkerDirty:YES];  /* programmatic prefill doesn't fire this, only typing */
}

/* The single Done verb: commit any pending worker-credential edit, then close
 * if presented modally. As the nav root (the gate / empty-state) there is
 * nothing to dismiss to — the commit's keychain-change notification is what
 * drives the coordinator forward. On a validation/save error -commit... has
 * already surfaced it in the footer, so we stay put. */
- (void)doneAction
{
  [[self view] endEditing:YES];
  if (![self commitWorkerCredentialsIfNeeded]) return;
  if ([self dismissesOnDone]) [self enil_dismissModalViewController];
}

/* Returns YES when it is safe to proceed (saved, or nothing changed); NO when a
 * validation/save error was surfaced and the caller should stay on screen.
 * Saving posts ENILKeychainDidChangeNotification, which ENILRootCoordinator
 * observes to continue a launch gated on missing credentials. */
- (BOOL)commitWorkerCredentialsIfNeeded
{
  if (![self workerDirty]) return YES;
  NSString *url = [self trimmed:[[self urlField] text]];
  NSString *secret = [self trimmed:[[self secretField] text]];
  if (![url length] || ![secret length]) {
    [self setStatusMessageAndReload:
      NSLocalizedString(@"Both fields are required", nil)];
    return NO;
  }
  if (![[ENILKeychain sharedKeychain] saveWorkerURL:url secret:secret]) {
    ENILLog(@"PreferencesViewController.commit", @"keychain write failed");
    [self setStatusMessageAndReload:NSLocalizedString(
      @"Could not save to the keychain. Check the URL and try again.", nil)];
    return NO;
  }
  ENILLog(@"PreferencesViewController.commit",
          @"saved worker credentials (url=%@)", url);
  [self setWorkerDirty:NO];
  return YES;
}

#pragma mark - Helpers

- (NSString *)trimmed:(NSString *)string
{
  return [string stringByTrimmingCharactersInSet:
    [NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

- (void)setStatusMessageAndReload:(NSString *)message
{
  [self setStatusMessage:message];
  [[self tableView] reloadData];
}

- (BOOL)envOverrideActive
{
  const char *url = getenv("ENIL_WORKER_URL");
  const char *secret = getenv("ENIL_WORKER_SECRET");
  return (url && *url) || (secret && *secret);
}

- (void)dealloc
{
  [_urlField setDelegate:nil];
  [_secretField setDelegate:nil];
}

@end
