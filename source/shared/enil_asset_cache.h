#ifndef ENIL_ASSET_CACHE_H
#define ENIL_ASSET_CACHE_H
#include <sqlite3.h>

/* Validate an account-relative sticker/sticon PNG and return its dimensions. */
int enil_asset_cache_png(const char *account_dir, const char *relative,
                         int *width, int *height);
/* Clear stale sticker/sticon paths so the normal download queue repairs them.
 * Performs filesystem work outside write transactions. Returns 0, or -1 on a
 * database error (the caller must not report successful reconciliation). */
int enil_asset_cache_reconcile(sqlite3 *db, const char *account_dir);
#endif
