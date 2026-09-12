#ifndef ENIL_COCOA_IMAGE_H
#define ENIL_COCOA_IMAGE_H

#include <stddef.h>

/* Image transcoding via ImageIO. Implemented in enil_cocoa.m — the single
 * Apple-frameworks translation unit in source/shared; this header stays pure
 * C so portable callers never pull in a framework. */

typedef struct {
  int orig_width;
  int orig_height;
  int thumb_width;
  int thumb_height;
} ENILThumbInfo;

/* Generate a JPEG thumbnail of src_path at thumb_path (bounded by an internal
   thumbnail size constant on the longest edge). Fills info with original and
   thumbnail pixel dimensions. Returns 0 on success, -1 on failure. */
int enil_thumb_generate(const char *src_path, const char *thumb_path,
                        ENILThumbInfo *info);

/* Transcode any source image (JPEG, PNG, GIF, …) to a JPEG at dst_path.
 *   max_px > 0 → longest edge bounded by max_px (CGImageSource thumbnail).
 *   max_px = 0 → full-resolution copy via CGImageSourceCreateImageAtIndex.
 * `info` is filled with both the original dimensions and the dimensions of
 * the written JPEG. Returns 0 on success, -1 on failure. */
int enil_image_jpeg_create(const char *src_path, const char *dst_path,
                           int max_px, ENILThumbInfo *info);

#endif /* ENIL_COCOA_IMAGE_H */
