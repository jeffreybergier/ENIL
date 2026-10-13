#import <AppKit/AppKit.h>
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#import "roster_test.h"
int main(int argc,char **argv) {
  NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];
  if(argc!=2) return 2;
  TestRosterRows([NSString stringWithUTF8String:argv[1]]);
  [pool drain]; return 0;
}
