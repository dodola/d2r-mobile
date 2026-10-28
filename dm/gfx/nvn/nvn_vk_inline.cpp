/* SPIRV-Tools 的 C 桥，三步与 upstream 2428e5b 一致，验证输出后才交给驱动。
 * C bridge to SPIRV-Tools, using upstream 2428e5b's three passes; validates output before the driver sees it. */
#include "nvn_vk_inline.h"
#include <spirv-tools/optimizer.hpp>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>
#include <mutex>

int dm_vk_subgroup_helper(const uint32_t *w,size_t n) {
    if(!w||n<5||w[0]!=0x07230203)return -1;
    std::unordered_set<uint32_t> entry;
    for(size_t i=5;i<n;){size_t wc=w[i]>>16;unsigned op=w[i]&65535;
        if(!wc||wc>n-i)return -1;
        if(op==15&&wc>=3)entry.insert(w[i+2]);i+=wc;
    }
    uint32_t cur=0;bool in=false,found=false;
    for(size_t i=5;i<n;){size_t wc=w[i]>>16;unsigned op=w[i]&65535;
        if(op==54){if(wc<5||in)return -1;cur=w[i+2];in=true;}
        else if(op==56){if(!in)return -1;in=false;}
        else if(op>=333&&op<=366&&in&&!entry.count(cur))found=true;i+=wc;
    }
    return in?-1:int(found);
}
int dm_vk_inline_helpers(const uint32_t *w,size_t n,uint32_t **out,size_t *count) {
    *out=nullptr;*count=0;
    int needed=dm_vk_subgroup_helper(w,n);if(needed<=0)return needed;
    /* 预热与游戏共用优化字节；完整输入比对防止散列碰撞，容量封顶 32 MiB。
     * Warm-up and gameplay reuse optimized bytes; full input comparisons reject hash collisions, with a 32 MiB cap. */
    struct Cached {std::vector<uint32_t> input,output;};
    static std::mutex lock;static std::vector<Cached> cache;static size_t used=0;
    {
        std::lock_guard<std::mutex> guard(lock);
        for(const auto &entry:cache)if(entry.input.size()==n&&!std::memcmp(entry.input.data(),w,n*4)){
            auto p=static_cast<uint32_t*>(std::malloc(entry.output.size()*4));if(!p)return -1;
            std::memcpy(p,entry.output.data(),entry.output.size()*4);*out=p;*count=entry.output.size();return 1;
        }
    }
    spvtools::Optimizer opt(SPV_ENV_VULKAN_1_1);
    opt.SetMessageConsumer([](spv_message_level_t,const char*,const spv_position_t&,const char *msg){std::fprintf(stderr,"[nvn-vulkan] INLINE_DIAGNOSTIC %s\n",msg);});
    opt.RegisterPass(spvtools::CreateMergeReturnPass());
    opt.RegisterPass(spvtools::CreateInlineExhaustivePass());
    opt.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    std::vector<uint32_t> result;
    if(!opt.Run(w,n,&result)||dm_vk_subgroup_helper(result.data(),result.size())!=0)return -1;
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
    if(!validator.Validate(result))return -1;
    auto p=static_cast<uint32_t*>(std::malloc(result.size()*4));if(!p)return -1;
    {
        std::lock_guard<std::mutex> guard(lock);size_t size=(n+result.size())*4;
        if(size<=(32u<<20)-used){cache.push_back({std::vector<uint32_t>(w,w+n),result});used+=size;}
    }
    std::memcpy(p,result.data(),result.size()*4);*out=p;*count=result.size();return 1;
}
