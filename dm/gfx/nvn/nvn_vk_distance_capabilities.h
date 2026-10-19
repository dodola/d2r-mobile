/* Restore mandatory capabilities for declared ClipDistance/CullDistance
 * BuiltIns. Some translator output omits them and spirv-val versions accept
 * the omission. Preserve all shader operations and report the real device
 * features required by the resulting module. Input must otherwise be valid
 * SPIR-V; this helper checks the bounds and shapes it consumes. */
#ifndef NVN_VK_DISTANCE_CAPABILITIES_H
#define NVN_VK_DISTANCE_CAPABILITIES_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define NVN_DISTANCE_CLIP 1u
#define NVN_DISTANCE_CULL 2u

static uint32_t *nvn_vk_distance_capabilities(const uint32_t *input,
        size_t word_count,size_t *out_count,uint32_t *required_mask,
        const char **error) {
    if(out_count)*out_count=0;
    if(required_mask)*required_mask=0;
    if(error)*error=NULL;
    const char *failure="distance capability arguments";
    uint32_t *members=NULL,*output=NULL;
    if(!input||!out_count||!required_mask||word_count<5)goto fail;
    failure="distance capability module header";
    if(input[0]!=0x07230203u||!input[3]||input[3]>1000000u||
            word_count>(SIZE_MAX/sizeof(uint32_t))-4)goto fail;
    members=calloc(input[3],sizeof *members);
    failure="distance capability allocation";
    if(!members)goto fail;
    uint32_t present=0,required=0;
    for(size_t at=5;at<word_count;){
        uint32_t n=input[at]>>16,op=input[at]&65535u;
        failure="distance capability instruction bounds";
        if(!n||n>word_count-at)goto fail;
        const uint32_t *a=input+at;
        if(op==17){
            failure="distance capability declaration shape";
            if(n!=2)goto fail;
            if(a[1]==32)present|=NVN_DISTANCE_CLIP;
            if(a[1]==33)present|=NVN_DISTANCE_CULL;
        }
        if(op==30){
            failure="distance capability struct bounds";
            if(n<2||!a[1]||a[1]>=input[3]||members[a[1]])goto fail;
            members[a[1]]=n-1; /* member count plus one, including empty structs */
        }
        at+=n;
    }
    for(size_t at=5;at<word_count;at+=input[at]>>16){
        const uint32_t *a=input+at;
        uint32_t n=a[0]>>16,op=a[0]&65535u,builtin=UINT32_MAX;
        if(op==71){
            failure="distance capability decoration shape";
            if(n<3)goto fail;
            if(a[2]==11){
                if(n!=4)goto fail;
                builtin=a[3];
                if((builtin==3||builtin==4)&&(!a[1]||a[1]>=input[3]))goto fail;
            }
        }else if(op==72){
            failure="distance capability member decoration shape";
            if(n<4)goto fail;
            if(a[3]==11){
                if(n!=5)goto fail;
                builtin=a[4];
                if(builtin==3||builtin==4){
                    failure="distance capability member bounds";
                    if(!a[1]||a[1]>=input[3]||!members[a[1]]||a[2]>=members[a[1]]-1)goto fail;
                }
            }
        }
        if(builtin==3)required|=NVN_DISTANCE_CLIP;
        if(builtin==4)required|=NVN_DISTANCE_CULL;
    }
    required|=present;
    uint32_t missing=required&~present;
    size_t extra=((missing&NVN_DISTANCE_CLIP)?2u:0u)+((missing&NVN_DISTANCE_CULL)?2u:0u);
    failure="distance capability output allocation";
    output=malloc((word_count+extra)*sizeof *output);
    if(!output)goto fail;
    memcpy(output,input,5*sizeof *output);
    size_t cursor=5;
    if(missing&NVN_DISTANCE_CLIP){output[cursor++]=2u<<16|17u;output[cursor++]=32;}
    if(missing&NVN_DISTANCE_CULL){output[cursor++]=2u<<16|17u;output[cursor++]=33;}
    memcpy(output+cursor,input+5,(word_count-5)*sizeof *output);
    free(members);*out_count=word_count+extra;*required_mask=required;
    return output;
fail:
    free(members);free(output);
    if(error)*error=failure;
    return NULL;
}
#endif
