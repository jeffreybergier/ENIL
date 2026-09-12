#import "AboutWindowController.h"
#import <AltivecCore/AltivecCore.h>

static const CGFloat kKVColWidthKey   = 152.0;
static const CGFloat kKVColWidthValue = 252.0;

#pragma mark - KeyValueTableView Implementation

@implementation KeyValueTableView

- (id)initWithFrame:(NSRect)frame;
{
  if ((self = [super initWithFrame:frame])) {
    [self setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    scrollView_ = [[NSScrollView alloc] initWithFrame:[self bounds]];
    [scrollView_ setHasVerticalScroller:YES];
    [scrollView_ setHasHorizontalScroller:YES];
    [scrollView_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [scrollView_ setBorderType:NSBezelBorder];

    tableView_ = [[NSTableView alloc]
      initWithFrame:[[scrollView_ contentView] bounds]];

    NSTableColumn *keyCol = [[[NSTableColumn alloc] initWithIdentifier:@"Key"] autorelease];
    [[keyCol headerCell] setStringValue:NSLocalizedString(@"Key", nil)];
    [keyCol setWidth:kKVColWidthKey];
    [tableView_ addTableColumn:keyCol];

    NSTableColumn *valCol = [[[NSTableColumn alloc] initWithIdentifier:@"Value"] autorelease];
    [[valCol headerCell] setStringValue:NSLocalizedString(@"Value", nil)];
    [valCol setWidth:kKVColWidthValue];
    [tableView_ addTableColumn:valCol];

    [tableView_ setDataSource:self];
    [tableView_ setDelegate:self];
    [tableView_ setUsesAlternatingRowBackgroundColors:YES];
    [tableView_ setAutoresizingMask:NSViewWidthSizable];
    [tableView_ setColumnAutoresizingStyle:NSTableViewLastColumnOnlyAutoresizingStyle];

    [scrollView_ setDocumentView:tableView_];
    [self addSubview:scrollView_];
  }
  return self;
}

- (void)setData:(NSDictionary *)data;
{
  [data_ autorelease];
  data_ = [data retain];
  [sortedKeys_ autorelease];
  sortedKeys_ = [[[data_ allKeys]
    sortedArrayUsingSelector:@selector(compare:)] retain];
  [tableView_ reloadData];
}

- (NSDictionary *)data { return data_; }

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView;
{
  (void)tableView;
  return (NSInteger)[sortedKeys_ count];
}

- (id)tableView:(NSTableView *)tableView
    objectValueForTableColumn:(NSTableColumn *)tableColumn
                          row:(NSInteger)row;
{
  (void)tableView;
  NSString *key = [sortedKeys_ objectAtIndex:(NSUInteger)row];
  if ([[tableColumn identifier] isEqualToString:@"Key"]) return key;
  return [data_ objectForKey:key];
}

- (void)dealloc;
{
  [scrollView_ release];
  [tableView_ release];
  [data_ release];
  [sortedKeys_ release];
  [super dealloc];
}

@end

#pragma mark - AboutWindowController Implementation

@implementation AboutWindowController

- (id)init { return [super initWithWindowNibName:@"ignored"]; }

- (void)loadWindow;
{
  XPWindowStyleMask mask = XPWindowStyleMaskTitled
                         | XPWindowStyleMaskClosable
                         | XPWindowStyleMaskMiniaturizable;
  NSWindow *window = [[[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 320)
                                                  styleMask:mask
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO] autorelease];
  [window setTitle:NSLocalizedString(@"About ENIL", nil)];
  [window setReleasedWhenClosed:NO];
  [window center];
  [self setWindow:window];
}

- (void)windowDidLoad;
{
  [super windowDidLoad];
  NSView *contentView = [[self window] contentView];
  NSRect frame = NSInsetRect([contentView bounds], 8, 8);

  KeyValueTableView *kv = [[[KeyValueTableView alloc] initWithFrame:frame] autorelease];
  NSDictionary *v = [NSDictionary dictionaryWithObjectsAndKeys:
    [NSString stringWithUTF8String:zlibVersion()], @"libz",
    [NSString stringWithUTF8String:OpenSSL_version(OPENSSL_VERSION)],
    @"libssl",
    [NSString stringWithUTF8String:curl_version()], @"libcurl",
    [NSString stringWithUTF8String:cJSON_Version()], @"cJSON",
    [NSString stringWithUTF8String:sqlite3_libversion()], @"sqlite3",
    NSLocalizedString(@"no version provided", nil), @"qrcodegen", nil];
  [kv setData:v];
  [contentView addSubview:kv];
}

@end
