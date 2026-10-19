#include "enil_jpeg.h"
#include <stdio.h>

int enil_jpeg_complete(const char *path) {
  FILE *f;
  int marker, c, scan = 0, seen_scan = 0, ok = 0;
  if (!path || !(f = fopen(path, "rb"))) return 0;
  if (fgetc(f) != 0xff || fgetc(f) != 0xd8) goto done;
  for (;;) {
    if (scan) {
      do { c = fgetc(f); } while (c != EOF && c != 0xff);
      if (c == EOF) goto done;
    } else if (fgetc(f) != 0xff) goto done;
    do { marker = fgetc(f); } while (marker == 0xff);
    if (marker == EOF) goto done;
    if (scan && (marker == 0 || (marker >= 0xd0 && marker <= 0xd7)))
      continue; /* Stuffed FF or restart marker within entropy data. */
    scan = 0;
    if (marker == 0xd9) { ok = seen_scan; break; }
    if (marker == 0 || marker == 0xd8 ||
        (marker >= 0xd0 && marker <= 0xd7)) goto done;
    if (marker != 1) {
      int hi = fgetc(f), lo, remaining;
      if (hi == EOF) goto done;
      lo = fgetc(f);
      if (lo == EOF) goto done;
      remaining = ((hi << 8) | lo) - 2;
      if (remaining < 0) goto done;
      /* Read rather than seek: a seek past EOF would hide truncation. */
      while (remaining--) if (fgetc(f) == EOF) goto done;
    }
    if (marker == 0xda) { scan = 1; seen_scan = 1; }
  }
done:
  if (ferror(f)) ok = 0;
  fclose(f);
  return ok;
}
