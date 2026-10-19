#pragma once

#include <stddef.h>
#include <stdint.h>

/* Android save-cloud adapter.
 *
 * The current transport accepts an explicit filesystem directory. A mounted
 * sync directory owned by the application, or a test fixture, supplies an
 * absolute directory through DM_ANDROID_CLOUD_DIR.  The portable
 * dm_cloudsync.c policy is reused: one pack per character/stash,
 * atomic local replacement, backups and deletion markers.  Providers may
 * call dm_android_cloud_set_directory() after they have mounted their tree;
 * the call is intentionally separate from the lifecycle so a provider can
 * be swapped before the game starts.
 */
int dm_android_cloud_set_directory(const char *directory);
/* SAF/provider callbacks are synchronous on the serial cloud worker, or the
 * unmounted-save startup thread. Success means complete provider acceptance
 * and readback; it does not assert server-side replication. refresh must
 * finish the complete listing into the supplied existing EMPTY cache. */
typedef struct {
    int (*refresh)(const char *cache_dir,uint64_t attempt,void *opaque);
    int (*publish)(const char *cache_dir,uint64_t attempt,void *opaque);
    void (*cancel)(uint64_t attempt,void *opaque); /* optional, must not block */
} dm_android_cloud_provider_ops;
/* provider_id is exactly 64 lowercase SHA-256 hex digits, independent of
 * cache_root. Configure only before boot. All NULL explicitly disables cloud,
 * including DM_ANDROID_CLOUD_DIR fallback, while retaining durable queues. */
int dm_android_cloud_set_provider(const char *provider_id,const char *cache_root,
                                  const dm_android_cloud_provider_ops *ops,void *opaque);
typedef struct {
    int configured,enabled,busy,pending,active,last_error;
    uint64_t attempt,successful_attempts;
} dm_android_cloud_status;
/* Copy only; returns EBUSY without changing out if the queue mutex is held. */
int dm_android_cloud_status_get(dm_android_cloud_status *out);
/* Configured transport AND the applied Custom.icloud setting. */
int dm_android_cloud_enabled(void);
int dm_android_cloud_boot(const char *save_directory);
void dm_android_cloud_flush(int background);
int dm_android_cloud_flush_wait(double seconds);
/* Clear the one-shot background flush gate when the app becomes active again.
 * Without this, a flush requested while no snapshot was pending would leave
 * the gate armed and make the next in-game commit upload immediately, which
 * violates the iOS policy of deferring in-game snapshots until leaving the
 * run. */
void dm_android_cloud_foreground(void);
void dm_android_cloud_shutdown(void);

/* Applied Custom changes and leaving a run use the same upload policy as iOS. */
void dm_android_cloud_message(const char *category, const char *action);
