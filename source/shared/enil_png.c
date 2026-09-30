#include "enil_png.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <zlib.h>

static uint32_t be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int enil_png_info(const char *path, int *width, int *height) {
  static const unsigned char signature[8] = {137,80,78,71,13,10,26,10};
  unsigned char header[8], buf[8192], checksum[4];
  FILE *f;
  int w = 0, h = 0, seen_header = 0, seen_data = 0, ok = 0;
  if (width) *width = 0;
  if (height) *height = 0;
  if (!path || !(f = fopen(path, "rb"))) return 0;
  if (fread(header, 1, 8, f) != 8 || memcmp(header, signature, 8)) goto done;
  while (fread(header, 1, 8, f) == 8) {
    uint32_t length = be32(header), remaining = length;
    uLong crc = crc32(0L, header + 4, 4);
    int ihdr = !memcmp(header + 4, "IHDR", 4);
    int idat = !memcmp(header + 4, "IDAT", 4);
    int iend = !memcmp(header + 4, "IEND", 4);
    if (length > INT_MAX || (!seen_header && !ihdr)) goto done;
    if (ihdr && (seen_header || length != 13)) goto done;
    if (iend && (length || !seen_data)) goto done;
    while (remaining) {
      size_t n = remaining < sizeof(buf) ? remaining : sizeof(buf);
      if (fread(buf, 1, n, f) != n) goto done;
      crc = crc32(crc, buf, (uInt)n);
      if (ihdr) {
        uint32_t x = be32(buf), y = be32(buf + 4);
        int depth = buf[8], color = buf[9];
        int valid_depth =
          (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
          (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
          ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16));
        if (!x || !y || x > INT_MAX || y > INT_MAX || !valid_depth ||
            buf[10] || buf[11] || buf[12] > 1) goto done;
        w = (int)x; h = (int)y;
      }
      remaining -= (uint32_t)n;
    }
    if (fread(checksum, 1, 4, f) != 4 || be32(checksum) != (uint32_t)crc) goto done;
    if (ihdr) seen_header = 1;
    if (idat && length) seen_data = 1;
    if (iend) { ok = fgetc(f) == EOF && !ferror(f); break; }
  }
done:
  fclose(f);
  if (ok) {
    if (width) *width = w;
    if (height) *height = h;
  }
  return ok;
}
