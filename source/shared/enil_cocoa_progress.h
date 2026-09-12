#ifndef ENIL_COCOA_PROGRESS_H
#define ENIL_COCOA_PROGRESS_H

/* Sync status / progress bridge. Implemented in enil_cocoa.m; fans sync state
 * out as ENILSyncStatusNotification on the main thread. Pure-C header so the
 * portable sync engine can post progress without importing a framework. */

/*
 * Posts ENILSyncStatusNotification on the main thread.
 * When step == total and error == 0, also posts ENILSyncDidFinishNotification(ok=YES).
 * When error != 0, also posts ENILSyncDidFinishNotification(ok=NO).
 *
 * key (nullable): opaque identifier — follow-up posts with the same key
 *   coalesce in the SyncMiniViewController queue (update-in-place instead
 *   of enqueue). NULL means "anonymous one-shot event".
 * indeterminate: 1 = no meaningful progress percentage (barber-pole spinner);
 *   0 = unit_count/unit_total drive a determinate progress bar.
 *
 * Safe to call from any thread.
 */
void enil_progress_post(int step, int total, int unit_count, int unit_total,
                        const char *message, int error,
                        const char *key, int indeterminate);

/*
 * Posts ENILSyncStatusNotification on the main thread for a transient,
 * out-of-band event (e.g. SSE-driven activity). Does NOT post
 * ENILSyncDidFinishNotification — the sync engine alone owns the
 * begin/finish lifecycle. The SyncMiniViewController queue treats these
 * the same way it treats sync-engine posts (coalesces by key, 3s dwell).
 * Safe to call from any thread.
 */
void enil_status_post(const char *key, const char *message, int indeterminate);

/*
 * Same as enil_status_post but tags the userInfo with error=1. The
 * SyncMiniViewController renders error items in red and keeps them on
 * screen indefinitely (no 3s dwell-and-promote) until a follow-up post
 * with the same key and error=0 supersedes it. Use for sticky
 * configuration / auth failures the user must act on. Like
 * enil_status_post, this NEVER posts ENILSyncDidFinishNotification —
 * the sync engine alone owns the begin/finish lifecycle.
 * Safe to call from any thread.
 */
void enil_status_post_error(const char *key, const char *message);

#endif /* ENIL_COCOA_PROGRESS_H */
