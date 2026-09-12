#import "PreferencesWindowController.h"
#import "ENILKeychain.h"
#import "XPFoundation.h"
#import "AIFontAwesome.h"
#include <stdlib.h>

/* Pane-content metrics. The pane view is sized to kPaneW × kPaneH and the
 * window auto-grows around it (toolbar + title bar) via frameRectForContentRect:.
 * Resizing in -selectPaneWithIdentifier: lets each pane declare its own height
 * the day a second pane is added. */
static const CGFloat kPaneW       = 540.0;
static const CGFloat kPaneH       = 180.0;
static const CGFloat kMargin      = 20.0;
static const CGFloat kLabelW      = 120.0;
static const CGFloat kRowH        = 24.0;
static const CGFloat kRowGap      = 12.0;
static const CGFloat kButtonW     = 96.0;
static const CGFloat kButtonH     = 32.0;
static const CGFloat kButtonGap   = 8.0;
static const CGFloat kIconPt      = 24.0;
static const CGFloat kIconCanvas  = 32.0;
/* Bezeled NSTextField text sits ~3pt below the top of the frame (cell
 * draws text vertically centered inside the bezel's inner padding),
 * but unbezeled label NSTextFields draw text from the very top of the
 * frame. Drop labels by this amount so their baseline aligns with the
 * field's text baseline — same effect IB applies to form labels. */
static const CGFloat kLabelBaselineDrop = 3.0;

/* Toolbar identifiers. One identifier per pane — the value also doubles as
 * the toolbar's selectedItemIdentifier, which is what gives the clicked item
 * its highlighted "tab" appearance. */
static NSString *const kTbWorker = @"ENILPrefsTb.Worker";

@interface PreferencesWindowController (Private) <XPToolbarDelegate>
- (NSTextField *)labelWithFrame:(NSRect)f text:(NSString *)t bold:(BOOL)bold;
- (void)refreshEnvHint;
- (NSView *)buildWorkerPaneView;
- (void)selectPaneWithIdentifier:(NSString *)identifier;
- (NSToolbarItem *)makeToolbarItemWithIdentifier:(NSString *)ident
                                            icon:(AIFontAwesomeIcon)icon
                                           label:(NSString *)label;
@end

@implementation PreferencesWindowController

- (id)init { return [super initWithWindowNibName:@"ignored"]; }

- (void)loadWindow;
{
  XPWindowStyleMask mask = XPWindowStyleMaskTitled | XPWindowStyleMaskClosable;
  NSRect frame = NSMakeRect(0, 0, kPaneW, kPaneH);
  NSWindow *window = [[[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:mask
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO] autorelease];
  [window setTitle:NSLocalizedString(@"Preferences", nil)];
  [window setReleasedWhenClosed:NO];
  [window setShowsToolbarButton:NO];

  /* Critical ordering: set the window on the controller BEFORE attaching
   * the toolbar. -setToolbar: synchronously invokes the delegate to
   * populate default items, and -toolbar:itemForItemIdentifier:... calls
   * [self window] for the icon rasterizer's backing-scale lookup. If the
   * window hasn't been installed via -setWindow: yet, that [self window]
   * call re-enters -loadWindow → infinite recursion → freeze. */
  [self setWindow:window];

  /* Preferences-style toolbar. Icon-and-label is the System Preferences /
   * Safari look. Customization is off because each item is a pane selector,
   * not a user-configurable button. NSToolbar exists since 10.0; the
   * selectable-item delegate method is 10.4+ — both within the Tiger floor. */
  NSToolbar *tb = [[[NSToolbar alloc]
    initWithIdentifier:@"ENILPrefsToolbar"] autorelease];
  [tb setDelegate:self];
  [tb setAllowsUserCustomization:NO];
  [tb setAutosavesConfiguration:NO];
  [tb setDisplayMode:NSToolbarDisplayModeIconAndLabel];
  [tb setSizeMode:NSToolbarSizeModeDefault];
  [tb setSelectedItemIdentifier:kTbWorker];
  [window setToolbar:tb];

  [window center];
}

- (void)windowDidLoad;
{
  [super windowDidLoad];
  [self selectPaneWithIdentifier:kTbWorker];
}

#pragma mark - Pane assembly

- (NSView *)buildWorkerPaneView;
{
  NSView *root = [[[NSView alloc] initWithFrame:
    NSMakeRect(0, 0, kPaneW, kPaneH)] autorelease];

  ENILKeychain *kc = [ENILKeychain sharedKeychain];
  NSString *curURL    = [kc workerURL];
  NSString *curSecret = [kc workerSecret];

  CGFloat y = kPaneH - kMargin - kRowH;

  NSTextField *urlLabel = [self
    labelWithFrame:NSMakeRect(kMargin, y - kLabelBaselineDrop, kLabelW, kRowH)
              text:NSLocalizedString(@"Worker URL:", nil)
              bold:NO];
  [root addSubview:urlLabel];

  NSRect fieldF = NSMakeRect(kMargin + kLabelW, y,
                             kPaneW - 2 * kMargin - kLabelW, kRowH);
  urlField_ = [[NSTextField alloc] initWithFrame:fieldF];
  [urlField_ setStringValue:curURL ? curURL : @""];
  [[urlField_ cell] setPlaceholderString:@"https://your-worker.workers.dev"];
  [root addSubview:urlField_];

  y -= (kRowH + kRowGap);

  NSTextField *secretLabel = [self
    labelWithFrame:NSMakeRect(kMargin, y - kLabelBaselineDrop, kLabelW, kRowH)
              text:NSLocalizedString(@"Shared Secret:", nil)
              bold:NO];
  [root addSubview:secretLabel];

  fieldF.origin.y = y;
  secretField_ = [[NSSecureTextField alloc] initWithFrame:fieldF];
  [secretField_ setStringValue:curSecret ? curSecret : @""];
  [root addSubview:secretField_];

  y -= (kRowH + kRowGap);

  envHint_ = [[NSTextField alloc] initWithFrame:
    NSMakeRect(kMargin, y, kPaneW - 2 * kMargin, kRowH)];
  [envHint_ setStringValue:@""];
  [envHint_ setBezeled:NO];
  [envHint_ setDrawsBackground:NO];
  [envHint_ setEditable:NO];
  [envHint_ setSelectable:NO];
  [envHint_ setFont:[NSFont systemFontOfSize:11.0]];
  [envHint_ setTextColor:[NSColor disabledControlTextColor]];
  [root addSubview:envHint_];
  [self refreshEnvHint];

  NSRect saveF   = NSMakeRect(kPaneW - kMargin - kButtonW,
                              kMargin, kButtonW, kButtonH);
  NSRect cancelF = NSMakeRect(kPaneW - kMargin - 2 * kButtonW - kButtonGap,
                              kMargin, kButtonW, kButtonH);

  NSButton *save = [[[NSButton alloc] initWithFrame:saveF] autorelease];
  [save setTitle:NSLocalizedString(@"Save", nil)];
  [save setBezelStyle:XPBezelStyleRounded];
  [save setKeyEquivalent:@"\r"];
  [save setTarget:self];
  [save setAction:@selector(saveAction:)];
  [root addSubview:save];

  NSButton *cancel = [[[NSButton alloc] initWithFrame:cancelF] autorelease];
  [cancel setTitle:NSLocalizedString(@"Cancel", nil)];
  [cancel setBezelStyle:XPBezelStyleRounded];
  [cancel setKeyEquivalent:@"\033"];
  [cancel setTarget:self];
  [cancel setAction:@selector(cancelAction:)];
  [root addSubview:cancel];

  return root;
}

- (void)selectPaneWithIdentifier:(NSString *)identifier;
{
  NSView *pane = nil;
  if ([identifier isEqualToString:kTbWorker]) {
    pane = [self buildWorkerPaneView];
  }
  if (!pane) return;

  NSWindow *win = [self window];
  NSRect contentRect    = [pane frame];
  NSRect targetFrame    = [win frameRectForContentRect:contentRect];
  NSRect currentFrame   = [win frame];
  /* Keep the toolbar pinned where it is by anchoring the top-left corner —
   * the window grows/shrinks downward only when content height changes. */
  NSPoint topLeft       = NSMakePoint(currentFrame.origin.x,
                                      NSMaxY(currentFrame));
  targetFrame.origin.x  = topLeft.x;
  targetFrame.origin.y  = topLeft.y - targetFrame.size.height;

  [win setContentView:pane];
  [win setFrame:targetFrame display:YES animate:[win isVisible]];
}

- (void)switchPaneAction:(id)sender;
{
  if (![sender isKindOfClass:[NSToolbarItem class]]) return;
  NSString *ident = [(NSToolbarItem *)sender itemIdentifier];
  if (!ident) return;
  [[[self window] toolbar] setSelectedItemIdentifier:ident];
  [self selectPaneWithIdentifier:ident];
}

#pragma mark - NSToolbar Delegate

- (NSArray *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar;
{
  (void)toolbar;
  return [NSArray arrayWithObjects:kTbWorker, nil];
}

- (NSArray *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar;
{
  return [self toolbarAllowedItemIdentifiers:toolbar];
}

- (NSArray *)toolbarSelectableItemIdentifiers:(NSToolbar *)toolbar;
{
  return [self toolbarAllowedItemIdentifiers:toolbar];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
     itemForItemIdentifier:(NSString *)identifier
 willBeInsertedIntoToolbar:(BOOL)flag;
{
  (void)toolbar; (void)flag;
  if ([identifier isEqualToString:kTbWorker]) {
    return [self makeToolbarItemWithIdentifier:identifier
                                          icon:AIFACloud
                                         label:NSLocalizedString(@"Worker", nil)];
  }
  return nil;
}

- (NSToolbarItem *)makeToolbarItemWithIdentifier:(NSString *)ident
                                            icon:(AIFontAwesomeIcon)icon
                                           label:(NSString *)label;
{
  if (!ident || !label) return nil;
  NSToolbarItem *item = [[[NSToolbarItem alloc]
    initWithItemIdentifier:ident] autorelease];
  [item setLabel:label];
  [item setPaletteLabel:label];
  [item setImage:[AIFontAwesome imageForIcon:icon
                                     style:AIFontAwesomeStyleSolid
                                  iconSize:kIconPt
                                canvasSize:kIconCanvas
                                     scale:[[self window] XP_backingScaleFactor]]];
  [item setTarget:self];
  [item setAction:@selector(switchPaneAction:)];
  return item;
}

#pragma mark - Pane content helpers

- (NSTextField *)labelWithFrame:(NSRect)f text:(NSString *)t bold:(BOOL)bold;
{
  NSTextField *lbl = [[[NSTextField alloc] initWithFrame:f] autorelease];
  [lbl setStringValue:t];
  [lbl setBezeled:NO];
  [lbl setDrawsBackground:NO];
  [lbl setEditable:NO];
  [lbl setSelectable:NO];
  [lbl setFont:bold ? [NSFont boldSystemFontOfSize:13.0]
                    : [NSFont systemFontOfSize:13.0]];
  return lbl;
}

- (void)refreshEnvHint;
{
  const char *eu = getenv("ENIL_WORKER_URL");
  const char *es = getenv("ENIL_WORKER_SECRET");
  if ((eu && *eu) || (es && *es)) {
    [envHint_ setStringValue:NSLocalizedString(
      @"Environment variables override these settings while set.", nil)];
  } else {
    [envHint_ setStringValue:@""];
  }
}

#pragma mark - Actions

- (void)saveAction:(id)sender;
{
  (void)sender;
  NSString *url    = [urlField_ stringValue];
  NSString *secret = [secretField_ stringValue];
  if (![url length] || ![secret length]) {
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:NSLocalizedString(@"Both fields are required", nil)];
    [a setInformativeText:NSLocalizedString(
      @"Worker URL and Shared Secret must both be set.", nil)];
    [a runModal];
    return;
  }
  if (![[ENILKeychain sharedKeychain] saveWorkerURL:url secret:secret]) {
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:NSLocalizedString(@"Could not save credentials", nil)];
    [a setInformativeText:NSLocalizedString(
      @"The keychain refused the write. See Console.app for details.", nil)];
    [a runModal];
    return;
  }
  ENILLog(@"PreferencesWindowController.saveAction",
          @"saved worker credentials (url=%@)", url);
  [[self window] performClose:self];
}

- (void)cancelAction:(id)sender;
{
  (void)sender;
  [[self window] performClose:self];
}

- (void)dealloc;
{
  [urlField_ release];
  [secretField_ release];
  [envHint_ release];
  [super dealloc];
}

@end
