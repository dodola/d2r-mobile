/* Run the Android filesystem probe against Linux temporary files. This covers
 * the real HLE save transaction and renameat2 recovery without a guest/device. */
#include "../../host/dm.h"
#include <time.h>
struct dm_flags g_flag;
uint64_t g_t0_ns;
uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void dm_prof_add(int id, uint64_t ns, uint64_t b) { (void)id; (void)ns; (void)b; }
/* These fixtures allocate writable memory, so no guest write-protection hook
 * is needed. That tracker is exercised separately by t_dirty_race. */
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
/* Preserve the original game language in this filesystem-only fixture. */
int dm_custom_lang_explicit(void) { return -1; }
int dm_sys_lang(void) { return 8; }
int dm_android_fs_verify(const char *, const char *);
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    return dm_android_fs_verify("/tmp", argv[1]);
}
