//
//  MediaViewerViewController.m
//  ENIL
//

#import "MediaViewerViewController.h"
#import "XPFoundation.h"

@interface MediaViewerViewController () <UIScrollViewDelegate>
@property (nonatomic, copy)   NSString *imagePath;
@property (nonatomic, strong) UIScrollView *scrollView;
@property (nonatomic, strong) UIImageView *imageView;
@end

@implementation MediaViewerViewController

- (instancetype)initWithImagePath:(NSString *)imagePath
{
  if ([imagePath length] == 0) {
    @throw [NSException exceptionWithName:NSInvalidArgumentException
                                  reason:@"imagePath empty"
                                userInfo:nil];
  }
  if ((self = [super initWithNibName:nil bundle:nil])) {
    _imagePath = [imagePath copy];
  }
  return self;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  [[self view] setBackgroundColor:[UIColor blackColor]];
  [[self navigationItem] setTitle:[[self imagePath] lastPathComponent]];
  [self buildScrollView];
  [self loadImage];
}

- (void)buildScrollView
{
  UIScrollView *sv = [[UIScrollView alloc] initWithFrame:[[self view] bounds]];
  [sv setAutoresizingMask:UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight];
  [sv setDelegate:self];
  [sv setMinimumZoomScale:1.0];
  [sv setMaximumZoomScale:4.0];
  [sv setBackgroundColor:[UIColor blackColor]];
  [[self view] addSubview:sv];
  [self setScrollView:sv];
}

- (void)loadImage
{
  UIImage *image = [UIImage imageWithContentsOfFile:[self imagePath]];
  if (image == nil) {
    [self showFailureLabel];
    return;
  }
  UIImageView *iv = [[UIImageView alloc] initWithImage:image];
  [iv setContentMode:UIViewContentModeScaleAspectFit];
  [iv setFrame:[[self scrollView] bounds]];
  [iv setAutoresizingMask:UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight];
  [[self scrollView] addSubview:iv];
  [self setImageView:iv];
}

- (void)showFailureLabel
{
  ENILLog(@"MediaViewerViewController.loadImage",
          @"could not load %@", [self imagePath]);
  UILabel *label = [[UILabel alloc] initWithFrame:[[self view] bounds]];
  [label setAutoresizingMask:UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight];
  [label setNumberOfLines:0];
  [label setTextColor:[UIColor whiteColor]];
  [label setBackgroundColor:[UIColor blackColor]];
  [label setText:NSLocalizedString(@"Image unavailable.", nil)];
  [[self view] addSubview:label];
}

#pragma mark - UIScrollViewDelegate

- (UIView *)viewForZoomingInScrollView:(UIScrollView *)scrollView
{
  (void)scrollView;
  return [self imageView];
}

@end
