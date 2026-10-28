/* 合成 SPIR-V 验证真实优化器、暖加载复用、坏模块拒绝及 Adreno 策略，不包含游戏内容。
 * Synthetic SPIR-V tests the real optimizer, warm-load reuse, malformed rejection and Adreno policy; contains no game content. */
#include "../nvn/nvn_vk_inline.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <vector>
int main(){
    assert(dm_vk_inline_default(0x5143,"Adreno (TM) 750"));
    assert(dm_vk_inline_default(0x5143,"Adreno 799"));
    for(const char *name:{"Adreno 730","Adreno 740","Adreno 830","Adreno X1-85","Mali-G925"})assert(!dm_vk_inline_default(0x5143,name));
    assert(!dm_vk_inline_default(0x13b5,"Adreno 750"));
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    for(const char *stage:{"GLCompute","Fragment"}){
        std::string text=std::string("OpCapability Shader\nOpCapability GroupNonUniform\nOpCapability GroupNonUniformArithmetic\nOpMemoryModel Logical GLSL450\nOpEntryPoint ")+stage+" %main \"main\"\n";
        text+=stage==std::string("GLCompute")?"OpExecutionMode %main LocalSize 1 1 1\n":"OpExecutionMode %main OriginUpperLeft\n";
        text+=R"(
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%scope = OpConstant %uint 3
%one = OpConstant %uint 1
%helper = OpFunction %void None %fn
%h = OpLabel
%sum = OpGroupNonUniformIAdd %uint %scope Reduce %one
OpReturn
OpFunctionEnd
%main = OpFunction %void None %fn
%m = OpLabel
%call = OpFunctionCall %void %helper
OpReturn
OpFunctionEnd
)";
        std::vector<uint32_t> input;assert(tools.Assemble(text,&input));assert(tools.Validate(input));assert(dm_vk_subgroup_helper(input.data(),input.size())==1);
        uint32_t *out=nullptr;size_t n=0;assert(dm_vk_inline_helpers(input.data(),input.size(),&out,&n)==1);
        assert(dm_vk_subgroup_helper(out,n)==0&&tools.Validate(out,n));
        spvtools::Optimizer reference(SPV_ENV_VULKAN_1_1);reference.RegisterPass(spvtools::CreateMergeReturnPass());reference.RegisterPass(spvtools::CreateInlineExhaustivePass());reference.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
        std::vector<uint32_t> expected;assert(reference.Run(input.data(),input.size(),&expected));assert(expected.size()==n&&!memcmp(expected.data(),out,n*4));
        uint32_t *again=nullptr;size_t n2=0;assert(dm_vk_inline_helpers(input.data(),input.size(),&again,&n2)==1&&n2==n&&!memcmp(out,again,n*4));free(again);
        assert(dm_vk_inline_helpers(out,n,&again,&n2)==0&&again==nullptr);
        free(out);input[5]=0;assert(dm_vk_inline_helpers(input.data(),input.size(),&out,&n)==-1);
    }
    puts("INLINE PASS compute fragment byte_identity warm_reuse malformed policy");
}
