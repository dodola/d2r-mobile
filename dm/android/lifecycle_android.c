#include "../host/dm.h"
#include "services_fs.h"
#include "lifecycle_android.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

extern uint64_t dm_fs_commit_count(void);

/* Echoes currently has no reviewed game-private HUD offsets. Do not
 * manufacture an in-game result or poke an arbitrary address: a title
 * adapter must bind these callbacks after validating its exact build. The
 * generic part still observes real transactional Commit events and durably
 * syncs the committed tree for lifecycle callers. */
static pthread_mutex_t adapter_lock = PTHREAD_MUTEX_INITIALIZER;
static dm_android_save_adapter adapter;
static char adapter_title[64];
static _Atomic int missing_logged;
static pthread_mutex_t save_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t save_control_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t save_wait_epoch = 1;

int dm_android_save_bind(const dm_android_save_adapter *value) {
    if (!value || !value->title || !*value->title || !value->is_in_game || !value->request_save)
        return EINVAL;
    if (!!value->begin_save_wait != !!value->end_save_wait) return EINVAL;
    if (strlen(value->title) >= sizeof adapter_title) return ENAMETOOLONG;
    pthread_mutex_lock(&adapter_lock);
    if (adapter.title) { pthread_mutex_unlock(&adapter_lock); return EALREADY; }
    adapter = *value;
    strcpy(adapter_title, value->title);
    adapter.title = adapter_title;
    char title[sizeof adapter_title];
    strcpy(title, adapter_title);
    atomic_store(&missing_logged, 0);
    pthread_mutex_unlock(&adapter_lock);
    LOG("ANDROID_SAVE_ADAPTER_BOUND title=%s", title);
    return 0;
}

void dm_android_save_unbind(void) {
    atomic_fetch_add(&save_wait_epoch,1);
    pthread_mutex_lock(&save_control_lock);
    /* Keep removal and rollback atomic relative to the next begin/queue. */
    pthread_mutex_lock(&adapter_lock);
    dm_android_save_adapter current=adapter;
    pthread_mutex_unlock(&adapter_lock);
    if (current.end_save_wait) current.end_save_wait(current.opaque);
    pthread_mutex_lock(&adapter_lock);
    memset(&adapter, 0, sizeof adapter);
    adapter_title[0] = 0;
    pthread_mutex_unlock(&adapter_lock);
    pthread_mutex_unlock(&save_control_lock);
    LOG("ANDROID_SAVE_ADAPTER_UNBOUND");
}

int dm_android_save_adapter_available(void) {
    pthread_mutex_lock(&adapter_lock);
    int available = adapter.title != NULL;
    pthread_mutex_unlock(&adapter_lock);
    return available;
}

static int adapter_snapshot(dm_android_save_adapter *out, char *title, size_t title_size) {
    pthread_mutex_lock(&adapter_lock);
    *out = adapter;
    if (out->title && title && title_size) {
        strncpy(title, out->title, title_size - 1);
        title[title_size - 1] = 0;
        out->title = title;
    }
    pthread_mutex_unlock(&adapter_lock);
    return out->title != NULL;
}

static void log_missing_adapter(void) {
    if (!atomic_exchange(&missing_logged, 1))
        LOG("ANDROID_SAVE_ADAPTER_UNAVAILABLE title=echoes-of-wisdom reason=game-private save offsets are not registered; lifecycle will only sync the transaction tree");
}

static uint64_t save_wait_limit(double max_s) {
    if (!(max_s >= 0.0) || !isfinite(max_s)) max_s = 0.0;
    if (max_s > 30.0) max_s = 30.0; /* lifecycle callers must remain bounded */
    return (uint64_t)(max_s * 1000000000.0);
}

static int wait_for_commit(double max_s, uint64_t before,
                           uint64_t *commits, double *ms, uint64_t epoch) {
    if (commits) *commits = 0;
    if (ms) *ms = 0;
    uint64_t start = now_ns(), last = before, quiet_deadline = 0;
    uint64_t deadline = start + save_wait_limit(max_s);
    for (;;) {
        if (epoch && epoch != atomic_load(&save_wait_epoch)) break;
        uint64_t current = dm_fs_commit_count(), now = now_ns();
        if (current > before) {
            if (current != last) {
                last = current;
                quiet_deadline = now + 200000000ull;
            }
            if (quiet_deadline && now >= quiet_deadline) break;
        }
        if (now >= deadline) break;
        usleep(20000);
    }
    uint64_t observed = last > before ? last - before : 0;
    if (commits) *commits = observed;
    if (ms) *ms = (now_ns() - start) / 1000000.0;
    return (int)(observed > INT_MAX ? INT_MAX : observed);
}
int dm_android_save_wait_for_commit(double max_s, uint64_t before,
                                    uint64_t *commits, double *ms) {
    return wait_for_commit(max_s,before,commits,ms,0);
}

void dm_android_save_cancel(void) {
    atomic_fetch_add(&save_wait_epoch,1);
    pthread_mutex_lock(&save_control_lock);
    dm_android_save_adapter current;
    char title[sizeof adapter_title];
    if (adapter_snapshot(&current,title,sizeof title) && current.end_save_wait)
        current.end_save_wait(current.opaque);
    pthread_mutex_unlock(&save_control_lock);
}

int dm_ingame_now(void) {
    dm_android_save_adapter current;
    char title[sizeof adapter_title];
    if (!adapter_snapshot(&current, title, sizeof title)) { log_missing_adapter(); return 0; }
    int value = current.is_in_game(current.opaque);
    if (value < 0) {
        LOG("ANDROID_SAVE_ADAPTER_INGAME_ERROR title=%s result=%d", current.title, value);
        return 0;
    }
    return value != 0;
}

int dm_request_save(void) {
    dm_android_save_adapter current;
    char title[sizeof adapter_title];
    if (!adapter_snapshot(&current, title, sizeof title)) { log_missing_adapter(); return 0; }
    int value = current.request_save(current.opaque);
    LOG("ANDROID_SAVE_REQUEST title=%s result=%d", current.title, value);
    return value > 0;
}

uint64_t dm_android_save_epoch(void) { return atomic_load(&save_wait_epoch); }

int dm_android_save_wait_current(uint64_t epoch,double max_s,
                                 uint64_t *commits,int *froze,double *ms) {
    pthread_mutex_lock(&save_wait_lock);
    if (commits) *commits = 0;
    if (froze) *froze = 0;
    if (ms) *ms = 0;
    uint64_t start = now_ns(), before = dm_fs_commit_count();
    dm_android_save_adapter current;
    char title[sizeof adapter_title];
    pthread_mutex_lock(&save_control_lock);
    if (!epoch || epoch!=atomic_load(&save_wait_epoch)) {
        pthread_mutex_unlock(&save_control_lock);
        pthread_mutex_unlock(&save_wait_lock);
        return 0;
    }
    int bound=adapter_snapshot(&current,title,sizeof title), freeze=0;
    if (bound && current.begin_save_wait) freeze=current.begin_save_wait(current.opaque);
    int cancelled=epoch!=atomic_load(&save_wait_epoch);
    int requested=freeze>=0 && !cancelled ? dm_request_save() : 0;
    pthread_mutex_unlock(&save_control_lock);
    uint64_t observed = 0;
    double waited_ms = 0;
    if (!cancelled) wait_for_commit(max_s, before, &observed, &waited_ms, epoch);
    pthread_mutex_lock(&save_control_lock);
    cancelled=epoch!=atomic_load(&save_wait_epoch);
    if (bound && current.end_save_wait) current.end_save_wait(current.opaque);
    pthread_mutex_unlock(&save_control_lock);
    if (cancelled) requested=0;
    if (froze) *froze=freeze>0;
    if (commits) *commits = observed;
    /* Use the stable configured root rather than retaining a pointer into the
     * mount table: an Activity callback can race a title unmount while the
     * game is leaving a run. */
    const char *commit_dir = getenv("DM_SAVE");
    if (observed && commit_dir && dm_android_fs_sync_tree(commit_dir))
        LOG("ANDROID_SAVE_SYNC_FAILED path=%s errno=%d", commit_dir, errno);
    if (ms) *ms = (now_ns() - start) / 1000000.0;
    LOG("ANDROID_SAVE_WAIT requested=%d commits=%llu elapsed_ms=%.0f adapter=%s", requested,
        (unsigned long long)observed, ms ? *ms : 0.0, dm_android_save_adapter_available() ? "bound" : "absent");
    pthread_mutex_unlock(&save_wait_lock);
    return requested;
}
int dm_save_wait(double max_s,uint64_t *commits,int *froze,double *ms) {
    /* Capture before serialization: a queued waiter cannot inherit a newer
     * generation merely because another save was ahead of it. */
    return dm_android_save_wait_current(dm_android_save_epoch(),max_s,commits,froze,ms);
}
