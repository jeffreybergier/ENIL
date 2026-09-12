#ifndef ENIL_MESSAGE_FORMAT_H
#define ENIL_MESSAGE_FORMAT_H

#include <sqlite3.h>
#include <stddef.h>

char *enil_message_text_html(sqlite3 *db, const char *message_id,
                             const char *text);
char *enil_message_text_html_for_sticons(sqlite3 *db, const char *text,
                                         int count,
                                         const char * const *package_ids,
                                         const char * const *sticon_ids);
int enil_message_sticker_image_info(sqlite3 *db, const char *sticker_id,
                                    const char *package_id,
                                    char *path, size_t path_size,
                                    int *width, int *height);
void enil_message_format_free(char *p);

#endif
