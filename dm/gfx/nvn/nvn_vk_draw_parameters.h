/* Vulkan draw parameters as exact per-pipeline specialization values. This
 * optional path avoids broken mobile-driver BuiltIn imports while preserving
 * BaseVertex, BaseInstance and DrawIndex from the actual draw arguments.
 * Input modules must already have passed spirv-val; this transform additionally
 * checks every shape it changes and refuses unsupported pointer escapes. */
#ifndef NVN_VK_DRAW_PARAMETERS_H
#define NVN_VK_DRAW_PARAMETERS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define NVN_DRAW_SPEC_BASE 0x7fff0000u

/* Return the first interface operand, or zero for an unterminated entry name. */
static size_t nvn_draw_interfaces(const uint32_t *a,uint32_t n) {
    if(n<4)return 0;
    for(size_t j=3;j<n;j++){
        uint32_t w=a[j];
        if(!(w&0xffu)||!(w&0xff00u)||!(w&0xff0000u)||!(w&0xff000000u))return j+1;
    }
    return 0;
}
/* SPIR-V literals can have the same numeric value as a removed variable ID.
 * Only inspect ID positions. Unknown instructions remain conservative (all
 * words inspected), rather than silently accepting an unknown pointer use. */
static int nvn_draw_id_operand(const uint32_t *a,uint32_t n,uint32_t j) {
    uint32_t op=a[0]&65535u;
    if(op==8)return j==1; /* OpLine: file ID, line and column literals. */
    if(op==12)return j!=4; /* OpExtInst instruction number. */
    if(op==54)return j!=3; /* FunctionControl. */
    if(op==59)return j!=3; /* StorageClass. */
    if(op==68)return j!=4; /* ArrayLength member index. */
    if(op==79)return j<5; /* VectorShuffle component literals. */
    if(op==81)return j<4; /* CompositeExtract indices. */
    if(op==82)return j<5; /* CompositeInsert indices. */
    if(op==123)return j!=4; /* GenericCastToPtrExplicit storage class. */
    if(op==246)return j<3; /* Loop control and dependent literals. */
    if(op==247)return j<2; /* Selection control. */
    if(op==250)return j<4; /* Branch weights. */
    if(op==251)return j<3||!(j&1); /* 32-bit switch literal/label pairs. */
    if(op==256||op==257)return j==1; /* Lifetime size literal. */
    if((op>=264&&op<=271)||op==342||(op>=349&&op<=364))return j!=4;
    uint32_t image_mask=0;
    if(op==87||op==88||op==91||op==92||op==95||op==98||
       op==305||op==306||op==309||op==310||op==313||op==320)image_mask=5;
    if(op==89||op==90||op==93||op==94||op==96||op==97||
       op==307||op==308||op==311||op==312||op==314||op==315)image_mask=6;
    if(op==99)image_mask=4;
    if(image_mask&&j==image_mask)return 0;
    /* MemoryAccess: its bitmask and Aligned byte count are literals. Scope
     * operands and alias-list IDs are still inspected. Two masks are allowed
     * on CopyMemory; walk each mask and all its operands explicitly. */
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
static uint32_t *nvn_vk_draw_parameters(const uint32_t *in,size_t words,size_t *out_words,uint32_t *mask) {
    if(!out_words||!mask)return NULL;
    *out_words=0;*mask=0;
    if(!in||words<5||words>SIZE_MAX/4-24||in[0]!=0x07230203||!in[3]||in[3]>1000000)return NULL;
    uint32_t bound=in[3],types[3]={0},used=0;
    uint8_t *field=calloc(bound,1),*variable=calloc(bound,1),*int32=calloc(bound,1);
    uint32_t *pointee=calloc(bound,4),*variable_type=calloc(bound,4),*value_type=calloc(bound,4),*out=NULL;
    size_t first_type=words,first_function=words;int found=0;
    if(!field||!variable||!int32||!pointee||!variable_type||!value_type)goto done;
    for(size_t at=5;at<words;){
        uint32_t n=in[at]>>16,op=in[at]&65535;const uint32_t *a=in+at;
        if(!n||n>words-at)goto done;
        if(op>=19&&op<=39&&first_type==words)first_type=at;
        if(op==54&&first_function==words)first_function=at;
        if(op==15){size_t interfaces=nvn_draw_interfaces(a,n);if(!interfaces)goto done;
            for(size_t j=interfaces;j<n;j++)if(!a[j]||a[j]>=bound)goto done;}
        if(op==71&&n==4&&a[2]==11&&a[3]>=4424&&a[3]<=4426){
            if(!a[1]||a[1]>=bound||(field[a[1]]&&field[a[1]]!=a[3]-4423))goto done;
            field[a[1]]=(uint8_t)(a[3]-4423);found=1;
        }
        if(op==71&&n==4&&a[2]==1&&a[3]>=NVN_DRAW_SPEC_BASE&&a[3]<NVN_DRAW_SPEC_BASE+3)goto done;
        if(op==21&&n==4&&a[1]<bound&&a[2]==32&&a[3]<=1)int32[a[1]]=1;
        if(op==32&&n==4&&a[1]<bound&&a[2]==1&&a[3]<bound)pointee[a[1]]=a[3];
        at+=n;
    }
    if(!found){out=malloc(words*4);if(out){memcpy(out,in,words*4);*out_words=words;}goto done;}
    if(first_type==words||first_function==words||first_type>=first_function)goto done;
    /* Establish pointer types and loaded integer types independently of order. */
    for(size_t at=5;at<words;){
        uint32_t n=in[at]>>16,op=in[at]&65535;const uint32_t *a=in+at;
        if(op==59&&n>=4&&a[2]<bound&&field[a[2]]){
            if(n!=4||at>=first_function||a[3]!=1||a[1]>=bound||!pointee[a[1]]||!int32[pointee[a[1]]]||variable[a[2]])goto done;
            variable[a[2]]=1;variable_type[a[2]]=pointee[a[1]];
        }
        /* A switch selector must be int32 for literal/label parsing below.
         * All valid scalar int32-producing instructions start type,result. */
        if(n>=3&&a[1]<bound&&int32[a[1]]&&a[2]<bound)value_type[a[2]]=a[1];
        at+=n;
    }
    for(uint32_t id=1;id<bound;id++)if(field[id]&&!variable[id])goto done;
    for(size_t at=5;at<words;){
        uint32_t n=in[at]>>16,op=in[at]&65535;const uint32_t *a=in+at;
        if(at>=first_function){
            if(op==251&&(n<3||a[1]>=bound||!value_type[a[1]]))goto done;
            if(op==61&&n>=4&&a[3]<bound&&field[a[3]]){
                if(n!=4||a[1]!=variable_type[a[3]]||!a[2]||a[2]>=bound)goto done;
                unsigned f=field[a[3]]-1;
                if(types[f]&&types[f]!=a[1])goto done;
                types[f]=a[1];used|=1u<<f;
            }else for(uint32_t j=1;j<n;j++)if(nvn_draw_id_operand(a,n,j)&&a[j]<bound&&field[a[j]])goto done;
        }else if(op!=15&&op!=5&&op!=71&&op!=59){
            /* Global types/constants cannot legally consume Input pointers.
             * Group decorations are deliberately unsupported for these IDs. */
            if(op==74||op==75||op==332)for(uint32_t j=1;j<n;j++)if(a[j]<bound&&field[a[j]])goto done;
        }
        at+=n;
    }
    /* At most three annotations/constants; all replacements are no larger. */
    out=malloc((words+24)*4);if(!out)goto done;memcpy(out,in,20);out[3]=bound+3;size_t dst=5;
    for(size_t at=5;at<words;){
        uint32_t n=in[at]>>16,op=in[at]&65535;const uint32_t *a=in+at;
        if(at==first_type)for(unsigned f=0;f<3;f++)if(types[f]){out[dst++]=4u<<16|71;out[dst++]=bound+f;out[dst++]=1;out[dst++]=NVN_DRAW_SPEC_BASE+f;}
        if(at==first_function)for(unsigned f=0;f<3;f++)if(types[f]){out[dst++]=4u<<16|50;out[dst++]=types[f];out[dst++]=bound+f;out[dst++]=0;}
        int drop=(op==17&&n==2&&a[1]==4427)||((op==5||op==71)&&n>=2&&a[1]<bound&&field[a[1]])||(op==59&&n>=4&&a[2]<bound&&field[a[2]]);
        if(op==15){
            size_t interfaces=nvn_draw_interfaces(a,n),start=dst;
            memcpy(out+dst,a,interfaces*4);dst+=interfaces;
            for(size_t j=interfaces;j<n;j++)if(!field[a[j]])out[dst++]=a[j];
            out[start]=(uint32_t)(dst-start)<<16|15;
        }else if(op==61&&n==4&&a[3]<bound&&field[a[3]]){
            out[dst++]=4u<<16|83;out[dst++]=a[1];out[dst++]=a[2];out[dst++]=bound+field[a[3]]-1;
        }else if(!drop){memcpy(out+dst,a,n*4);dst+=n;}
        at+=n;
    }
    *out_words=dst;*mask=used;
done:
    free(field);free(variable);free(int32);free(pointee);free(variable_type);free(value_type);return out;
}
#endif
