#pragma once

#include <stdint.h>

/* Title-specific save hooks are deliberately opt-in. A title adapter owns
 * the guest address checks and must return 1 only after it has safely queued
 * the original game's save path. The common lifecycle waits on the
 * transactional Commit counter without guessing private HUD offsets. */
typedef struct {
    const char *title;
    int (*is_in_game)(void *opaque);
    int (*request_save)(void *opaque);
    void *opaque;
    /* Optional title pause handshake. Begin returns 1 when it froze the
     * world, 0 when unnecessary, or -1 without changing a paused title.
     * End restores its pause before thawing and is safe to call on cancel. */
    int (*begin_save_wait)(void *opaque);
    void (*end_save_wait)(void *opaque);
} dm_android_save_adapter;

int dm_android_save_bind(const dm_android_save_adapter *adapter);
void dm_android_save_unbind(void);
int dm_android_save_adapter_available(void);
/* Resume/destroy invalidates a pending wait and rolls back its title pause
 * handshake. A later wait cannot inherit the cancelled request. */
void dm_android_save_cancel(void);
/* Capture before queueing asynchronous lifecycle work. A cancelled generation
 * must never begin a new pause handshake after a foreground notification. */
uint64_t dm_android_save_epoch(void);
int dm_android_save_wait_current(uint64_t epoch,double max_s,
                                 uint64_t *commits,int *froze,double *ms);

/* Wait for a real nn::fs::Commit that occurs after `before`.  This is used by
 * title exit paths that can ask the original application to save through its
 * own applet/exit handling, without pretending that a private save offset is
 * available.  Returns the number of commits observed (possibly zero after a
 * bounded timeout). */
int dm_android_save_wait_for_commit(double max_s, uint64_t before,
                                    uint64_t *commits, double *ms);

int dm_ingame_now(void);
int dm_request_save(void);
int dm_save_wait(double max_s, uint64_t *commits, int *froze, double *ms);
