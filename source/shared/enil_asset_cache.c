#include "enil_asset_cache.h"
#include "enil_png.h"
#include "enil_cocoa_log.h"
#include <stdio.h>
#include <string.h>

int enil_asset_cache_png(const char *dir, const char *relative, int *w, int *h) {
  char path[2048];
  int n;
  if (w) *w = 0;
  if (h) *h = 0;
  if (!dir || !relative || strncmp(relative, "purchases/", 10) ||
      strstr(relative, "../")) return 0;
  n = snprintf(path, sizeof(path), "%s/%s", dir, relative);
  return n > 0 && (size_t)n < sizeof(path) && enil_png_info(path, w, h);
}

int enil_asset_cache_reconcile(sqlite3 *db, const char *dir) {
  const char *tables[] = {"stickers_v2", "sticons_v2"};
  int table, repaired = 0;
  for (table = 0; table < 2; table++) {
    sqlite3_stmt *rows = NULL, *reset = NULL;
    char sql[512];
    int rc;
    /* Rowid order stays stable while only the path/dimension fields change. */
    snprintf(sql, sizeof(sql), "SELECT rowid,image_path,thumb_path,image_width,"
             "image_height,thumb_width,thumb_height FROM %s ORDER BY rowid", tables[table]);
    if (sqlite3_prepare_v2(db, sql, -1, &rows, NULL) != SQLITE_OK) goto fail;
    snprintf(sql, sizeof(sql), "UPDATE %s SET image_path=NULL,thumb_path=NULL,"
             "image_width=0,image_height=0,thumb_width=0,thumb_height=0 WHERE rowid=?",
             tables[table]);
    if (sqlite3_prepare_v2(db, sql, -1, &reset, NULL) != SQLITE_OK) goto fail;
    while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
      const char *image = (const char *)sqlite3_column_text(rows, 1);
      const char *thumb = (const char *)sqlite3_column_text(rows, 2);
      int iw = 0, ih = 0, tw = 0, th = 0, valid;
      if (!image && !thumb) continue; /* Already pending. */
      valid = enil_asset_cache_png(dir, image, &iw, &ih);
      if (valid && thumb && !strcmp(image, thumb)) { tw = iw; th = ih; }
      else valid = enil_asset_cache_png(dir, thumb, &tw, &th) && valid;
      if (valid && iw == sqlite3_column_int(rows, 3) && ih == sqlite3_column_int(rows, 4) &&
          tw == sqlite3_column_int(rows, 5) && th == sqlite3_column_int(rows, 6)) continue;
      sqlite3_bind_int64(reset, 1, sqlite3_column_int64(rows, 0));
      if (sqlite3_step(reset) != SQLITE_DONE) goto fail;
      sqlite3_reset(reset);
      repaired++;
    }
    if (rc != SQLITE_DONE) goto fail;
    sqlite3_finalize(rows);
    sqlite3_finalize(reset);
    continue;
fail:
    ENIL_LOG("Sync.cache", "reconciliation failed: %s", sqlite3_errmsg(db));
    sqlite3_finalize(rows);
    sqlite3_finalize(reset);
    return -1;
  }
  ENIL_LOG("Sync.cache", "%d invalid sticker/sticon entries queued for repair", repaired);
  return 0;
}
