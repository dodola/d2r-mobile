/* Portable NVN null-resource lowering for Vulkan devices without robustness2.
 * Remove accesses to resources known to be unbound for this pipeline variant:
 * numeric reads/queries/atomics become zero; writes are discarded. Keep all
 * accesses to real resources unchanged. Opaque/pointer escapes fail closed.
 */
#ifndef NVN_VK_NULL_SHADER_H
#define NVN_VK_NULL_SHADER_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {size_t at;uint32_t count,op,type,id;int action;} NvnNullInstruction;
/* action: 0 preserve, 1 remove opaque/pointer access, 2 zero numeric result. */
static int nvn_vk_null_shader(const uint32_t *in,size_t words,const uint32_t *bindings,size_t count,
        uint32_t **output,size_t *output_words,const char **error) {
    *output=NULL;*output_words=0;*error="invalid SPIR-V null-resource input";
    if(words<5||in[0]!=0x07230203||!in[3]||in[3]>1000000)return 0;
    uint32_t bound=in[3];int ok=0;size_t ni=0,first_function=words;
    NvnNullInstruction *ins=calloc(words,sizeof *ins);
    uint8_t *taint=calloc(bound,1),*opaque=calloc(bound,1),*removed=calloc(bound,1);
    uint32_t *binding=malloc((size_t)bound*4),*zero=calloc(bound,4);
    if(!ins||!taint||!opaque||!removed||!binding||!zero){*error="null-resource allocation";goto done;}
    for(uint32_t i=0;i<bound;i++)binding[i]=UINT32_MAX;
    for(size_t at=5;at<words;){
        uint32_t n=in[at]>>16,op=in[at]&0xffff;
        if(!n||n>words-at)goto done;
        NvnNullInstruction *p=&ins[ni++];*p=(NvnNullInstruction){.at=at,.count=n,.op=op};
        if(op==54&&first_function==words)first_function=at;
        if(op==71&&n==4&&in[at+1]<bound&&in[at+2]==33)binding[in[at+1]]=in[at+3];
        if((op==25||op==26||op==27||op==32)&&n>=2&&in[at+1]<bound)opaque[in[at+1]]=1;
        at+=n;
    }
    if(first_function==words){*error="null-resource module has no function";goto done;}
    for(uint32_t id=1;id<bound;id++)for(size_t j=0;j<count;j++)if(binding[id]==bindings[j])taint[id]=1;
    for(size_t pass=0;pass<=ni;pass++){
        int changed=0;
        for(size_t i=0;i<ni;i++){
            NvnNullInstruction *p=&ins[i];if(p->at<first_function)continue;
            const uint32_t *a=in+p->at;uint32_t n=p->count,op=p->op;int access=0,action=0;
            if((op==65||op==66||op==67||op==70||op==61||op==68||op==83||op==100||op==60)&&n>=4)
                access=a[3]<bound&&taint[a[3]];
            else if(op==86&&n==5)access=(a[3]<bound&&taint[a[3]])||(a[4]<bound&&taint[a[4]]);
            else if(((op>=87&&op<=98)||(op>=101&&op<=107)||(op>=305&&op<=315)||op==320||(op>=227&&op<=242&&op!=228)||op==318)&&n>=4)
                access=a[3]<bound&&taint[a[3]];
            else if((op==62||op==99||op==228||op==319)&&n>=3){
                if(a[1]<bound&&taint[a[1]])p->action=1;
                continue;
            }
            if(!access)continue;
            if(n<4||a[1]>=bound||a[2]>=bound){*error="null-resource result shape";goto done;}
            p->type=a[1];p->id=a[2];
            if(opaque[p->type]){
                action=1;if(!taint[p->id]){taint[p->id]=1;removed[p->id]=1;changed=1;}
            }else action=2;
            p->action=action;
        }
        if(!changed)break;
        if(pass==ni){*error="null-resource propagation did not converge";goto done;}
    }
    /* Refuse pointer/opaque values passed through an operation whose semantics
     * were not proved above (for example a mixed null/live pointer Phi). */
    for(size_t i=0;i<ni;i++){
        NvnNullInstruction *p=&ins[i];if(p->at<first_function||p->action)continue;
        const uint32_t *a=in+p->at;
        for(uint32_t j=1;j<p->count;j++){
            if(p->op==8||p->op==317||(p->op==12&&j==4)||
               (p->op==79&&j>=5)||(p->op==81&&j>=4)||(p->op==82&&j>=5)||
               (p->op==246&&j>=3)||(p->op==247&&j>=2)||(p->op==250&&j>=4)||
               (p->op==251&&j>=3&&(j&1)))continue;
            if(a[j]<bound&&taint[a[j]]){
            *error="unsupported null-resource pointer/opaque escape";goto done;
            }
        }
    }
    uint32_t next=bound;size_t extra=0;
    for(size_t i=0;i<ni;i++)if(ins[i].action==2&&!zero[ins[i].type]){zero[ins[i].type]=next++;extra+=3;}
    uint32_t *out=malloc((words+extra)*4);if(!out){*error="null-resource output allocation";goto done;}
    memcpy(out,in,20);out[3]=next;size_t at=5;int constants=0;
    for(size_t i=0;i<ni;i++){
        NvnNullInstruction *p=&ins[i];const uint32_t *a=in+p->at;
        if(!constants&&p->at==first_function){
            for(uint32_t t=1;t<bound;t++)if(zero[t]){out[at++]=(3u<<16)|46;out[at++]=t;out[at++]=zero[t];}
            constants=1;
        }
        if(p->action==1)continue;
        if((p->op==5||p->op==6||p->op==71||p->op==72)&&p->count>=2&&a[1]<bound&&removed[a[1]])continue;
        if(p->action==2){out[at++]=(4u<<16)|83;out[at++]=p->type;out[at++]=p->id;out[at++]=zero[p->type];}
        else{memcpy(out+at,a,p->count*4);at+=p->count;}
    }
    *output=out;*output_words=at;*error=NULL;ok=1;
done:
    free(ins);free(taint);free(opaque);free(removed);free(binding);free(zero);return ok;
}
#endif
