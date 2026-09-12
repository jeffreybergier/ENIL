#import "StatusViewController.h"
#import "ENILBottomToolbarView.h"
#import "SyncMiniViewController.h"

@implementation StatusViewController

- (id)initWithAccount:(id)account;
{
  if ((self = [super init])) {
    account_ = account;  /* unsafe_unretained; only forwarded to the mini bar */
  }
  return self;
}

/* Lifecycle split: -loadView creates only the root bar so [self view] is
 * valid before any child wiring. -viewDidLoad adopts the child and inserts
 * its view. No -viewDidLayout: the SyncMini view fills barView_'s bounds
 * exactly with WidthSizable|HeightSizable autoresize. */

- (void)loadView;
{
  ENILBottomToolbarView *bar = [[ENILBottomToolbarView alloc]
      initWithFrame:NSMakeRect(0, 0, 240.0, 32.0)];
  barView_ = bar;
  [barView_ setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
  /* Hand the bar to the base; barView_ stays as a non-owning typed alias. */
  [self setView:barView_];
  [barView_ release];
}

- (void)viewDidLoad;
{
  [super viewDidLoad];
  syncMiniController_ = [[SyncMiniViewController alloc] initWithAccount:account_];
  /* AI_addChildViewController: wires the responder chain on every tier —
   * see MessageListViewController for the rationale. */
  [self AI_addChildViewController:syncMiniController_];
  NSView *miniView = [syncMiniController_ view];
  [miniView setFrame:[barView_ bounds]];
  [miniView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [barView_ addSubview:miniView];
}

- (void)dealloc;
{
  /* barView_ is a non-owning alias — the base releases view_. */
  [syncMiniController_ release];
  [super dealloc];
}

@end
