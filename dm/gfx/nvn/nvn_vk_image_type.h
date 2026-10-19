/* Reflect descriptor image types from otherwise valid SPIR-V. This does not
 * infer a view from texture storage: Dim/Depth/Arrayed/MS/Sampled are preserved
 * exactly. Unsupported descriptor arrays, aliases and decoration groups fail
 * conservatively. An image-free module succeeds with zero records. */
#ifndef NVN_VK_IMAGE_TYPE_H
#define NVN_VK_IMAGE_TYPE_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define NVN_VK_IMAGE_TYPE_MAX 64u

typedef struct {
    uint32_t variable_id, image_type_id, sampled_image_type_id;
    uint32_t descriptor_set, binding;
    uint32_t sampled_type_id, dim, depth, arrayed, multisampled, sampled;
    uint32_t image_format;
    /* UINT32_MAX means the optional OpTypeImage Access Qualifier is absent. */
    uint32_t access_qualifier;
} NvnVkImageType;

typedef struct {
    uint32_t at, binding, descriptor_set;
    uint8_t has_binding, has_set;
} NvnVkImageTypeFact;

/* Returns 1 on success, including no images; returns 0 with count=0 and a
 * static error string on failure. records must have room for 64 entries.
 * No module bytes are changed; partial records must be ignored on failure. */
static int nvn_vk_image_types(const uint32_t *words,size_t word_count,
        NvnVkImageType records[NVN_VK_IMAGE_TYPE_MAX],size_t *record_count,
        const char **error) {
    if(record_count)*record_count=0;
    if(error)*error=NULL;
    const char *failure="image type reflection arguments";
    NvnVkImageTypeFact *facts=NULL;
    if(!words||!records||!record_count||word_count<5)goto fail;
    failure="image type reflection module header";
    if(words[0]!=0x07230203u||!words[3]||words[3]>1000000u||
            word_count>UINT32_MAX)goto fail;
    uint32_t bound=words[3];int grouped_decorations=0;
    failure="image type reflection allocation";
    facts=calloc(bound,sizeof *facts);if(!facts)goto fail;
    for(size_t at=5;at<word_count;){
        const uint32_t *a=words+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        failure="image type reflection instruction bounds";
        if(!n||n>word_count-at)goto fail;
        if(op==25||op==26||op==27||op==28||op==29||op==32||op==59){
            failure="image type reflection consumed declaration shape";
            if((op==25&&(n!=9&&n!=10))||(op==26&&n!=2)||
               (op==27&&n!=3)||(op==28&&n!=4)||(op==29&&n!=3)||
               (op==32&&n!=4)||(op==59&&(n!=4&&n!=5)))goto fail;
            uint32_t id=op==59?a[2]:a[1];
            failure="image type reflection declaration identity";
            if(!id||id>=bound||facts[id].at)goto fail;
            facts[id].at=(uint32_t)at;
        }
        if(op==71){
            failure="image type reflection decoration shape";
            if(n<3||!a[1]||a[1]>=bound)goto fail;
            if(a[2]==33||a[2]==34){
                if(n!=4)goto fail;
                NvnVkImageTypeFact *f=facts+a[1];
                failure="image type reflection duplicate descriptor decoration";
                if(a[2]==33){if(f->has_binding)goto fail;f->binding=a[3];f->has_binding=1;}
                else{if(f->has_set)goto fail;f->descriptor_set=a[3];f->has_set=1;}
            }
        }
        if(op==73||op==74||op==75)grouped_decorations=1;
        /* OpDecorateId cannot safely stand in for literal Binding/Set facts. */
        if(op==332){
            failure="image type reflection ID decoration shape";
            if(n<3)goto fail;
            if(a[2]==33||a[2]==34)grouped_decorations=1;
        }
        at+=n;
    }
    size_t count=0;
    for(size_t at=5;at<word_count;at+=words[at]>>16){
        const uint32_t *a=words+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op!=59||a[3]!=0)continue; /* UniformConstant only. */
        failure="image type reflection UniformConstant pointer";
        if(n!=4||!a[1]||a[1]>=bound||!facts[a[1]].at)goto fail;
        const uint32_t *pointer=words+facts[a[1]].at;
        if((pointer[0]&65535u)!=32||pointer[2]!=0)goto fail;
        uint32_t target_id=pointer[3];
        if(!target_id||target_id>=bound||!facts[target_id].at)goto fail;
        const uint32_t *target=words+facts[target_id].at;
        uint32_t target_op=target[0]&65535u,sampled_image_id=0;
        if(target_op==26)continue; /* A separate sampler is known, not an image. */
        failure="image type reflection descriptor arrays unsupported";
        if(target_op==28||target_op==29)goto fail;
        if(target_op==27){
            sampled_image_id=target_id;target_id=target[2];
            failure="image type reflection sampled image target";
            if(!target_id||target_id>=bound||!facts[target_id].at)goto fail;
            target=words+facts[target_id].at;target_op=target[0]&65535u;
        }
        failure="image type reflection unknown UniformConstant target";
        if(target_op!=25)goto fail;
        failure="image type reflection grouped descriptor decorations unsupported";
        if(grouped_decorations)goto fail;
        failure="image type reflection missing Binding or DescriptorSet";
        NvnVkImageTypeFact *var=facts+a[2];
        if(!var->has_binding||!var->has_set)goto fail;
        failure="image type reflection image operand range";
        if(!target[2]||target[2]>=bound||target[3]>6||target[4]>2||
                target[5]>1||target[6]>1||target[7]>2||
                ((target[0]>>16)==10&&target[9]>2))goto fail;
        failure="image type reflection too many images";
        if(count>=NVN_VK_IMAGE_TYPE_MAX)goto fail;
        failure="image type reflection duplicate set/binding";
        for(size_t j=0;j<count;j++)if(records[j].descriptor_set==var->descriptor_set&&
                records[j].binding==var->binding)goto fail;
        records[count++]=(NvnVkImageType){
            .variable_id=a[2],.image_type_id=target_id,.sampled_image_type_id=sampled_image_id,
            .descriptor_set=var->descriptor_set,.binding=var->binding,
            .sampled_type_id=target[2],.dim=target[3],.depth=target[4],
            .arrayed=target[5],.multisampled=target[6],.sampled=target[7],
            .image_format=target[8],
            .access_qualifier=(target[0]>>16)==10?target[9]:UINT32_MAX};
    }
    if(count){
        /* Vulkan descriptor pointers are normally consumed directly by Load.
         * Do not attempt to follow pointer-valued copies, access chains, phi,
         * select, parameters or extension instructions in function bodies.
         * All typed results put Result Type first; a conservative false reject
         * is preferable to assigning a descriptor view to an unresolved alias. */
        int in_function=0;
        for(size_t at=5;at<word_count;at+=words[at]>>16){
            const uint32_t *a=words+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
            if(op==54)in_function=1;
            if(n>=3&&a[1]<bound&&facts[a[1]].at&&
                    (in_function||(op>=41&&op<=52))&&op!=59){
                const uint32_t *type=words+facts[a[1]].at;
                failure="image type reflection descriptor pointer alias unsupported";
                if((type[0]&65535u)==32&&type[2]==0)goto fail;
            }
            /* Also reject passing a descriptor pointer to a function whose
             * result is not a pointer. Opaque image values loaded from the
             * descriptor remain supported. */
            if(op==57)for(uint32_t j=4;j<n;j++){
                uint32_t id=a[j];if(!id||id>=bound||!facts[id].at)continue;
                const uint32_t *var=words+facts[id].at;
                failure="image type reflection descriptor pointer argument unsupported";
                if((var[0]&65535u)==59&&var[3]==0)goto fail;
            }
            if(op==56)in_function=0;
        }
    }
    free(facts);*record_count=count;return 1;
fail:
    free(facts);if(error)*error=failure;return 0;
}
#endif
