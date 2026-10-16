/* Reconstruct affected vertex inputs into Private variables at entry. Existing
 * loads, constant/dynamic component access and local pointer aliases then read
 * the same complete value. Input pointers escaping to functions or mixed-storage
 * pointer selection are rejected explicitly. No GLSL/runtime compiler needed. */
#ifndef NVN_VK_VERTEX_SPV_H
#define NVN_VK_VERTEX_SPV_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t split,convert[16],tail[16]; } NvnVkVertexAdapt;
typedef struct { uint32_t type,scalar,components,kind; } NvnVkVertexInput;
typedef struct {
    uint32_t variable,type,scalar,components,head,head_type,head_ptr,tail,tail_ptr,raw_scalar,convert;
} NvnVkVertexRewrite;
typedef struct { uint32_t op,a,b,id; } NvnVkVertexType;
static uint32_t nvn_vk_vertex_intern(const uint32_t *w,size_t words,NvnVkVertexType types[128],
                                     unsigned *count,uint32_t *next,unsigned op,uint32_t a,uint32_t b) {
    for(size_t o=5;o<words;o+=w[o]>>16)
        if((w[o]&65535)==op&&w[o]>>16==4&&w[o+2]==a&&w[o+3]==b)return w[o+1];
    for(unsigned i=0;i<*count;i++)if(types[i].op==op&&types[i].a==a&&types[i].b==b)return types[i].id;
    if(*count==128)return 0;
    uint32_t id=(*next)++;types[(*count)++]=(NvnVkVertexType){op,a,b,id};return id;
}

/* The input ABI comes from SPIR-V, never vkmeta's reserved attribute word. */
static int nvn_vk_vertex_inputs(const uint32_t *w,size_t words,NvnVkVertexInput input[16]) {
    if(words<5||w[0]!=0x07230203||!w[3]||w[3]>1000000)return 0;
    uint32_t bound=w[3];uint32_t *type=calloc(bound,16);if(!type)return 0;
    uint32_t *op=type,*a=op+bound,*b=a+bound,*location=b+bound;
    for(uint32_t i=0;i<bound;i++)location[i]=UINT32_MAX;
    memset(input,0,16*sizeof(*input));int ok=1;
    for(size_t o=5;o<words;){uint32_t n=w[o]>>16,k=w[o]&65535;if(!n||n>words-o){ok=0;break;}
        if((k==21&&n==4)||(k==22&&n==3)||(k==23&&n==4)||(k==32&&n==4)){
            uint32_t id=w[o+1];if(id>=bound){ok=0;break;}op[id]=k;a[id]=w[o+2];b[id]=n>3?w[o+3]:0;
        }
        if(k==71&&n==4&&w[o+2]==30){if(w[o+1]>=bound){ok=0;break;}location[w[o+1]]=w[o+3];}
        o+=n;
    }
    if(ok)for(size_t o=5;o<words;o+=w[o]>>16)if((w[o]&65535)==59&&w[o]>>16>=4&&w[o+3]==1){
        uint32_t ptr=w[o+1],id=w[o+2];if(ptr>=bound||id>=bound||b[ptr]>=bound){ok=0;break;}
        if(location[id]==UINT32_MAX)continue;
        uint32_t loc=location[id],value=b[ptr],scalar=op[value]==23?a[value]:value;
        if(loc>=16||scalar>=bound||input[loc].type||a[scalar]!=32||
            (op[scalar]!=21&&op[scalar]!=22)){ok=0;break;}
        uint32_t count=op[value]==23?b[value]:1;if(!count||count>4){ok=0;break;}
        input[loc]=(NvnVkVertexInput){value,scalar,count,op[scalar]==22?0:b[scalar]?2:1};
    }
    if(ok)for(size_t o=5;o<words;o+=w[o]>>16)
        if((w[o]&65535)==71&&w[o]>>16==4&&w[o+2]==31&&w[o+1]<bound&&
           location[w[o+1]]!=UINT32_MAX&&w[o+3]){ok=0;break;}
    free(type);return ok;
}

static uint8_t *nvn_vk_vertex_adapt(const uint8_t *bytes,size_t *size,
                                   const NvnVkVertexAdapt *plan) {
    const uint32_t *w=(const uint32_t *)bytes;size_t words=*size/4;
    NvnVkVertexInput inputs[16];if(*size%4||!nvn_vk_vertex_inputs(w,words,inputs))return NULL;
    uint32_t bound=w[3],next=bound;
    uint32_t *data=calloc(bound,3*sizeof(uint32_t));if(!data)return NULL;
    uint32_t *ptr_private=data,*derived=data+bound,*loc=data+2*bound;
    for(uint32_t i=0;i<bound;i++)loc[i]=UINT32_MAX;
    NvnVkVertexRewrite r[16]={0};uint32_t active=0,entry=0;
    NvnVkVertexType types[128];unsigned type_count=0;
    for(size_t o=5;o<words;o+=w[o]>>16){unsigned n=w[o]>>16,k=w[o]&65535;
        if(k==15&&n>=4&&w[o+1]==0){if(entry&&entry!=w[o+2])goto fail;entry=w[o+2];}
        if(k==32&&n==4&&w[o+1]<bound&&w[o+2]==1){
            ptr_private[w[o+1]]=nvn_vk_vertex_intern(w,words,types,&type_count,&next,32,6,w[o+3]);
            if(!ptr_private[w[o+1]])goto fail;
        }
        if(k==71&&n==4&&w[o+2]==30&&w[o+1]<bound)loc[w[o+1]]=w[o+3];
    }
    if(!entry)goto fail;
    for(size_t o=5;o<words;o+=w[o]>>16)if((w[o]&65535)==59&&w[o]>>16>=4&&w[o+3]==1){
        uint32_t id=w[o+2],p=w[o+1];if(id>=bound||p>=bound)goto fail;unsigned l=loc[id];if(l>=16)continue;
        if(!(plan->split&(1u<<l))&&!plan->convert[l])continue;
        NvnVkVertexInput in=inputs[l];if(plan->convert[l]&&in.kind!=0)goto fail;
        if(!plan->convert[l]&&in.components<3)continue;
        if(!ptr_private[p]||!in.type)goto fail;
        active|=1u<<l;derived[id]=ptr_private[p];
        r[l]=(NvnVkVertexRewrite){.variable=id,.type=in.type,.scalar=in.scalar,.components=in.components,
            .head_type=in.type,.raw_scalar=in.scalar,.convert=plan->convert[l]};
        r[l].head=next++;
        if(plan->convert[l]){
            r[l].raw_scalar=nvn_vk_vertex_intern(w,words,types,&type_count,&next,21,32,plan->convert[l]==2);
            r[l].head_type=in.components==1?r[l].raw_scalar:nvn_vk_vertex_intern(w,words,types,&type_count,&next,23,r[l].raw_scalar,in.components);
        }
        r[l].head_ptr=nvn_vk_vertex_intern(w,words,types,&type_count,&next,32,1,r[l].head_type);
        if((plan->split&(1u<<l))&&in.components>=3){r[l].tail=next++;r[l].tail_ptr=nvn_vk_vertex_intern(w,words,types,&type_count,&next,32,1,r[l].raw_scalar);}
        if(!r[l].raw_scalar||!r[l].head_type||!r[l].head_ptr||(r[l].tail&&!r[l].tail_ptr))goto fail;
    }
    if(!active)goto fail;
    /* Retype descendants without changing any component indexing or control flow. */
    int changed;
    do {changed=0;for(size_t o=5;o<words;o+=w[o]>>16){unsigned n=w[o]>>16,k=w[o]&65535;uint32_t result=0,p=0;
        if((k==65||k==66||k==83)&&n>=4){result=w[o+2];p=w[o+3];}
        if(k==169&&n==6){result=w[o+2];p=w[o+4];if(p>=bound||w[o+5]>=bound)goto fail;if(!derived[p])p=w[o+5];}
        if(k==245&&n>=5){result=w[o+2];for(unsigned j=3;j<n;j+=2)if(w[o+j]<bound&&derived[w[o+j]]){p=w[o+j];break;}}
        if(result>=bound||p>=bound)goto fail;
        if(result&&derived[p]&&!derived[result]){if(w[o+1]>=bound||!ptr_private[w[o+1]])goto fail;derived[result]=ptr_private[w[o+1]];changed=1;}
    }}while(changed);
    for(size_t o=5;o<words;o+=w[o]>>16){unsigned n=w[o]>>16,k=w[o]&65535;
        if(k==57)for(unsigned j=4;j<n;j++)if(w[o+j]<bound&&derived[w[o+j]])goto fail;
        if(k==254&&n==2&&w[o+1]<bound&&derived[w[o+1]])goto fail;
        if(k==67&&n>=4&&w[o+3]<bound&&derived[w[o+3]])goto fail;
        if(k==169&&n==6&&w[o+2]<bound&&derived[w[o+2]])
            if(w[o+4]>=bound||w[o+5]>=bound||derived[w[o+4]]!=derived[w[o+5]])goto fail;
        if(k==245&&n>=5&&w[o+2]<bound&&derived[w[o+2]])
            for(unsigned j=3;j<n;j+=2)if(w[o+j]>=bound||derived[w[o+j]]!=derived[w[o+2]])goto fail;
    }
    if(words>SIZE_MAX/4-bound*4-2048)goto fail;
    size_t capacity=words+(size_t)bound*4+2048,used=5;uint32_t *out=malloc(capacity*4);if(!out)goto fail;
    memcpy(out,w,20);int annotations=0,globals=0,inside=0,pending=0,found=0;
#define VEMIT(...) do {const uint32_t v_[]={__VA_ARGS__};size_t z_=sizeof(v_)/4;if(z_>capacity-used){free(out);goto fail;}memcpy(out+used,v_,sizeof v_);used+=z_;}while(0)
    for(size_t o=5;o<words;){unsigned n=w[o]>>16,k=w[o]&65535;
        if(!annotations&&k>=19&&k<=39){
            for(unsigned l=0;l<16;l++)if(active&(1u<<l)){
                VEMIT(4u<<16|71,r[l].head,30,l);
                if(r[l].tail)VEMIT(4u<<16|71,r[l].tail,30,plan->tail[l]);
            }
            annotations=1;
        }
        if(k==54&&!globals){
            for(unsigned t=0;t<type_count;t++)VEMIT(4u<<16|types[t].op,types[t].id,types[t].a,types[t].b);
            /* Original types may be interleaved with global variables. Emit
             * globals after all types so reused pointer types are never forward
             * references. Preserve the original variable order/initializers. */
            for(size_t g=5;g<o;g+=w[g]>>16)if((w[g]&65535)==59){
                unsigned count=w[g]>>16;if(count>capacity-used){free(out);goto fail;}
                memcpy(out+used,w+g,count*4);
                if(w[g+2]<bound&&derived[w[g+2]]){out[used+1]=derived[w[g+2]];out[used+3]=6;}
                used+=count;
            }
            for(unsigned l=0;l<16;l++)if(active&(1u<<l)){
                VEMIT(4u<<16|59,r[l].head_ptr,r[l].head,1);
                if(r[l].tail)VEMIT(4u<<16|59,r[l].tail_ptr,r[l].tail,1);
            }
            globals=1;
        }
        if(k==59&&!globals){o+=n;continue;}
        if(k==54)inside=w[o+2]==entry;
        if(k==248&&inside&&!found){pending=1;found=1;}
        else if(pending&&k!=59&&k!=8&&k!=317){
            for(unsigned l=0;l<16;l++)if(active&(1u<<l)){
                NvnVkVertexRewrite v=r[l];uint32_t value=next++;
                VEMIT(4u<<16|61,v.head_type,value,v.head);
                if(v.tail){uint32_t tail=next++,joined=next++;VEMIT(4u<<16|61,v.raw_scalar,tail,v.tail);VEMIT(6u<<16|82,v.head_type,joined,tail,value,2);value=joined;}
                if(v.convert){uint32_t converted=next++;VEMIT(4u<<16|(v.convert==1?112:111),v.type,converted,value);value=converted;}
                VEMIT(3u<<16|62,v.variable,value);
            }
            pending=0;
        }
        if(k==15&&n>=4&&w[o+1]==0){
            size_t start=used;VEMIT(w[o],w[o+1],w[o+2]);unsigned j=3;
            do {if(j>=n){free(out);goto fail;}uint32_t s=w[o+j++];VEMIT(s);if(!(s&0xff)||!(s&0xff00)||!(s&0xff0000)||!(s&0xff000000))break;}while(1);
            for(;j<n;j++){uint32_t id=w[o+j];if(id>=bound){free(out);goto fail;}if(!derived[id]||w[1]>=0x10400)VEMIT(id);}
            for(unsigned l=0;l<16;l++)if(active&(1u<<l)){VEMIT(r[l].head);if(r[l].tail)VEMIT(r[l].tail);}
            out[start]=((uint32_t)(used-start)<<16)|15;o+=n;continue;
        }
        if(k==71&&n>=3&&w[o+1]<bound&&derived[w[o+1]]){
            unsigned d=w[o+2];if(d==30){o+=n;continue;}
            if(d==13||d==14||d==16||d==17||d==18||d==31||d==32){
                for(unsigned l=0;l<16;l++)if(r[l].variable==w[o+1]){
                    memcpy(out+used,w+o,n*4);out[used+1]=r[l].head;used+=n;
                    if(r[l].tail&&d!=31&&d!=32){memcpy(out+used,w+o,n*4);out[used+1]=r[l].tail;used+=n;}
                }
                o+=n;continue;
            }
        }
        if(n>capacity-used){free(out);goto fail;}memcpy(out+used,w+o,n*4);
        if(k==59&&n>=4&&w[o+2]<bound&&derived[w[o+2]]){out[used+1]=derived[w[o+2]];out[used+3]=6;}
        else if((k==65||k==66||k==83||k==169||k==245)&&n>=4&&w[o+2]<bound&&derived[w[o+2]])out[used+1]=derived[w[o+2]];
        used+=n;
        o+=n;
    }
    if(!globals||!found||pending||next>1000000){free(out);goto fail;}
    out[3]=next;*size=used*4;free(data);return (uint8_t *)out;
#undef VEMIT
fail:free(data);return NULL;
}
#endif
