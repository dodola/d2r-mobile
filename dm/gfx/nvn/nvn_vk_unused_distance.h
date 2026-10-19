/* Remove only provably unaccessed ClipDistance/CullDistance output members.
 * All active values and unrelated shader operations remain intact. Input must
 * be valid SPIR-V. Unsupported aggregate/alias layouts return an unchanged
 * allocated copy; malformed consumed shapes/allocation failures return NULL.
 */
#ifndef NVN_VK_UNUSED_DISTANCE_H
#define NVN_VK_UNUSED_DISTANCE_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Numeric literals are not references, even when equal to a variable/type ID.
 * Unknown instructions are conservative: every operand is inspected. */
static int nvn_unused_distance_id_operand(const uint32_t *a,uint32_t n,uint32_t j) {
    uint32_t op=a[0]&65535u;
    if(op==8)return j==1;
    if(op==12)return j!=4;
    if(op==54||op==59)return j!=3;
    if(op==68||op==123)return j!=4;
    if(op==79||op==82)return j<5;
    if(op==81)return j<4;
    if(op==246)return j<3;
    if(op==247)return j<2;
    if(op==250)return j<4;
    /* Switch literal widths require type-sensitive decoding. Conservatively
     * retain the input whenever a candidate ID occurs in any switch word. */
    if(op==256||op==257)return j==1;
    if((op>=264&&op<=271)||op==342||(op>=349&&op<=364))return j!=4;
    uint32_t image_mask=0;
    if(op==87||op==88||op==91||op==92||op==95||op==98||
       op==305||op==306||op==309||op==310||op==313||op==320)image_mask=5;
    if(op==89||op==90||op==93||op==94||op==96||op==97||
       op==307||op==308||op==311||op==312||op==314||op==315)image_mask=6;
    if(op==99)image_mask=4;
    if(image_mask&&j==image_mask)return 0;
    uint32_t memory_mask=op==61?4:op==62?3:op==63?3:op==64?4:0;
    if(memory_mask&&j>=memory_mask){
        uint32_t pos=memory_mask;
        for(unsigned block=0;block<2&&pos<n;block++){
            uint32_t bits=a[pos];if(j==pos)return 0;pos++;
            if(bits&2u){if(j==pos)return 0;pos++;}
            if(bits&8u)pos++;
            if(bits&16u)pos++;
            if(bits&0x10000u)pos++;
            if(bits&0x20000u)pos++;
            if(op!=63&&op!=64)break;
        }
    }
    return 1;
}
static uint32_t *nvn_vk_unused_distance(const uint32_t *in,size_t words,
        size_t *out_words,uint32_t *removed_mask,const char **error) {
    if(out_words)*out_words=0;
    if(removed_mask)*removed_mask=0;
    if(error)*error=NULL;
    const char *failure="unused distance arguments";
    uint32_t *out=NULL,*struct_at=NULL,*pointer_to=NULL,*root=NULL,*constant=NULL,*constant_type=NULL;
    uint32_t *new_member=NULL,*rewrite=NULL,*extra_type=NULL,*extra_value=NULL;
    uint8_t *kind=NULL,*used=NULL,*candidate=NULL,*seen_root=NULL,*int32=NULL,*has_constant=NULL;
    size_t first_function=words,extra_count=0;uint32_t removed=0,retained=0,bound=0;
    if(!in||!out_words||!removed_mask||words<5)goto fail;
    failure="unused distance module header";
    if(in[0]!=0x07230203u||!in[3]||in[3]>1000000u||words>SIZE_MAX/sizeof(uint32_t)/2-8)goto fail;
    bound=in[3];
    struct_at=calloc(bound,4);pointer_to=calloc(bound,4);root=calloc(bound,4);
    constant=calloc(bound,4);constant_type=calloc(bound,4);int32=calloc(bound,1);has_constant=calloc(bound,1);
    candidate=calloc(bound,1);seen_root=calloc(bound,1);
    kind=calloc(words,1);used=calloc(words,1);new_member=calloc(words,4);rewrite=calloc(words,4);
    extra_type=calloc(words,4);extra_value=calloc(words,4);
    failure="unused distance allocation";
    if(!struct_at||!pointer_to||!root||!constant||!constant_type||!int32||!has_constant||
       !candidate||!seen_root||!kind||!used||!new_member||!rewrite||!extra_type||!extra_value)goto fail;
    for(size_t at=5;at<words;){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        failure="unused distance instruction bounds";
        if(!n||n>words-at)goto fail;
        if(op==30){
            failure="unused distance struct shape";
            if(n<2||!a[1]||a[1]>=bound||struct_at[a[1]]||at>UINT32_MAX)goto fail;
            struct_at[a[1]]=(uint32_t)at;
        }
        if(op==32){failure="unused distance pointer shape";if(n!=4||!a[1]||a[1]>=bound||!a[3]||a[3]>=bound)goto fail;pointer_to[a[1]]=a[3];}
        if(op==21&&n==4&&a[1]<bound&&a[2]==32&&a[3]<=1)int32[a[1]]=1;
        if(op==54&&first_function==words)first_function=at;
        at+=n;
    }
    for(size_t at=5;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op==72){
            failure="unused distance member decoration shape";
            if(n<4||!a[1]||a[1]>=bound||!struct_at[a[1]]||a[2]>=(in[struct_at[a[1]]]>>16)-2)goto fail;
            if(a[3]==11){
                if(n!=5)goto fail;
                if(a[4]==3||a[4]==4){kind[struct_at[a[1]]+2+a[2]]=(uint8_t)(a[4]==3?1:2);candidate[a[1]]=1;}
            }
        }
        if(op==43&&n==4&&a[1]<bound&&int32[a[1]]&&a[2]<bound){has_constant[a[2]]=1;constant[a[2]]=a[3];constant_type[a[2]]=a[1];}
    }
    int any=0;for(uint32_t id=1;id<bound;id++)any|=candidate[id];
    if(!any)goto unchanged;
    /* Only direct Output structs, with no aggregate constructors, array
     * wrappers, initializers, parameter types or pointer aliases, are proved. */
    for(size_t at=5;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op==73||op==74||op==75)goto unchanged;
        if(op==32&&candidate[a[3]]&&a[2]!=3)goto unchanged;
        if(op==30){for(uint32_t j=2;j<n;j++)if(a[j]<bound&&candidate[a[j]])goto unchanged;}
        if((op==28||op==29)&&n>=3&&a[2]<bound&&candidate[a[2]])goto unchanged;
        if(op==33){for(uint32_t j=2;j<n;j++)if(a[j]<bound&&(candidate[a[j]]||(pointer_to[a[j]]&&candidate[pointer_to[a[j]]])))goto unchanged;}
        if(op==59&&n>=4&&a[1]<bound&&pointer_to[a[1]]&&candidate[pointer_to[a[1]]]){
            if(n!=4||at>=first_function||a[3]!=3||!a[2]||a[2]>=bound)goto unchanged;
            root[a[2]]=pointer_to[a[1]];seen_root[root[a[2]]]=1;
        }
        /* Direct aggregate constants, loads, composite operations etc. */
        if(n>=3&&op!=30&&op!=5&&op!=6&&op!=71&&op!=72&&op!=32&&a[1]<bound&&candidate[a[1]])goto unchanged;
    }
    for(uint32_t id=1;id<bound;id++)if(candidate[id]&&!seen_root[id])goto unchanged;
    for(size_t at=first_function;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        int chain=(op==65||op==66)&&n>=5&&a[3]<bound&&root[a[3]];
        if(chain){
            uint32_t s=root[a[3]],idx=a[4];
            if(idx>=bound||!has_constant[idx]||constant[idx]>=(in[struct_at[s]]>>16)-2)goto unchanged;
            used[struct_at[s]+2+constant[idx]]=1;
        }
        for(uint32_t j=1;j<n;j++)if(nvn_unused_distance_id_operand(a,n,j)&&a[j]<bound&&root[a[j]]&&!(chain&&j==3))goto unchanged;
    }
    for(uint32_t s=1;s<bound;s++)if(candidate[s]){
        uint32_t kept=0,n=in[struct_at[s]]>>16;
        for(uint32_t j=2;j<n;j++){
            size_t pos=struct_at[s]+j;
            if(kind[pos]&&!used[pos]){new_member[pos]=UINT32_MAX;removed|=kind[pos];}
            else {new_member[pos]=kept++;retained|=kind[pos];}
        }
        if(!kept)goto unchanged;
    }
    if(!removed)goto unchanged;
    for(size_t at=5;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op==71&&n==4&&a[2]==11){if(a[3]==3)retained|=1;if(a[3]==4)retained|=2;}
        if((op==65||op==66)&&n>=5&&a[3]<bound&&root[a[3]]){
            uint32_t s=root[a[3]],idx=a[4],mapped=new_member[struct_at[s]+2+constant[idx]];
            if(mapped==constant[idx])continue;
            if(mapped==UINT32_MAX){failure="unused distance active member accounting";goto fail;}
            size_t extra=0;
            for(;extra<extra_count;extra++)if(extra_type[extra]==constant_type[idx]&&extra_value[extra]==mapped)break;
            if(extra==extra_count){extra_type[extra]=constant_type[idx];extra_value[extra]=mapped;extra_count++;}
            if(extra>UINT32_MAX-bound){failure="unused distance ID overflow";goto fail;}
            rewrite[at+4]=bound+(uint32_t)extra;
        }
    }
    failure="unused distance output allocation";
    out=malloc((words+extra_count*4)*4);if(!out)goto fail;
    memcpy(out,in,20);out[3]=bound+(uint32_t)extra_count;size_t dst=5;
    for(size_t at=5;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(at==first_function)for(size_t j=0;j<extra_count;j++){
            out[dst++]=4u<<16|43;out[dst++]=extra_type[j];out[dst++]=bound+(uint32_t)j;out[dst++]=extra_value[j];
        }
        if(op==17&&n==2&&((a[1]==32&&!(retained&1))||(a[1]==33&&!(retained&2))))continue;
        if(op==30&&candidate[a[1]]){
            size_t start=dst;out[dst++]=0;out[dst++]=a[1];
            for(uint32_t j=2;j<n;j++)if(new_member[at+j]!=UINT32_MAX)out[dst++]=a[j];
            out[start]=(uint32_t)(dst-start)<<16|30;
        }else if((op==6||op==72)&&n>=3&&a[1]<bound&&candidate[a[1]]){
            failure="unused distance member metadata bounds";
            if(a[2]>=(in[struct_at[a[1]]]>>16)-2)goto fail;
            uint32_t mapped=new_member[struct_at[a[1]]+2+a[2]];
            if(mapped==UINT32_MAX)continue;
            memcpy(out+dst,a,n*4);out[dst+2]=mapped;dst+=n;
        }else{
            memcpy(out+dst,a,n*4);
            if((op==65||op==66)&&n>=5&&rewrite[at+4])out[dst+4]=rewrite[at+4];
            dst+=n;
        }
    }
    *out_words=dst;*removed_mask=removed;goto done;
unchanged:
    out=malloc(words*4);failure="unused distance unchanged allocation";
    if(!out)goto fail;
    memcpy(out,in,words*4);*out_words=words;*removed_mask=0;goto done;
fail:
    free(out);out=NULL;if(error)*error=failure;
done:
    free(struct_at);free(pointer_to);free(root);free(constant);free(constant_type);free(int32);free(has_constant);
    free(candidate);free(seen_root);free(kind);free(used);free(new_member);free(rewrite);free(extra_type);free(extra_value);
    return out;
}
#endif
