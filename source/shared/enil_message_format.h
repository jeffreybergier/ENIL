#ifndef ENIL_MESSAGE_FORMAT_H
#define ENIL_MESSAGE_FORMAT_H

#include <sqlite3.h>
#include <stddef.h>

/* Return malloc-owned HTML; caller frees with enil_message_format_free.
 * Synchronous: caller owns database thread affinity. Stored LINE ranges are
 * UTF-16; valid legacy byte ranges remain readable. Invalid ranges render
 * literal text. Preview alt_texts must match the ordered outgoing resources. */
char *enil_message_text_html(sqlite3 *db, const char *message_id,
                             const char *text);
char *enil_message_text_html_for_sticons(sqlite3 *db, const char *text,
                                         int count,
                                         const char * const *package_ids,
                                         const char * const *sticon_ids,
                                         const char * const *alt_texts);
int enil_message_sticker_image_info(sqlite3 *db, const char *sticker_id,
                                    const char *package_id,
                                    char *path, size_t path_size,
                                    int *width, int *height);
void enil_message_format_free(char *p);

#endif
