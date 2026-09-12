/* ============================================================================
 * Sticky failure gate. See enil_health.h for the contract.
 *
 * WORKER failure is process-global (one shared worker for the app). LINE
 * failure is per-account: it lives in an enil_health_t and the LINE-source
 * calls act on whichever health the calling thread has bound (none = inert).
 * ==========================================================================*/

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "enil_health.h"
#include "enil_cocoa_progress.h"
#include "enil_cocoa_log.h"

struct enil_health {
  int  line_failed;
  char account_id[64]; /* logging only */
};

/* WORKER is shared infrastructure, so its gate is a single process-global
 * flag. A plain int is fine: reads/writes are atomic on every target arch and
 * the worst case of a stale read is one extra doomed call before the gate
 * kicks in — well inside the safety envelope. */
static int s_worker_failed = 0;

/* Thread-local binding of the calling thread's current account health. Set by
 * enil_health_bind() at each per-account background-thread entry; read by the
 * LINE-source calls. NULL on any thread that never bound (e.g. QR login). */
static pthread_key_t  s_current_key;
static pthread_once_t s_current_once = PTHREAD_ONCE_INIT;

static void current_key_make(void) { pthread_key_create(&s_current_key, NULL); }

static enil_health_t *current_health(void) {
  pthread_once(&s_current_once, current_key_make);
  return (enil_health_t *)pthread_getspecific(s_current_key);
}

void enil_health_bind(enil_health_t *h) {
  pthread_once(&s_current_once, current_key_make);
  pthread_setspecific(s_current_key, h);
}

enil_health_t *enil_health_create(const char *account_id) {
  enil_health_t *h = (enil_health_t *)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (account_id) {
    strncpy(h->account_id, account_id, sizeof(h->account_id) - 1);
    h->account_id[sizeof(h->account_id) - 1] = '\0';
  }
  return h;
}

void enil_health_destroy(enil_health_t *h) {
  if (!h) return;
  if (current_health() == h) enil_health_bind(NULL);
  free(h);
}

/* ---- worker (global) ----------------------------------------------------- */

static void worker_set_failure(const char *user_message) {
  s_worker_failed = 1;
  ENIL_LOG("Health.set_failure", "[error.worker] Worker Error - %s",
           (user_message && *user_message) ? user_message : "unknown error");
  enil_status_post_error("error.worker", "Worker Error");
}

static void worker_clear_failure(void) {
  int was = s_worker_failed;
  s_worker_failed = 0;
  if (was) {
    ENIL_LOG("Health.clear_failure", "[error.worker] cleared");
    enil_status_post("error.worker", "Reconnecting", 1);
  }
}

/* ---- line (per bound account) -------------------------------------------- */

static void line_set_failure(const char *user_message) {
  enil_health_t *h = current_health();
  if (!h) return; /* unbound thread (e.g. QR login): inert */
  h->line_failed = 1;
  ENIL_LOG("Health.set_failure", "[error.line] LINE Error (%s) - %s",
           h->account_id[0] ? h->account_id : "?",
           (user_message && *user_message) ? user_message : "unknown error");
  enil_status_post_error("error.line", "LINE Error");
}

static void line_clear_failure(void) {
  enil_health_t *h = current_health();
  int was;
  if (!h) return;
  was = h->line_failed;
  h->line_failed = 0;
  if (was) {
    ENIL_LOG("Health.clear_failure", "[error.line] (%s) cleared",
             h->account_id[0] ? h->account_id : "?");
    enil_status_post("error.line", "Reconnecting", 1);
  }
}

/* ---- public source-keyed API --------------------------------------------- */

void enil_health_set_failure(enil_err_source_t src, const char *user_message) {
  if (src == ENIL_ERR_WORKER) worker_set_failure(user_message);
  else if (src == ENIL_ERR_LINE) line_set_failure(user_message);
}

void enil_health_clear_failure(enil_err_source_t src) {
  if (src == ENIL_ERR_WORKER) worker_clear_failure();
  else if (src == ENIL_ERR_LINE) line_clear_failure();
}

int enil_health_is_failed(enil_err_source_t src) {
  if (src == ENIL_ERR_WORKER) return s_worker_failed;
  if (src == ENIL_ERR_LINE) {
    enil_health_t *h = current_health();
    return h ? h->line_failed : 0;
  }
  return 0;
}

int enil_health_any_failed(void) {
  enil_health_t *h = current_health();
  return s_worker_failed || (h && h->line_failed);
}
