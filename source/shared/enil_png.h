#ifndef ENIL_PNG_H
#define ENIL_PNG_H

/* Validate PNG signature, dimensions, chunk CRCs and complete IDAT/IEND data.
 * Works for static PNG and APNG without depending on platform image APIs.
 * Returns 1 on success; clears dimensions on failure. */
int enil_png_info(const char *path, int *width, int *height);

#endif
