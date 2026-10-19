/* Linux host regression for the upstream TLS ownership merge. Real pthread
 * keys/destructors and production thread/slot code; a thread-local array models
 * the Bionic TCB, so this is not an Android TLS ABI or device test. */
#include "../../host/dm.h"
#include <assert.h>
#include <stdatomic.h>

static _Thread_local uintptr_t test_tcb[16];
static _Atomic unsigned allocations, releases;
static void *counted_calloc(size_t n, size_t size) {
    void *p = calloc(n, size); assert(p); atomic_fetch_add(&allocations, 1); return p;
}
static void counted_free(void *p) {
    if (p) atomic_fetch_add(&releases, 1);
    free(p);
}
#define calloc counted_calloc
#define free counted_free
#define __builtin_thread_pointer() test_tcb
#include "../thread_slots.c"
#include "../../host/thread.c"
#undef __builtin_thread_pointer
#undef free
#undef calloc

pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate, g_keyres[2], g_keyaspect, g_keyaspectui;
int android_get_device_api_level(void) { return 29; }
void dm_gate(void) {}
uint64_t dm_game_res(int i) { (void)i; return 0; }
uint64_t front_game_res(int i) { (void)i; return 0; }
static char guest_owned[64];
static void *one(void *arg) {
    guest_thread_init();
    assert(dm_android_thread_has_guest_context());
    /* The guest replaces logical TLS pointers; the allocation owner key keeps
     * the original pointers independently of the current logical slot values. */
    dm_android_slot_set(g_keytls, guest_owned);
    dm_android_slot_set(g_keytlr, guest_owned + 32);
    if ((uintptr_t)arg & 1) {
        dm_android_slots_detach();
        assert(!dm_android_thread_has_guest_context());
    }
    return NULL;
}
int main(void) {
    assert(!pthread_key_create(&g_key18, NULL));
    assert(!pthread_key_create(&g_keytls, NULL));
    assert(!pthread_key_create(&g_keytlr, NULL));
    assert(!pthread_key_create(&g_keygate, NULL));
    for (uintptr_t i = 0; i < 200; i++) {
        pthread_t t; assert(!pthread_create(&t, NULL, one, (void *)i));
        assert(!pthread_join(t, NULL));
    }
    assert(allocations == 800 && releases == 800);
    printf("ANDROID_THREAD_LIFETIME threads=200 explicit_detach=100 pthread_exit=100 allocations=800 releases=800 passed=1\n");
    return 0;
}
