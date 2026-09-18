#ifndef ENIL_LOGIN_STORE_H
#define ENIL_LOGIN_STORE_H

/* Local login lifecycle. No network requests. All directories are siblings
 * under the account root; returned directory strings are owned by the caller. */
int enil_login_store_prepare(const char *staging_dir, const char *profile,
                              const char *expected_mid);
char *enil_login_store_select(const char *staging_dir);
int enil_login_store_stage(const char *staging_dir, const char *pending_dir);
int enil_login_store_can_restart(const char *staging_dir);
/* Explicit user action: retain the old attempt but exclude it from recovery. */
int enil_login_store_restart(const char *staging_dir);
/* Atomically install a staged session. The account engine must be stopped. */
int enil_login_store_activate(const char *staging_dir, const char *account_dir);

#endif
