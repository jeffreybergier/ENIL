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
  self.view.backgroundColor = [UIColor blackColor];
  self.navigationItem.title = [self.imagePath lastPathComponent];
  [self buildScrollView];
  [self loadImage];
}

- (void)buildScrollView
{
  UIScrollView *sv = [[UIScrollView alloc] initWithFrame:self.view.bounds];
  sv.autoresizingMask =
    UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  sv.delegate = self;
  sv.minimumZoomScale = 1.0;
  sv.maximumZoomScale = 4.0;
  sv.backgroundColor = [UIColor blackColor];
  [self.view addSubview:sv];
  self.scrollView = sv;
}

- (void)loadImage
{
  UIImage *image = [UIImage imageWithContentsOfFile:self.imagePath];
  if (image == nil) {
    [self showFailureLabel];
    return;
  }
  UIImageView *iv = [[UIImageView alloc] initWithImage:image];
  iv.contentMode = UIViewContentModeScaleAspectFit;
  iv.frame = self.scrollView.bounds;
  iv.autoresizingMask =
    UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.scrollView addSubview:iv];
  self.imageView = iv;
}

- (void)showFailureLabel
{
  ENILLog(@"MediaViewerViewController.loadImage",
          @"could not load %@", self.imagePath);
  UILabel *label = [[UILabel alloc] initWithFrame:self.view.bounds];
  label.autoresizingMask =
    UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  label.numberOfLines = 0;
  label.textColor = [UIColor whiteColor];
  label.backgroundColor = [UIColor blackColor];
  label.text = NSLocalizedString(@"Image unavailable.", nil);
  [self.view addSubview:label];
}

#pragma mark - UIScrollViewDelegate

- (UIView *)viewForZoomingInScrollView:(UIScrollView *)scrollView
{
  (void)scrollView;
  return self.imageView;
}

@end
