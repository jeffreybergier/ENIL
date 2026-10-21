/* Offline driver: opens only the supplied local database, read-only. */
#include "enil_message_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void enil_log(const char *tag, const char *fmt, ...) {
  (void)tag; (void)fmt;
}
int main(int argc, char **argv) {
  sqlite3 *db;
  sqlite3_stmt *stmt;
  int rc;
  if (argc != 3 || sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, NULL))
    return 1;
  rc = sqlite3_prepare_v2(db,
    "SELECT id,text FROM messages_v2 WHERE id=? OR ?='*' ORDER BY id",
    -1, &stmt, NULL);
  if (rc != SQLITE_OK) return 2;
  sqlite3_bind_text(stmt, 1, argv[2], -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, argv[2], -1, SQLITE_STATIC);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    const char *text = (const char *)sqlite3_column_text(stmt, 1);
    char *html;
    if (!strcmp(argv[2], "preview")) {
      const char *pkg[] = {"pkg"}, *sid[] = {"1"}, *alt[] = {"shy"};
      html = enil_message_text_html_for_sticons(db, text, 1, pkg, sid, alt);
    } else html = enil_message_text_html(db, id, text);
    if (!html) return 3;
    printf("%s%c%s%c", id, 0, html, 0);
    free(html);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return rc == SQLITE_DONE ? 0 : 4;
}
