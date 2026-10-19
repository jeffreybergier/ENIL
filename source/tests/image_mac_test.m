/* Run only with disposable fixture/output paths. No account or network access.
 * Usage: image-test thumbnail-output valid-image... --reject bad-image... */
#import <Foundation/Foundation.h>
#include "enil_cocoa_image.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  int i, reject = 0, failures = 0, checked = 0;
  if (argc < 3) return 2;
  for (i = 2; i < argc; i++) {
    int w = 0, h = 0, rc;
    ENILThumbInfo info;
    if (!strcmp(argv[i], "--reject")) { reject = 1; continue; }
    rc = enil_image_validate(argv[i], &w, &h);
    if ((reject && rc == 0) || (!reject && (rc != 0 || w <= 0 || h <= 0))) {
      fprintf(stderr, "FAIL validation: %s (%d, %dx%d)\n", argv[i], rc, w, h);
      failures++;
    }
    if (!reject && (enil_thumb_generate(argv[i], argv[1], &info) != 0 ||
        enil_image_validate(argv[1], &w, &h) != 0 || w > 600 || h > 600)) {
      fprintf(stderr, "FAIL thumbnail: %s\n", argv[i]);
      failures++;
    }
    if (reject && enil_thumb_generate(argv[i], argv[1], &info) == 0) {
      fprintf(stderr, "FAIL accepted truncated source: %s\n", argv[i]);
      failures++;
    }
    checked++;
  }
  printf("%s: %d image fixtures, %d failures\n",
         failures ? "FAIL" : "PASS", checked, failures);
  [pool drain];
  return failures ? 1 : 0;
}
