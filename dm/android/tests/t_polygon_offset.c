/* Exercise the production opcode dispatcher without a Vulkan device. This
 * deliberately includes replay internals to register only a CPU command pool.
 * The offline linker/harness must trap any unexpected external function call. */
#include "../../gfx/nvn/nvn_vk.c"
#include <assert.h>

int main(void) {
    struct {op_hdr begin,offset; p_poly_offset values;uint32_t pad;op_hdr end;} commands={
        .begin={OP_BEGIN,8},.offset={OP_POLY_OFFSET,24},.values={1.5f,-2.0f,0.25f},.end={OP_END,8}};
    G.current_submission=&G.submission_slot;
    G.npool=1;G.pools[0].guest=(const uint8_t *)&commands;G.pools[0].size=sizeof commands;
    execute((const uint8_t *)&commands,0); // previously aborts even when offset is disabled
#ifndef EXPECT_OLD_ABORT
    assert(G.state.polygon_offset.factor==1.5f&&G.state.polygon_offset.units==-2.0f&&G.state.polygon_offset.clamp==0.25f);
    NvnVkDepthBias bias,other;
    assert(nvn_vk_depth_bias(0,1.5f,-2,.25f,0,&bias)==NVN_VK_DEPTH_BIAS_OK&&!bias.enabled);
    assert(nvn_vk_depth_bias(3,1.5f,-2,.25f,0,&bias)==NVN_VK_DEPTH_BIAS_OK&&!bias.enabled);
    assert(nvn_vk_depth_bias(4,1.5f,-2,0,0,&bias)==NVN_VK_DEPTH_BIAS_OK);
    assert(bias.enabled&&bias.constant==-2&&bias.slope==1.5f&&bias.clamp==0);
    assert(nvn_vk_depth_bias(7,1.5f,-2,.25f,1,&bias)==NVN_VK_DEPTH_BIAS_OK&&bias.enabled&&bias.clamp==.25f);
    assert(nvn_vk_depth_bias(4,1.5f,-2,.25f,0,&other)==NVN_VK_DEPTH_BIAS_CLAMP_UNSUPPORTED);
    assert(nvn_vk_depth_bias(4,NAN,0,0,1,&other)==NVN_VK_DEPTH_BIAS_INVALID);
    assert(nvn_vk_depth_bias(4,0,INFINITY,0,1,&other)==NVN_VK_DEPTH_BIAS_INVALID);
    assert(nvn_vk_depth_bias(4,0,0,-INFINITY,1,&other)==NVN_VK_DEPTH_BIAS_INVALID);
    assert(nvn_vk_depth_bias(0,NAN,INFINITY,NAN,0,&other)==NVN_VK_DEPTH_BIAS_OK&&!other.enabled);
    /* The complete effective bias is part of the pipeline key. Disabled
     * offset values canonicalize identically; enabled slope/units/clamp differ. */
    PsoKey a={0},b={0};a.depth_bias=bias;
    assert(memcmp(&a,&b,sizeof a));b.depth_bias=bias;assert(!memcmp(&a,&b,sizeof a));
    b.depth_bias.slope=2;assert(memcmp(&a,&b,sizeof a));b=a;
    b.depth_bias.constant=2;assert(memcmp(&a,&b,sizeof a));b=a;
    b.depth_bias.clamp=-.25f;assert(memcmp(&a,&b,sizeof a));
    commands.values=(p_poly_offset){0,0,0};execute((const uint8_t *)&commands,0);
    assert(G.state.polygon_offset.factor==0&&G.state.polygon_offset.units==0&&G.state.polygon_offset.clamp==0);
#endif
    return 0;
}
