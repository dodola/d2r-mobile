/* 检查扩展集隔离、幂等性、畸形模块原子拒绝和精确字节变更。
 * Check extension isolation, idempotence, atomic malformed-module rejection and exact word changes. */
#include "../nvn/nvn_vk_nan_minmax.h"
#include <assert.h>
#include <stdio.h>

static const uint32_t fixture[]={
    0x07230203,0x10000,0,32,0,
    (6u<<16)|11,1,0x4c534c47,0x6474732e,0x3035342e,0,
    (4u<<16)|11,2,0x6568744f,0x72,
    (7u<<16)|12,3,4,1,37,5,6,
    (7u<<16)|12,3,7,1,40,5,6,
    (8u<<16)|12,3,8,1,43,5,6,9,
    (7u<<16)|12,3,10,2,37,5,6,
    (7u<<16)|12,3,11,1,79,5,6
};
static void reject(const uint32_t *input,size_t n){
    uint32_t *w=malloc(n*4);assert(w);memcpy(w,input,n*4);
    NvnVkNanMinmaxStats s={1,2,3};assert(!nvn_vk_nan_minmax(w,n,&s));
    assert(!s.min&&!s.max&&!s.clamp&&!memcmp(w,input,n*4));free(w);
}
int main(void){
    enum {N=sizeof fixture/sizeof *fixture};uint32_t w[N],before[N];
    memcpy(w,fixture,sizeof w);NvnVkNanMinmaxStats s;
    assert(nvn_vk_nan_minmax(w,N,&s));assert(s.min==1&&s.max==1&&s.clamp==1);
    unsigned changes=0;for(unsigned i=0;i<N;i++)if(w[i]!=fixture[i]){
        assert((fixture[i]==37&&w[i]==79)||(fixture[i]==40&&w[i]==80)||(fixture[i]==43&&w[i]==81));changes++;
    }
    assert(changes==3&&w[41]==37);
    memcpy(before,w,sizeof w);assert(nvn_vk_nan_minmax(w,N,&s));
    assert(!s.min&&!s.max&&!s.clamp&&!memcmp(w,before,sizeof w));
    memcpy(w,fixture,sizeof w);w[0]=0;reject(w,N);
    memcpy(w,fixture,sizeof w);w[3]=0;reject(w,N);
    memcpy(w,fixture,sizeof w);w[5]=0;reject(w,N);
    memcpy(w,fixture,sizeof w);w[5]=(5u<<16)|11;reject(w,N);
    memcpy(w,fixture,sizeof w);w[6]=32;reject(w,N);
    memcpy(w,fixture,sizeof w);w[15]=(6u<<16)|12;reject(w,N);
    memcpy(w,fixture,sizeof w);w[18]=32;reject(w,N);
    reject(fixture,N-1);
    /* 最后一条坏指令前的有效改写不能泄漏到原模块。
     * A bad final instruction must not leave earlier valid rewrites in the input. */
    memcpy(w,fixture,sizeof w);w[N-7]=(6u<<16)|12;reject(w,N);
    /* 第二个同名扩展导入也必须被修正。
     * A second import of the same extension must also be rewritten. */
    uint32_t multi[N+6];memcpy(multi,fixture,sizeof fixture);
    memcpy(multi+N,fixture+5,6*4);multi[N+1]=12;multi[18]=12;
    assert(nvn_vk_nan_minmax(multi,N+6,&s)&&s.min==1&&s.max==1&&s.clamp==1);
    /* 对截断模块和任意字长做有界随机检查，供 ASan/UBSan 捕获越界。
     * Exercise truncation and random word counts under ASan/UBSan. */
    uint32_t random=0x629ab135;
    for(unsigned k=0;k<20000;k++){
        memcpy(w,fixture,sizeof w);random=random*1664525u+1013904223u;
        size_t n=random%(N+1);random=random*1664525u+1013904223u;
        w[random%N]=random;memcpy(before,w,sizeof w);
        int ok=nvn_vk_nan_minmax(w,n,&s);if(!ok)assert(!memcmp(w,before,sizeof w));
    }
    puts("NAN_MINMAX passed=1 malformed_atomic=1 random_cases=20000");return 0;
}
