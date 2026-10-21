/* Fault-around / protect-around semantics of the dirty tracker.
 *
 * The tracker may only ever over-approximate: every page that was written must read as dirty, a page outside the
 * claimed window must keep its exact state, and protecting more than requested must still catch every later write. */
#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-56s 期望 %-4lld → 真结果 %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

enum { WINDOW = 8, PAGES = 64 };

int main(void) {
    /* The window is expressed in pages of the host page size; pin it before the first range is registered. */
    setenv("DM_FAULT_AROUND", "8", 1);
    setenv("DM_PROTECT_AROUND", "8", 1);
    crash_install();
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *mem = mmap(NULL, (size_t)PAGES * (size_t)pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(mem, (size_t)PAGES * (size_t)pg);

    /* Protecting one page protects its whole aligned window (pages 16..23), and nothing else. */
    nvn_dirty_protect(mem + 20 * pg, (size_t)pg);
    uint64_t stamp = nvn_dirty_now();
    uint64_t f0, p0, f1, p1;
    nvn_dirty_stats(&f0, &p0);
    mem[16 * pg] = 1;                                   /* first page of the window: protected, so it faults */
    nvn_dirty_stats(&f1, &p1);
    EXPECT("保护一页 → 同窗口首页也受保护（写入触发缺页）", 1, f1 - f0);
    for (int i = 0; i < PAGES; i++) {
        char name[96];
        snprintf(name, sizeof name, "窗口内被写页及邻页标脏 / 窗口外保持干净：第 %d 页", i);
        int in_window = i >= 16 && i < 24;
        int dirty = nvn_dirty_since(mem + i * pg, (size_t)pg, stamp);
        if (i == 16) EXPECT(name, 1, dirty);                       /* the written page is always dirty */
        else if (!in_window) EXPECT(name, 0, dirty);               /* nothing outside the window is touched */
        else if (!dirty) { /* neighbours may be dirty or not, both are legal; they just must not be wrong below */ }
    }
    /* Fault-around released the window with one fault: later writes to its pages do not fault again. */
    nvn_dirty_stats(&f0, &p0);
    for (int i = 17; i < 24; i++) mem[i * pg] = 2;
    nvn_dirty_stats(&f1, &p1);
    EXPECT("同一窗口内其余页的写入不再触发缺页", 0, f1 - f0);
    for (int i = 17; i < 24; i++) {
        char name[96];
        snprintf(name, sizeof name, "窗口内逐页写入后每页都判脏：第 %d 页", i);
        EXPECT(name, 1, nvn_dirty_since(mem + i * pg, (size_t)pg, stamp));
    }
    /* The window next to it is still protected: a write there is detected by a real fault and marked dirty. */
    nvn_dirty_stats(&f0, &p0);
    nvn_dirty_protect(mem + 24 * pg, (size_t)pg);
    uint64_t stamp2 = nvn_dirty_now();
    mem[31 * pg] = 3;
    nvn_dirty_stats(&f1, &p1);
    EXPECT("相邻窗口的保护页仍由真实缺页检测", 1, f1 - f0);
    EXPECT("相邻窗口被写页判脏", 1, nvn_dirty_since(mem + 31 * pg, (size_t)pg, stamp2));
    EXPECT("更早的窗口不受影响", 0, nvn_dirty_since(mem + 16 * pg, (size_t)pg, stamp2));

    /* A write straight after the protect (no read in between) is never missed anywhere in the window. */
    for (int i = 40; i < 48; i++) {
        nvn_dirty_protect(mem + 40 * pg, (size_t)pg);
        uint64_t s = nvn_dirty_now();
        mem[i * pg] = 4;
        char name[96];
        snprintf(name, sizeof name, "保护后紧接的写入不漏：窗口内第 %d 页", i);
        EXPECT(name, 1, nvn_dirty_since(mem + i * pg, (size_t)pg, s));
    }
    /* Pages the GPU writes stay exact: fault-around neither claims them as neighbours nor expands when one of them
     * faults, so GPU-produced data is never re-uploaded from stale guest memory because a neighbour was written. */
    nvn_dirty_mark_gpu_written(mem + 34 * pg, (size_t)pg);
    nvn_dirty_protect(mem + 36 * pg, (size_t)pg);                  /* protects the whole window 32..39 */
    uint64_t s4 = nvn_dirty_now();
    nvn_dirty_stats(&f0, &p0);
    mem[32 * pg] = 6;
    nvn_dirty_stats(&f1, &p1);
    EXPECT("GPU 写过的页：窗口内前面的页触发一次缺页", 1, f1 - f0);
    EXPECT("GPU 写过的页没有被顺带标脏", 0, nvn_dirty_since(mem + 34 * pg, (size_t)pg, s4));
    nvn_dirty_stats(&f0, &p0);
    mem[33 * pg] = 6;                                              /* claimed together with page 32 */
    nvn_dirty_stats(&f1, &p1);
    EXPECT("认领到 GPU 页之前的邻页不再缺页", 0, f1 - f0);
    nvn_dirty_stats(&f0, &p0);
    mem[35 * pg] = 6;                                              /* beyond the GPU page: still protected */
    nvn_dirty_stats(&f1, &p1);
    EXPECT("GPU 页之后的页仍受保护，写入触发真实缺页", 1, f1 - f0);
    EXPECT("GPU 页之后被写的页判脏", 1, nvn_dirty_since(mem + 35 * pg, (size_t)pg, s4));
    nvn_dirty_stats(&f0, &p0);
    mem[34 * pg] = 6;                                              /* a real write to the GPU page is still seen */
    nvn_dirty_stats(&f1, &p1);
    EXPECT("真正写 GPU 页本身仍然缺页", 1, f1 - f0);
    EXPECT("真正写 GPU 页本身判脏", 1, nvn_dirty_since(mem + 34 * pg, (size_t)pg, s4));
    /* Ranges near the end of a pool must not run past it. */
    nvn_dirty_protect(mem + (PAGES - 1) * pg, (size_t)pg);
    uint64_t s3 = nvn_dirty_now();
    mem[(PAGES - 1) * pg] = 5;
    EXPECT("池末尾页保护和写入在边界内", 1, nvn_dirty_since(mem + (PAGES - 1) * pg, (size_t)pg, s3));
    nvn_dirty_remove_range(mem);
    munmap(mem, (size_t)PAGES * (size_t)pg);
    (void)WINDOW;
    printf("DIRTY_AROUND failures=%d\n", g_fail);
    return g_fail != 0;
}
