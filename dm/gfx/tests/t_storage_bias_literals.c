/* Regression test for dm_storage_bias_apply (nvn_vk_storage_bias.h).
 *
 * A read-only raw-uint32 SSBO must be rewritten for index compensation even when a literal-only instruction
 * (OpCapability, OpMemoryModel, ...) happens to carry the same number as the variable's result id. The distance
 * repair appends OpCapability ClipDistance (32) / CullDistance (33); a variable with id 32 used to be rejected as
 * "used in an unsupported way", so the big vertex shaders that read a 19.5 MB buffer through a misaligned range
 * were copied whole into a temporary on every draw.
 *
 * Genuine unsupported uses (a store through the variable, taking its address into another instruction) must still
 * be rejected. */
#include "../nvn/nvn_vk_storage_bias.h"
#include <stdio.h>

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-60s want %-4lld got %-4lld %s\n", name, want_, got_, want_ == got_ ? "ok" : "FAIL"); \
    if (want_ != got_) g_fail++; \
} while (0)

enum { OP_CAPABILITY = 17, OP_MEMORY_MODEL = 14, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_TYPE_VOID = 19,
       OP_TYPE_INT = 21, OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_TYPE_FUNCTION = 33,
       OP_CONSTANT = 43, OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62,
       OP_ACCESS_CHAIN = 65, OP_LABEL = 248, OP_RETURN = 253 };
enum { DEC_BUFFER_BLOCK = 3, DEC_ARRAY_STRIDE = 6, DEC_BINDING = 33, DEC_DESCRIPTOR_SET = 34, DEC_OFFSET = 35 };

static size_t emit(uint32_t *w, size_t at, unsigned op, unsigned n, const uint32_t *operands) {
    w[at] = (uint32_t)n << 16 | op;
    for (unsigned i = 1; i < n; i++) w[at + i] = operands[i - 1];
    return at + n;
}
#define E(op, ...) do { const uint32_t o_[] = {__VA_ARGS__}; at = emit(w, at, (op), 1 + (unsigned)(sizeof o_ / sizeof o_[0]), o_); } while (0)

/* var_id is the id of the SSBO variable; capability is the literal of the first OpCapability.
 * store_through_var != 0 makes the module store through the variable (an unsupported use). */
static size_t build(uint32_t *w, uint32_t var_id, uint32_t capability, int store_through_var) {
    enum { VOID = 1, UINT = 2, INT = 3, C0 = 4, C5 = 5, ARR = 6, STR = 7, PSTR = 8, PUINT = 9, FNT = 10, MAIN = 11,
           LABEL = 12, AC = 13, LD = 14, BOUND = 64 };
    uint32_t id_var = var_id;
    size_t at = 5;
    w[0] = 0x07230203; w[1] = 0x00010300; w[2] = 0; w[3] = BOUND; w[4] = 0;
    E(OP_CAPABILITY, 1);
    E(OP_CAPABILITY, capability);
    E(OP_MEMORY_MODEL, 0, 1);
    E(OP_DECORATE, ARR, DEC_ARRAY_STRIDE, 4);
    E(OP_DECORATE, STR, DEC_BUFFER_BLOCK);
    E(OP_MEMBER_DECORATE, STR, 0, DEC_OFFSET, 0);
    E(OP_DECORATE, id_var, DEC_DESCRIPTOR_SET, 0);
    E(OP_DECORATE, id_var, DEC_BINDING, 1);
    E(OP_TYPE_VOID, VOID);
    E(OP_TYPE_INT, UINT, 32, 0);
    E(OP_TYPE_INT, INT, 32, 1);
    E(OP_CONSTANT, INT, C0, 0);
    E(OP_CONSTANT, INT, C5, 5);
    E(OP_TYPE_RUNTIME_ARRAY, ARR, UINT);
    E(OP_TYPE_STRUCT, STR, ARR);
    E(OP_TYPE_POINTER, PSTR, 2, STR);
    E(OP_TYPE_POINTER, PUINT, 2, UINT);
    E(OP_VARIABLE, PSTR, id_var, 2);
    E(OP_TYPE_FUNCTION, FNT, VOID);
    E(OP_FUNCTION, VOID, MAIN, 0, FNT);
    E(OP_LABEL, LABEL);
    E(OP_ACCESS_CHAIN, PUINT, AC, id_var, C0, C5);
    E(OP_LOAD, UINT, LD, AC);
    if (store_through_var) E(OP_STORE, AC, LD);
    E(OP_RETURN);
    E(OP_FUNCTION_END);
    return at;
}

static int apply(uint32_t var_id, uint32_t capability, int store, size_t *before, size_t *after, uint64_t *mask) {
    uint32_t words[256];
    size_t n = build(words, var_id, capability, store);
    uint8_t *bytes = malloc(n * 4);
    memcpy(bytes, words, n * 4);
    SBResource resources[2] = {{0, 0}, {1, 1}};
    SBInfo info;
    size_t size = n * 4;
    *before = size;
    uint8_t *out = dm_storage_bias_apply(bytes, &size, resources, 2, &info);
    int rewritten = out && out != bytes;
    *after = size;
    *mask = info.mask;
    free(out ? out : bytes);
    return rewritten;
}

int main(void) {
    size_t before, after;
    uint64_t mask;
    /* The ids 32 / 33 are exactly the literals the distance repair appends. */
    int r = apply(20, 32, 0, &before, &after, &mask);
    EXPECT("plain variable id: rewritten", 1, r);
    EXPECT("plain variable id: binding 1 in mask", 2, (long long)mask);
    r = apply(32, 32, 0, &before, &after, &mask);
    EXPECT("variable id == OpCapability literal (ClipDistance): rewritten", 1, r);
    EXPECT("  ... and the module grew", 1, after > before);
    EXPECT("  ... binding 1 in mask", 2, (long long)mask);
    r = apply(33, 33, 0, &before, &after, &mask);
    EXPECT("variable id == OpCapability literal (CullDistance): rewritten", 1, r);
    /* A store through the variable is a genuine unsupported use: unchanged. */
    r = apply(32, 32, 1, &before, &after, &mask);
    EXPECT("store through the variable: still rejected", 0, r);
    EXPECT("  ... mask empty", 0, (long long)mask);
    printf("STORAGE_BIAS_LITERALS failures=%d\n", g_fail);
    return g_fail != 0;
}
