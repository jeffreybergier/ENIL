#ifndef ENIL_COCOA_COLLATE_H
#define ENIL_COCOA_COLLATE_H

/* Locale-aware sqlite collation via CFStringCompare. Implemented in
 * enil_cocoa.m; pure-C header so portable callers stay framework-free. */

/* sqlite collation callback: locale-aware, case-insensitive, numeric-aware
 * UTF-8 string comparison (the C-callable equivalent of NSString's
 * -localizedCaseInsensitiveCompare:). The signature deliberately matches
 * sqlite3's xCompare so it can be passed straight to sqlite3_create_collation;
 * only plain C types cross this boundary. Inputs are byte buffers + lengths
 * and are NOT NUL-terminated. Returns <0, 0, or >0 like strcmp. */
int enil_localized_collate(void *ctx, int len_a, const void *a,
                           int len_b, const void *b);

#endif /* ENIL_COCOA_COLLATE_H */
