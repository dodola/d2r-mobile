/* Host adapter and regression tests for exact draw-parameter specialization. */
#include "../../gfx/nvn/nvn_vk_draw_parameters.h"
#include <assert.h>
#include <stdio.h>

#define W(op,n) ((uint32_t)(n)<<16|(op))
static const uint32_t module[]={
    0x07230203,0x10300,0,64,0,
    W(17,2),1,W(17,2),4427,W(14,3),0,1,
    W(15,8),0,10,0x6e69616d,0,4,5,6,
    W(71,4),4,11,4424,W(71,4),5,11,4425,W(71,4),6,11,4426,
    W(21,4),1,32,1,W(32,4),2,1,1,W(19,2),7,W(33,3),8,7,
    W(59,4),2,4,1,W(59,4),2,5,1,W(59,4),2,6,1,
    W(54,5),7,10,0,8,W(248,2),11,
    W(61,4),1,12,4,W(61,4),1,13,5,W(61,4),1,14,6,
    W(128,5),1,15,12,13,W(253,1),W(56,1)
};
static size_t locate(uint32_t *m,size_t n,unsigned op,unsigned occurrence){
    for(size_t at=5;at<n;at+=m[at]>>16)if((m[at]&65535)==op&&!occurrence--)return at;
    assert(!"instruction not found");return 0;
}
static void reject(const uint32_t *m,size_t n){
    size_t words=123;uint32_t mask=123,*out=nvn_vk_draw_parameters(m,n,&words,&mask);
    assert(!out&&!words&&!mask);
}
static void unit_tests(void){
    const size_t n=sizeof module/4;size_t count=0;uint32_t mask=0;
    uint32_t *out=nvn_vk_draw_parameters(module,n,&count,&mask);assert(out&&mask==7&&out[3]==67);
    unsigned specs=0,copies=0,vars=0;
    for(size_t at=5;at<count;at+=out[at]>>16){
        unsigned op=out[at]&65535;specs+=op==50;copies+=op==83;vars+=op==59;
        if(op==50){assert(out[at+1]==1&&out[at+2]>=64&&out[at+2]<=66);}
        if(op==83&&out[at+2]>=12&&out[at+2]<=14)assert(out[at+3]==64+out[at+2]-12);
    }
    assert(specs==3&&copies==3&&!vars);free(out);
    uint32_t changed[512];memcpy(changed,module,sizeof module);
    size_t at=locate(changed,n,71,0);changed[at+3]=11;
    at=locate(changed,n,71,1);changed[at+3]=11;
    at=locate(changed,n,71,2);changed[at+3]=11;
    out=nvn_vk_draw_parameters(changed,n,&count,&mask);assert(out&&!mask&&count==n&&!memcmp(changed,out,n*4));free(out);
    memcpy(changed,module,sizeof module);changed[5]=0;reject(changed,n);
    memcpy(changed,module,sizeof module);changed[n-1]=W(56,2);reject(changed,n);
    memcpy(changed,module,sizeof module);at=locate(changed,n,15,0);changed[at]=W(15,4);changed[at+3]=0x41414141;
    /* Rebuild the truncated module so malformed name is the actual cause. */
    memmove(changed+at+4,changed+at+8,(n-at-8)*4);reject(changed,n-4);
    memcpy(changed,module,sizeof module);at=locate(changed,n,21,0);changed[at+2]=64;reject(changed,n);
    memcpy(changed,module,sizeof module);at=locate(changed,n,32,0);changed[at+2]=7;reject(changed,n);
    memcpy(changed,module,sizeof module);at=locate(changed,n,59,0);changed[at+3]=7;reject(changed,n);
    memcpy(changed,module,sizeof module);at=locate(changed,n,61,0);changed[at+1]=7;reject(changed,n);
    memcpy(changed,module,sizeof module);at=locate(changed,n,61,0);changed[at]=W(83,4);reject(changed,n);
    memcpy(changed,module,sizeof module);at=locate(changed,n,128,0);changed[at+3]=4;reject(changed,n);
    memcpy(changed,module,sizeof module);at=locate(changed,n,71,0);changed[at+2]=1;changed[at+3]=NVN_DRAW_SPEC_BASE;reject(changed,n);
    /* Append a real pointer escape at global annotation level. */
    memcpy(changed,module,sizeof module);changed[n]=W(74,3);changed[n+1]=20;changed[n+2]=4;reject(changed,n+3);
    /* Each literal below deliberately equals a removed variable ID. The
     * instructions are shape-only tests; real shaders are spirv-val checked
     * separately. Preserve every instruction byte, excluding only builtins. */
    static const uint32_t literals[]={
        W(8,4),30,4,5,W(12,7),1,31,32,4,12,13,
        W(79,8),20,33,34,35,4,5,6,W(81,5),1,36,34,4,
        W(82,6),20,37,12,34,5,W(246,6),40,41,4,5,6,
        W(247,3),42,4,W(250,6),43,44,45,4,5,
        W(251,7),12,46,4,47,5,48,W(256,3),49,4,
        W(98,7),20,50,51,52,4,12,W(61,6),1,53,54,2,4,
        W(63,7),55,56,2,4,2,5,W(349,7),1,57,58,4,12,13
    };
    memcpy(changed,module,sizeof module);size_t end=locate(changed,n,253,0),extra=sizeof literals/4;
    memmove(changed+end+extra,changed+end,(n-end)*4);memcpy(changed+end,literals,sizeof literals);
    out=nvn_vk_draw_parameters(changed,n+extra,&count,&mask);assert(out&&mask==7);
    size_t literal_start=locate(out,count,8,0);assert(!memcmp(out+literal_start,literals,sizeof literals));free(out);
    /* Exhaustively truncate at each word and perturb instruction length. */
    for(size_t cut=0;cut<5;cut++)reject(module,cut);
    for(size_t i=5;i<n;i+=module[i]>>16){memcpy(changed,module,sizeof module);changed[i]&=65535;reject(changed,n);}
    puts("draw parameters: exact three-field mapping, int32/Input typing, literals, malformed bounds, reserved IDs and pointer escape checks passed");
}
int main(int argc,char **argv){
    if(argc==1){unit_tests();return 0;}
    if(argc!=3)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 3;
    if(fseek(f,0,SEEK_END)){fclose(f);return 3;}long bytes=ftell(f);rewind(f);
    if(bytes<20||bytes%4){fclose(f);return 3;}
    uint32_t *input=malloc((size_t)bytes),*output=NULL;size_t words=0;uint32_t mask=0;
    if(!input){fclose(f);return 3;}if(fread(input,1,(size_t)bytes,f)!=(size_t)bytes){fclose(f);free(input);return 3;}fclose(f);
    output=nvn_vk_draw_parameters(input,(size_t)bytes/4,&words,&mask);free(input);
    if(!output){fprintf(stderr,"draw-parameter lowering rejected input\n");return 4;}
    f=fopen(argv[2],"wb");if(!f){free(output);return 3;}
    int ok=fwrite(output,4,words,f)==words;if(fclose(f))ok=0;free(output);
    if(!ok)return 3;
    printf("{\"mask\":%u,\"words\":%zu,\"spec_base\":%u}\n",mask,words,NVN_DRAW_SPEC_BASE);return 0;
}
