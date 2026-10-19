/* Standalone Android texture-view ABI regression through the real forced
 * bootstrap and NVN thunks. No device, surface, shader cache or game assets.
 * The guest's two adjacent 16-byte views are followed immediately by a copy
 * region, reproducing the boundary that the old larger host overlay damaged.
 */
#include "hle.h"
#include "nvn.h"
#include "nvn_count.h"
#include <stddef.h>
#include <stdatomic.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
void *dm_android_native_window(void) { return NULL; }

extern const hle_force_ent __start_dm_force[], __stop_dm_force[];
typedef struct { _Atomic uint64_t count; void *fn; } Slot;
extern Slot nvn_slot[NVN_NFUNC];
extern const char *nvn_names[NVN_NFUNC];

#define VIEW_API(X) \
    X(SetDefaults, SETDEFAULTS) X(SetLevels, SETLEVELS) \
    X(SetLayers, SETLAYERS) X(SetFormat, SETFORMAT) \
    X(SetTarget, SETTARGET) X(SetSwizzle, SETSWIZZLE) \
    X(SetDepthStencilMode, SETDEPTHSTENCILMODE) \
    X(GetLevels, GETLEVELS) X(GetLayers, GETLAYERS) \
    X(GetFormat, GETFORMAT) X(GetTarget, GETTARGET) \
    X(GetSwizzle, GETSWIZZLE) X(GetDepthStencilMode, GETDEPTHSTENCILMODE)
#define DECLARE(name, upper) static PFNNVNTEXTUREVIEW##upper##PROC p_##name;
VIEW_API(DECLARE)
#undef DECLARE

typedef struct { _Alignas(8) unsigned char bytes[16]; } GuestView;
typedef struct {
    unsigned char before[16];
    GuestView view[2];
    NVNcopyRegion region;
    unsigned char after[16];
} Fixture;
_Static_assert(sizeof(NVNtextureView) == 16, "Android public guest view ABI");
_Static_assert(sizeof(GuestView) == 16, "fixed guest object boundary");
_Static_assert(offsetof(Fixture, region) == offsetof(Fixture, view) + 32,
               "copy region immediately follows two guest views");

typedef struct {
    unsigned levels, layers, format, target, swizzle, depth;
    int base, count, layer, layer_count;
    NVNformat fmt;
    NVNtextureTarget tgt;
    NVNtextureSwizzle channels[4];
    NVNtextureDepthStencilMode mode;
} Expected;
static unsigned checks, boundary_checks, copies;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) { fprintf(stderr, "NVN_VIEW_DEVICE failed=%s checks=%u\n", what, checks); exit(20); }
}
static NVNtextureView *view(Fixture *f, unsigned i) {
    return (NVNtextureView *)(void *)f->view[i].bytes;
}
static void protected_bytes(const Fixture *f, const Fixture *old, int writable) {
    const unsigned char *a = (const void *)f, *b = (const void *)old;
    size_t start = writable < 0 ? sizeof(*f) : offsetof(Fixture, view) + (unsigned)writable * 16;
    for (size_t i = 0; i < sizeof(*f); i++)
        if (i < start || i >= start + 16) check(a[i] == b[i], "adjacent view / region / guard changed");
    boundary_checks++;
}
/* Every setter may modify only its exact 16-byte guest object. */
#define MUTATE(index, call) do { Fixture old = f; call; protected_bytes(&f, &old, index); } while (0)

static void verify(Fixture *f, unsigned index, const Expected *e) {
    Fixture old = *f;
    struct {
        unsigned char before[16];
        int base, count, layer, layer_count;
        NVNformat format;
        NVNtextureTarget target;
        NVNtextureSwizzle swizzle[4];
        NVNtextureDepthStencilMode depth;
        unsigned char after[16];
    } out;
    memset(&out, 0x69, sizeof(out));
    NVNtextureView *v = view(f, index);
    check(p_GetLevels(v, &out.base, &out.count) == e->levels, "levels set flag");
    if (e->levels) check(out.base == e->base && out.count == e->count, "levels roundtrip");
    check(p_GetLayers(v, &out.layer, &out.layer_count) == e->layers, "layers set flag");
    if (e->layers) check(out.layer == e->layer && out.layer_count == e->layer_count, "layers roundtrip");
    check(p_GetFormat(v, &out.format) == e->format, "format set flag");
    if (e->format) check(out.format == e->fmt, "format roundtrip");
    check(p_GetTarget(v, &out.target) == e->target, "target set flag");
    if (e->target) check(out.target == e->tgt, "target roundtrip");
    check(p_GetSwizzle(v, &out.swizzle[0], &out.swizzle[1], &out.swizzle[2], &out.swizzle[3]) == e->swizzle,
          "swizzle set flag");
    for (unsigned i = 0; i < 4; i++)
        check(out.swizzle[i] == (e->swizzle ? e->channels[i] : (NVNtextureSwizzle)(2 + i)),
              "swizzle roundtrip / default identity");
    check(p_GetDepthStencilMode(v, &out.depth) == e->depth, "depth mode set flag");
    check(out.depth == (e->depth ? e->mode : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH),
          "depth mode roundtrip / default");
    for (unsigned i = 0; i < 16; i++) {
        check(out.before[i] == 0x69, "getter leading output guard");
        check(out.after[i] == 0x69, "getter trailing output guard");
    }
    protected_bytes(f, &old, -1);
}
static void verify_both(Fixture *f, const Expected *e) {
    verify(f, 0, &e[0]); verify(f, 1, &e[1]);
}

int main(int argc, char **argv) {
    if (argc > 2) return 2;
    g_t0_ns = now_ns();
    PFNNVNBOOTSTRAPLOADERPROC boot = NULL;
    for (const hle_force_ent *e = __start_dm_force; e < __stop_dm_force; e++)
        if (!strcmp(e->name, "nvnBootstrapLoader")) boot = (PFNNVNBOOTSTRAPLOADERPROC)e->fn;
    check(boot != NULL, "original forced bootstrap present");
#define LOAD(name, upper) p_##name = (PFNNVNTEXTUREVIEW##upper##PROC)boot("nvnTextureView" #name); check(p_##name != NULL, "bootstrap " #name);
    VIEW_API(LOAD)
#undef LOAD
    Fixture f; memset(&f, 0xa5, sizeof(f));
    f.region = (NVNcopyRegion){3, 5, 7, 1280, 720, 1};
    Expected e[2] = {0};
    MUTATE(0, p_SetDefaults(view(&f, 0)));
    MUTATE(1, p_SetDefaults(view(&f, 1)));
    const char *mode = argc == 2 ? argv[1] : "run";
    if (strcmp(mode, "run")) {
        if (!strcmp(mode, "levels128")) p_SetLevels(view(&f, 0), 128, 1);
        else if (!strcmp(mode, "level-count128")) p_SetLevels(view(&f, 0), 0, 128);
        else if (!strcmp(mode, "layers32768")) p_SetLayers(view(&f, 0), 32768, 1);
        else if (!strcmp(mode, "layer-count32768")) p_SetLayers(view(&f, 0), 0, 32768);
        else if (!strcmp(mode, "target11")) p_SetTarget(view(&f, 0), (NVNtextureTarget)11);
        else return 2;
        fprintf(stderr, "NVN_VIEW_DEVICE invalid representation accepted mode=%s\n", mode);
        return 21;
    }
    verify_both(&f, e);
    /* Initialize the second view first, then mutate the first to exercise both
     * the exact adjacent-view boundary and the following copy-region boundary. */
    e[1].levels = 1; e[1].base = 1; e[1].count = 1;
    MUTATE(1, p_SetLevels(view(&f, 1), 1, 1)); verify_both(&f, e);
    e[0].levels = 1; e[0].base = 4; e[0].count = 3;
    MUTATE(0, p_SetLevels(view(&f, 0), 4, 3)); verify_both(&f, e);
    e[0].layers = 1; e[0].layer = 257; e[0].layer_count = 17;
    MUTATE(0, p_SetLayers(view(&f, 0), 257, 17)); verify_both(&f, e);
    e[0].format = 1; e[0].fmt = NVN_FORMAT_RGBA32F;
    MUTATE(0, p_SetFormat(view(&f, 0), e[0].fmt)); verify_both(&f, e);
    e[0].target = 1; e[0].tgt = NVN_TEXTURE_TARGET_2D_ARRAY;
    MUTATE(0, p_SetTarget(view(&f, 0), e[0].tgt)); verify_both(&f, e);
    e[0].swizzle = 1;
    const NVNtextureSwizzle channels[4] = {NVN_TEXTURE_SWIZZLE_ONE, NVN_TEXTURE_SWIZZLE_ZERO,
                                         NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_R};
    memcpy(e[0].channels, channels, sizeof(channels));
    MUTATE(0, p_SetSwizzle(view(&f, 0), channels[0], channels[1], channels[2], channels[3]));
    verify_both(&f, e);
    e[0].depth = 1; e[0].mode = NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
    MUTATE(0, p_SetDepthStencilMode(view(&f, 0), e[0].mode)); verify_both(&f, e);
    /* Guest byte copies must carry every field without side-table identity. */
    MUTATE(1, memcpy(f.view[1].bytes, f.view[0].bytes, 16)); e[1] = e[0]; copies++;
    verify_both(&f, e);
    e[0] = (Expected){0};
    MUTATE(0, p_SetDefaults(view(&f, 0))); verify_both(&f, e);
    MUTATE(0, memcpy(f.view[0].bytes, f.view[1].bytes, 16)); e[0] = e[1]; copies++;
    verify_both(&f, e);
    /* Explicit zero-valued fields must remain distinct from unset fields. */
    e[0].base = 0; e[0].count = 1;
    MUTATE(0, p_SetLevels(view(&f, 0), 0, 1)); verify_both(&f, e);
    e[0].layer = 0; e[0].layer_count = 1;
    MUTATE(0, p_SetLayers(view(&f, 0), 0, 1)); verify_both(&f, e);
    e[0].fmt = NVN_FORMAT_RGBA8;
    MUTATE(0, p_SetFormat(view(&f, 0), e[0].fmt)); verify_both(&f, e);
    e[0].tgt = NVN_TEXTURE_TARGET_1D;
    MUTATE(0, p_SetTarget(view(&f, 0), e[0].tgt)); verify_both(&f, e);
    e[0].mode = NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
    MUTATE(0, p_SetDepthStencilMode(view(&f, 0), e[0].mode)); verify_both(&f, e);
    e[0] = (Expected){0};
    MUTATE(0, p_SetDefaults(view(&f, 0))); verify_both(&f, e);
    MUTATE(1, memcpy(f.view[1].bytes, f.view[0].bytes, 16)); e[1] = e[0]; copies++;
    verify_both(&f, e);
    uint64_t calls = 0; unsigned functions = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) {
        uint64_t n = atomic_load(&nvn_slot[i].count);
        if (n) { check(!strncmp(nvn_names[i], "nvnTextureView", 14), "only view thunks called"); functions++; calls += n; }
    }
    check(functions == 13, "all setters and getters used real thunks");
    printf("{\"passed\":true,\"guest_view_bytes\":16,\"adjacent_views\":2,\"copy_region_preserved\":true,"
           "\"canary_checks\":%u,\"checks\":%u,\"byte_copy_roundtrips\":%u,\"static_thunk_calls\":%llu,"
           "\"static_thunk_functions\":%u,\"surface_required\":false,\"shader_cache_required\":false,"
           "\"fixture_context\":true,\"original_game_frame\":false}\n",
           boundary_checks, checks, copies, (unsigned long long)calls, functions);
    return 0;
}
