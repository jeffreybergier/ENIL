#ifndef ENIL_JPEG_H
#define ENIL_JPEG_H

/* Check JPEG marker/segment completeness through EOI using bounded memory.
 * This does not decode pixels; callers must also validate with ImageIO. */
int enil_jpeg_complete(const char *path);

#endif
