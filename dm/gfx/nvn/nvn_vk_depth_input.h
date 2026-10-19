/* Explicit SPIR-V syntax variant builder, NOT a sampling-equivalence proof.
 * The caller must independently authorize the replacement, prove coordinates,
 * filtering/precision and source visibility/order semantics, and supply a
 * matching Vulkan input-attachment descriptor/renderpass. Never enables itself.
 * Input must otherwise be valid SPIR-V; consumed declarations/uses are checked
 * conservatively. Returned malloc-owned module must be validated before use.
 */
#ifndef NVN_VK_DEPTH_INPUT_H
#define NVN_VK_DEPTH_INPUT_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t descriptor_set,binding,input_attachment_index,variable_id;
    uint32_t source_pointer_type,source_sampled_image_type,source_image_type;
    uint32_t load_count,sample_count,new_image_type,new_pointer_type;
    uint32_t new_id_count,added_input_attachment_capability;
} NvnVkDepthInputInfo;

typedef struct {
    uint32_t at,binding,set;
    uint8_t has_binding,has_set,loaded;
} NvnVkDepthInputFact;

static uint32_t *nvn_vk_depth_input_variant(const uint32_t *in,size_t words,
        uint32_t descriptor_set,uint32_t binding,uint32_t attachment_index,
        size_t *out_words,NvnVkDepthInputInfo *info,const char **error) {
    uint32_t *out=NULL;NvnVkDepthInputFact *fact=NULL;
    const char *failure="depth input variant arguments";
    uint32_t bound=0,var=0,pointer_id=0,sampled_id=0,image_id=0,float_id=0;
    uint32_t entry=0,entries=0,memory_models=0,capability=0,loads=0,samples=0;
    size_t first_type=words,variable_at=words;int in_function=0,entry_function=0;
    if(out_words)*out_words=0;
    if(info)memset(info,0,sizeof *info);
    if(error)*error=NULL;
    if(!in||!out_words||!info||words<5)goto fail;
    failure="depth input variant module header";
    if(in[0]!=0x07230203u||in[1]<0x10000u||in[1]>0x10600u||in[4]||
       !in[3]||in[3]>999994u||words>UINT32_MAX||words>SIZE_MAX/4-40)goto fail;
    bound=in[3];fact=calloc(bound,sizeof *fact);
    failure="depth input variant allocation";if(!fact)goto fail;
    for(size_t at=5;at<words;){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        failure="depth input variant instruction bounds";
        if(!n||n>words-at)goto fail;
        if(op==73||op==74||op==75||op==332){failure="depth input variant grouped/ID decorations unsupported";goto fail;}
        if(op==17){if(n!=2)goto fail;if(a[1]==40)capability=1;}
        if(op==14){failure="depth input variant memory model";if(n!=3||a[1]!=0||a[2]!=1)goto fail;memory_models++;}
        if(op==15){
            failure="depth input variant requires one Fragment entry point";
            if(n<4||a[1]!=4||!a[2]||a[2]>=bound)goto fail;
            int terminated=0;for(uint32_t j=3;j<n&&!terminated;j++)
                for(unsigned b=0;b<4;b++)if(!((a[j]>>(8*b))&255u)){terminated=1;break;}
            if(!terminated)goto fail;
            entry=a[2];entries++;
        }
        if(op==16){failure="depth input variant execution mode";if(n<3)goto fail;
            if(a[2]==9){failure="depth input variant EarlyFragmentTests unsupported";goto fail;}}
        if(op==331){failure="depth input variant execution mode IDs unsupported";goto fail;}
        if(op>=19&&op<=39){
            if(first_type==words)first_type=at;
            failure="depth input variant type identity";
            if(n<2||!a[1]||a[1]>=bound||fact[a[1]].at)goto fail;
            fact[a[1]].at=(uint32_t)at;
        }
        if(op==59){
            failure="depth input variant variable identity";
            if((n!=4&&n!=5)||!a[2]||a[2]>=bound||fact[a[2]].at)goto fail;
            fact[a[2]].at=(uint32_t)at;
        }
        if(op==71){
            failure="depth input variant decoration shape";
            if(n<3||!a[1]||a[1]>=bound)goto fail;
            if(a[2]==33||a[2]==34){
                if(n!=4)goto fail;
                NvnVkDepthInputFact *f=fact+a[1];
                failure="depth input variant duplicate descriptor decoration";
                if(a[2]==33){if(f->has_binding)goto fail;f->has_binding=1;f->binding=a[3];}
                else{if(f->has_set)goto fail;f->has_set=1;f->set=a[3];}
            }
        }
        if(op==54){if(n!=5)goto fail;in_function=1;if(a[2]==entry)entry_function=1;}
        if(op==56){if(n!=1||!in_function)goto fail;in_function=0;}
        at+=n;
    }
    failure="depth input variant Fragment module structure";
    if(entries!=1||memory_models!=1||!entry_function||in_function||first_type==words)goto fail;
    for(uint32_t i=1;i<bound;i++)if(fact[i].has_binding&&fact[i].has_set&&
            fact[i].binding==binding&&fact[i].set==descriptor_set){
        failure="depth input variant duplicate target descriptor";if(var)goto fail;var=i;
    }
    failure="depth input variant target descriptor missing";if(!var||!fact[var].at)goto fail;
    variable_at=fact[var].at;const uint32_t *v=in+variable_at;
    failure="depth input variant target variable shape";
    if((v[0]&65535u)!=59||(v[0]>>16)!=4||v[3]!=0||!v[1]||v[1]>=bound||!fact[v[1]].at)goto fail;
    pointer_id=v[1];const uint32_t *p=in+fact[pointer_id].at;
    failure="depth input variant sampler2D pointer";
    if((p[0]&65535u)!=32||(p[0]>>16)!=4||p[2]!=0||!p[3]||p[3]>=bound||!fact[p[3]].at)goto fail;
    sampled_id=p[3];const uint32_t *s=in+fact[sampled_id].at;
    if((s[0]&65535u)!=27||(s[0]>>16)!=3||!s[2]||s[2]>=bound||!fact[s[2]].at)goto fail;
    image_id=s[2];const uint32_t *im=in+fact[image_id].at;
    failure="depth input variant requires ordinary float 2D nonarray nonMS sampled image";
    if((im[0]&65535u)!=25||(im[0]>>16)!=9||im[3]!=1||im[4]>1||im[5]||im[6]||im[7]!=1||im[8]||
       !im[2]||im[2]>=bound||!fact[im[2]].at)goto fail;
    float_id=im[2];const uint32_t *fl=in+fact[float_id].at;
    if((fl[0]&65535u)!=22||(fl[0]>>16)!=3||fl[2]!=32)goto fail;
    /* Discover only direct opaque-value loads; pointer/value aliases are not
     * followed. Memory operands on a descriptor Load are unsupported. */
    for(size_t at=5;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op==61&&n>=4&&a[3]==var){
            failure="depth input variant direct load shape/identity";
            if(n!=4||a[1]!=sampled_id||!a[2]||a[2]>=bound||fact[a[2]].at)goto fail;
            fact[a[2]].at=(uint32_t)at;fact[a[2]].loaded=1;loads++;
        }
    }
    failure="depth input variant target has no direct loads";if(!loads)goto fail;
    for(size_t at=5;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op==87&&n>=4&&a[3]<bound&&fact[a[3]].loaded){
            /* Optional ImageOperands None is the same supported empty mask;
             * every bias/offset/LOD/other operand is explicitly rejected. */
            failure="depth input variant sample operands unsupported";
            if(n!=5&&(n!=6||a[5]))goto fail;
            failure="depth input variant sample result must be float vec4";
            if(!a[1]||a[1]>=bound||!fact[a[1]].at)goto fail;
            const uint32_t *rt=in+fact[a[1]].at;
            if((rt[0]&65535u)!=23||(rt[0]>>16)!=4||rt[2]!=float_id||rt[3]!=4)goto fail;
            if(!a[2]||a[2]>=bound||!a[4]||a[4]>=bound)goto fail;
            samples++;
        }
        if(op==5){failure="depth input variant debug name shape";if(n<3)goto fail;continue;}
        if(op==6||op==7||op==3||op==4||op==10||op==11)continue; /* Debug/source strings. */
        if(op==15)continue; /* Existing interface ID is retained, if present. */
        if(op==71&&a[1]==var){
            failure="depth input variant target decoration unsupported";
            if(n!=4||(a[2]!=33&&a[2]!=34))goto fail;
            continue;
        }
        /* Decoration enumerants/literals (e.g. Location=30) are not IDs.
         * Decorations on the changed opaque load value remain unsupported. */
        if(op==71){
            failure="depth input variant decorated opaque value unsupported";
            if(fact[a[1]].loaded)goto fail;
            continue;
        }
        for(uint32_t j=1;j<n;j++){
            if((op==43&&j>=3)||(op==12&&j==4))continue;
            if(a[j]==var){
                failure="depth input variant descriptor pointer alias/use unsupported";
                if(!((op==59&&j==2&&at==variable_at)||(op==61&&n==4&&j==3)))goto fail;
            }
            if(a[j]<bound&&fact[a[j]].loaded){
                failure="depth input variant loaded image alias/use unsupported";
                if(!((op==61&&j==2&&fact[a[j]].at==at)||(op==87&&j==3)))goto fail;
            }
        }
    }
    failure="depth input variant target has no supported samples";if(!samples)goto fail;
    /* At most six fresh IDs leave shared sampled-image/pointer types intact.
     * Reuse existing canonical scalar/vector/SubpassData types: SPIR-V forbids
     * duplicate non-aggregate type declarations. Late matching declarations
     * are conservatively rejected rather than reordered or forward-referenced.
     * SubpassData Depth=0 is the standard float subpassInput representation;
     * host view/renderpass selects the actual depth aspect. */
    uint32_t new_image=0,new_pointer=0,new_int=0,new_ivec=0,next_id=bound;
    int add_image=0,add_pointer=0,add_int=0,add_ivec=0;
    for(uint32_t id=1;id<bound;id++)if(fact[id].at){
        const uint32_t *a=in+fact[id].at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        int im_match=op==25&&n==9&&a[2]==float_id&&a[3]==6&&!a[4]&&!a[5]&&!a[6]&&a[7]==2&&!a[8];
        int int_match=op==21&&n==4&&a[2]==32&&a[3]==1;
        if(im_match||int_match){
            failure="depth input variant matching type declared after target";
            if(fact[id].at>=variable_at)goto fail;
            if(im_match)new_image=id;else new_int=id;
        }
    }
    if(!new_image){new_image=next_id++;add_image=1;}
    if(!new_int){new_int=next_id++;add_int=1;}
    for(uint32_t id=1;id<bound;id++)if(fact[id].at){
        const uint32_t *a=in+fact[id].at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        int ptr_match=op==32&&n==4&&a[2]==0&&a[3]==new_image;
        int vec_match=op==23&&n==4&&a[2]==new_int&&a[3]==2;
        if(ptr_match||vec_match){
            failure="depth input variant matching type declared after target";
            if(fact[id].at>=variable_at)goto fail;
            if(ptr_match)new_pointer=id;else new_ivec=id;
        }
    }
    if(!new_pointer){new_pointer=next_id++;add_pointer=1;}
    if(!new_ivec){new_ivec=next_id++;add_ivec=1;}
    uint32_t zero=next_id++,zero_coord=next_id++;
    size_t decl_words=9u*add_image+4u*(add_pointer+add_int+add_ivec)+9;
    size_t extra=decl_words+4+(capability?0:2);
    out=malloc((words+extra)*4);failure="depth input variant output allocation";if(!out)goto fail;
    memcpy(out,in,20);out[3]=next_id;size_t dst=5;
    if(!capability){out[dst++]=2u<<16|17;out[dst++]=40;}
    for(size_t at=5;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(at==first_type){out[dst++]=4u<<16|71;out[dst++]=var;out[dst++]=43;out[dst++]=attachment_index;}
        if(at==variable_at){
            if(add_image){const uint32_t d[]={9u<<16|25,new_image,float_id,6,0,0,0,2,0};memcpy(out+dst,d,sizeof d);dst+=9;}
            if(add_int){const uint32_t d[]={4u<<16|21,new_int,32,1};memcpy(out+dst,d,sizeof d);dst+=4;}
            if(add_pointer){const uint32_t d[]={4u<<16|32,new_pointer,0,new_image};memcpy(out+dst,d,sizeof d);dst+=4;}
            if(add_ivec){const uint32_t d[]={4u<<16|23,new_ivec,new_int,2};memcpy(out+dst,d,sizeof d);dst+=4;}
            const uint32_t d[]={4u<<16|43,new_int,zero,0,5u<<16|44,new_ivec,zero_coord,zero,zero};
            memcpy(out+dst,d,sizeof d);dst+=9;
        }
        memcpy(out+dst,a,n*4);
        if(at==variable_at)out[dst+1]=new_pointer;
        if(op==61&&n==4&&a[3]==var)out[dst+1]=new_image;
        if(op==87&&n>=4&&a[3]<bound&&fact[a[3]].loaded){out[dst]=n<<16|98;out[dst+4]=zero_coord;}
        dst+=n;
    }
    failure="depth input variant output accounting";if(dst!=words+extra)goto fail;
    *info=(NvnVkDepthInputInfo){.descriptor_set=descriptor_set,.binding=binding,
        .input_attachment_index=attachment_index,.variable_id=var,.source_pointer_type=pointer_id,
        .source_sampled_image_type=sampled_id,.source_image_type=image_id,.load_count=loads,
        .sample_count=samples,.new_image_type=new_image,.new_pointer_type=new_pointer,
        .new_id_count=next_id-bound,.added_input_attachment_capability=!capability};
    *out_words=dst;free(fact);return out;
fail:
    free(fact);free(out);if(error)*error=failure;return NULL;
}
typedef struct {
    uint32_t descriptor_set,binding,width,height;
    float threshold_x,threshold_y;
} NvnVkDepthInputGuard;
typedef struct {
    NvnVkDepthInputInfo transform;
    uint32_t descriptor_set,binding,flag_variable_id,fragcoord_variable_id;
    uint32_t coordinate_type_id,first_coordinate_id,guard_count;
} NvnVkDepthInputGuardInfo;

enum {
    NVN_DI_ZERO,NVN_DI_ONE,NVN_DI_EIGHT,NVN_DI_23,NVN_DI_31,NVN_DI_32,
    NVN_DI_115,NVN_DI_127,NVN_DI_126,NVN_DI_137,NVN_DI_149,NVN_DI_150,
    NVN_DI_MANTMASK,NVN_DI_HIDDEN,NVN_DI_M,NVN_DI_FCLIMIT,NVN_DI_CONST_COUNT
};
/* Core 32-bit vector integer instructions only. Invalid inputs still execute
 * defined shifts (all counts masked to 0..31), then fail the validity mask. */
static size_t nvn_vk_depth_input_exact_guard_emit(uint32_t *out,size_t capacity,
        uint32_t id,uint32_t vec2,uint32_t vec4,uint32_t uint2,uint32_t bool2,
        uint32_t bool_id,uint32_t uint_id,uint32_t product_type,uint32_t frag,
        uint32_t uv,uint32_t flag,uint32_t uint_ptr,uint32_t zero,uint32_t one,
        const uint32_t c[NVN_DI_CONST_COUNT],int calibrated_ties) {
    enum { FC4,FC2,UVBITS,UVEXP,UVEXPLO,UVEXPHI,UVVALID,UVFRAC,UVMANT,
        PRODUCT,PL,PH,FCBITS,FCEXP,FCEXPLO,FCEXPHI,FCEXPVALID,FCFRAC,FCMANT,
        FCSHIFTRAW,FCSHIFT,FCHALF,FCONESHIFT,FCMASK,FCREMAINDER,FCEXACT,
        FCODD,FCODDOK,FCLIMITOK,FCVALIDA,FCVALIDB,FCVALID,F,LEFT,RIGHT,
        KRAW,K,HIGHCASE,COMPLEMENTRAW,COMPLEMENT,LEFTLORAW,LEFTLO,LEFTHILOW,
        LEFTHI,RIGHTLORAW,RIGHTLO,RIGHTHILOW,RIGHTHI,LOWERHI,LOWERHIEQUAL,
        LOWERLO,LOWERTIE,LOWERVALID,UPPERHI,UPPERHIEQUAL,UPPERLO,UPPERTIE,
        UPPERVALID,BOUNDSVALID,SOURCEVALID,PASSVEC,PASS,BIT,PTR,ATOMIC,COUNT };
    size_t dst=0;
#define NVN_DI_EMIT(...) do {const uint32_t z_[]={__VA_ARGS__};size_t n_=sizeof z_/4; \
    if(n_>capacity-dst){return 0;}memcpy(out+dst,z_,sizeof z_);dst+=n_;}while(0)
#define NVN_DI_BIN(op,type,r,a,b) NVN_DI_EMIT(5u<<16|(op),(type),id+(r),(a),(b))
    NVN_DI_EMIT(4u<<16|61,vec4,id+FC4,frag,7u<<16|79,vec2,id+FC2,id+FC4,id+FC4,0,1,
        4u<<16|124,uint2,id+UVBITS,uv);
    NVN_DI_BIN(194,uint2,UVEXP,id+UVBITS,c[NVN_DI_23]);
    NVN_DI_BIN(174,bool2,UVEXPLO,id+UVEXP,c[NVN_DI_115]);
    NVN_DI_BIN(178,bool2,UVEXPHI,id+UVEXP,c[NVN_DI_127]);
    NVN_DI_BIN(167,bool2,UVVALID,id+UVEXPLO,id+UVEXPHI);
    NVN_DI_BIN(199,uint2,UVFRAC,id+UVBITS,c[NVN_DI_MANTMASK]);
    NVN_DI_BIN(197,uint2,UVMANT,id+UVFRAC,c[NVN_DI_HIDDEN]);
    NVN_DI_BIN(151,product_type,PRODUCT,id+UVMANT,c[NVN_DI_M]);
    NVN_DI_EMIT(5u<<16|81,uint2,id+PL,id+PRODUCT,0,5u<<16|81,uint2,id+PH,id+PRODUCT,1,
        4u<<16|124,uint2,id+FCBITS,id+FC2);
    NVN_DI_BIN(194,uint2,FCEXP,id+FCBITS,c[NVN_DI_23]);
    NVN_DI_BIN(174,bool2,FCEXPLO,id+FCEXP,c[NVN_DI_126]);
    NVN_DI_BIN(178,bool2,FCEXPHI,id+FCEXP,c[NVN_DI_137]);
    NVN_DI_BIN(167,bool2,FCEXPVALID,id+FCEXPLO,id+FCEXPHI);
    NVN_DI_BIN(199,uint2,FCFRAC,id+FCBITS,c[NVN_DI_MANTMASK]);
    NVN_DI_BIN(197,uint2,FCMANT,id+FCFRAC,c[NVN_DI_HIDDEN]);
    NVN_DI_BIN(130,uint2,FCSHIFTRAW,c[NVN_DI_149],id+FCEXP);
    NVN_DI_BIN(199,uint2,FCSHIFT,id+FCSHIFTRAW,c[NVN_DI_31]);
    NVN_DI_BIN(194,uint2,FCHALF,id+FCMANT,id+FCSHIFT);
    NVN_DI_BIN(196,uint2,FCONESHIFT,c[NVN_DI_ONE],id+FCSHIFT);
    NVN_DI_BIN(130,uint2,FCMASK,id+FCONESHIFT,c[NVN_DI_ONE]);
    NVN_DI_BIN(199,uint2,FCREMAINDER,id+FCMANT,id+FCMASK);
    NVN_DI_BIN(170,bool2,FCEXACT,id+FCREMAINDER,c[NVN_DI_ZERO]);
    NVN_DI_BIN(199,uint2,FCODD,id+FCHALF,c[NVN_DI_ONE]);
    NVN_DI_BIN(170,bool2,FCODDOK,id+FCODD,c[NVN_DI_ONE]);
    NVN_DI_BIN(176,bool2,FCLIMITOK,id+FCHALF,c[NVN_DI_FCLIMIT]);
    NVN_DI_BIN(167,bool2,FCVALIDA,id+FCEXPVALID,id+FCEXACT);
    NVN_DI_BIN(167,bool2,FCVALIDB,id+FCODDOK,id+FCLIMITOK);
    NVN_DI_BIN(167,bool2,FCVALID,id+FCVALIDA,id+FCVALIDB);
    NVN_DI_BIN(196,uint2,F,id+FCHALF,c[NVN_DI_EIGHT]);
    NVN_DI_BIN(130,uint2,LEFT,id+F,c[NVN_DI_ONE]);
    NVN_DI_BIN(128,uint2,RIGHT,id+F,c[NVN_DI_ONE]);
    NVN_DI_BIN(130,uint2,KRAW,c[NVN_DI_150],id+UVEXP);
    NVN_DI_BIN(199,uint2,K,id+KRAW,c[NVN_DI_31]);
    NVN_DI_BIN(174,bool2,HIGHCASE,id+KRAW,c[NVN_DI_32]);
    NVN_DI_BIN(130,uint2,COMPLEMENTRAW,c[NVN_DI_32],id+KRAW);
    NVN_DI_BIN(199,uint2,COMPLEMENT,id+COMPLEMENTRAW,c[NVN_DI_31]);
    NVN_DI_BIN(196,uint2,LEFTLORAW,id+LEFT,id+K);
    NVN_DI_EMIT(6u<<16|169,uint2,id+LEFTLO,id+HIGHCASE,c[NVN_DI_ZERO],id+LEFTLORAW);
    NVN_DI_BIN(194,uint2,LEFTHILOW,id+LEFT,id+COMPLEMENT);
    NVN_DI_EMIT(6u<<16|169,uint2,id+LEFTHI,id+HIGHCASE,id+LEFTLORAW,id+LEFTHILOW);
    NVN_DI_BIN(196,uint2,RIGHTLORAW,id+RIGHT,id+K);
    NVN_DI_EMIT(6u<<16|169,uint2,id+RIGHTLO,id+HIGHCASE,c[NVN_DI_ZERO],id+RIGHTLORAW);
    NVN_DI_BIN(194,uint2,RIGHTHILOW,id+RIGHT,id+COMPLEMENT);
    NVN_DI_EMIT(6u<<16|169,uint2,id+RIGHTHI,id+HIGHCASE,id+RIGHTLORAW,id+RIGHTHILOW);
    NVN_DI_BIN(176,bool2,LOWERHI,id+LEFTHI,id+PH);
    NVN_DI_BIN(170,bool2,LOWERHIEQUAL,id+LEFTHI,id+PH);
    NVN_DI_BIN(calibrated_ties?178:176,bool2,LOWERLO,id+LEFTLO,id+PL);
    NVN_DI_BIN(167,bool2,LOWERTIE,id+LOWERHIEQUAL,id+LOWERLO);
    NVN_DI_BIN(166,bool2,LOWERVALID,id+LOWERHI,id+LOWERTIE);
    NVN_DI_BIN(176,bool2,UPPERHI,id+PH,id+RIGHTHI);
    NVN_DI_BIN(170,bool2,UPPERHIEQUAL,id+PH,id+RIGHTHI);
    NVN_DI_BIN(calibrated_ties?178:176,bool2,UPPERLO,id+PL,id+RIGHTLO);
    NVN_DI_BIN(167,bool2,UPPERTIE,id+UPPERHIEQUAL,id+UPPERLO);
    NVN_DI_BIN(166,bool2,UPPERVALID,id+UPPERHI,id+UPPERTIE);
    NVN_DI_BIN(167,bool2,BOUNDSVALID,id+LOWERVALID,id+UPPERVALID);
    NVN_DI_BIN(167,bool2,SOURCEVALID,id+UVVALID,id+FCVALID);
    NVN_DI_BIN(167,bool2,PASSVEC,id+BOUNDSVALID,id+SOURCEVALID);
    NVN_DI_EMIT(4u<<16|155,bool_id,id+PASS,id+PASSVEC,
        6u<<16|169,uint_id,id+BIT,id+PASS,zero,one,5u<<16|65,uint_ptr,id+PTR,flag,zero,
        7u<<16|241,uint_id,id+ATOMIC,id+PTR,one,zero,id+BIT);
#undef NVN_DI_BIN
#undef NVN_DI_EMIT
    (void)COUNT;return dst;
}

/* Optional observational safety flag, still NOT semantic authorization.
 * The host must enable FragmentStoresAndAtomics, clear one uint flag before
 * the submission, and fence/check it before publishing any guest writes/events.
 * Each replacement evaluates the ORIGINAL actual sample UV, with binary32
 * p=uv*extent, err=abs(p-FragCoord.xy), ulp=nextUp(abs(p))-abs(p), and
 * bound=nextUp(err+ulp). Every arithmetic step is NoContraction. Ordered tests
 * require finite p and err+ulp, and bound < threshold on each axis. nextUp is
 * implemented by incrementing positive IEEE754 bits, including zero/subnormal.
 * The caller supplies the strict snap half-step (e.g. 1/512), WITHOUT subtracting
 * another host margin, and independently proves the texture snap rule.
 * AtomicOr writes 1 on failure
 * and 0 on success, unconditionally, avoiding control-flow/derivative changes.
 * No discard, alpha or depth operation is added. Binding must be unused.
 */
static uint32_t *nvn_vk_depth_input_guarded_variant_impl(const uint32_t *in,size_t words,
        uint32_t descriptor_set,uint32_t binding,uint32_t attachment_index,
        const NvnVkDepthInputGuard *guard,size_t *out_words,
        NvnVkDepthInputGuardInfo *info,const char **error,int exact) {
    uint32_t *base=NULL,*out=NULL,*type_at=NULL,*result_type=NULL,*coord=NULL;
    uint8_t *loaded=NULL;size_t base_words=0;NvnVkDepthInputInfo basic;
    const char *failure="depth input guard arguments";
    uint32_t bound=0,float_id=0,vec2=0,vec4=0,frag=0,first_coord=0;
    uint32_t bool_id=0,bool2=0,uint_id=0,uint_ptr=0,uint2=0;
    size_t first_type=0,first_function=0;
    if(out_words)*out_words=0;
    if(info)memset(info,0,sizeof *info);
    if(error)*error=NULL;
    if(!in||!out_words||!info||!guard)goto fail;
    uint32_t tx,ty;memcpy(&tx,&guard->threshold_x,4);memcpy(&ty,&guard->threshold_y,4);
    failure="depth input guard extent/threshold";
    if(!guard->width||!guard->height||guard->width>16777216u||guard->height>16777216u||
       (tx&0x80000000u)||(ty&0x80000000u)||(tx&0x7fffffffu)==0||(ty&0x7fffffffu)==0||
       (tx&0x7f800000u)==0x7f800000u||(ty&0x7f800000u)==0x7f800000u)goto fail;
    failure="depth exact guard requires 1280x720 and strict 1/512 thresholds";
    if(exact&&(guard->width!=1280||guard->height!=720||tx!=0x3b000000u||ty!=0x3b000000u))goto fail;
    failure="depth input guard requires SPIR-V 1.3 or newer";
    if(words<5||in[1]<0x10300u)goto fail;
    base=nvn_vk_depth_input_variant(in,words,descriptor_set,binding,attachment_index,&base_words,&basic,&failure);
    if(!base)goto fail;
    bound=base[3];
    failure="depth input guard ID budget";
    uint32_t stride=exact?65u:26u;
    size_t global_words=exact?400u:140u,guard_words=exact?400u:160u;
    if((size_t)basic.sample_count*stride+(exact?80u:26u)>1000000u-bound||
       base_words>SIZE_MAX/4-global_words-(size_t)basic.sample_count*guard_words)goto fail;
    type_at=calloc(bound,4);result_type=calloc(bound,4);coord=calloc(bound,4);loaded=calloc(bound,1);
    failure="depth input guard allocation";if(!type_at||!result_type||!coord||!loaded)goto fail;
    for(size_t at=5;at<base_words;at+=base[at]>>16){
        const uint32_t *a=base+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op>=19&&op<=39){type_at[a[1]]=(uint32_t)at;if(!first_type)first_type=at;}
        if(op==54&&!first_function)first_function=at;
        if(op==71&&n==4&&a[2]==11&&a[3]==15){
            failure="depth input guard duplicate FragCoord";if(frag)goto fail;frag=a[1];
        }
        /* A separate descriptor may not already occupy the flag binding. */
        if(op==71&&n==4&&a[2]==34&&a[3]==guard->descriptor_set)result_type[a[1]]|=1;
        if(op==71&&n==4&&a[2]==33&&a[3]==guard->binding)result_type[a[1]]|=2;
    }
    failure="depth input guard flag binding occupied";
    for(uint32_t id=1;id<bound;id++)if(result_type[id]==3)goto fail;
    memset(result_type,0,bound*4u);
    failure="depth input guard missing direct FragCoord";if(!frag||!first_function)goto fail;
    float_id=base[type_at[basic.source_image_type]+2];
    for(size_t at=5;at<base_words;at+=base[at]>>16){
        const uint32_t *a=base+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op==20&&n==2)bool_id=a[1];
        if(op==21&&n==4&&a[2]==32&&!a[3])uint_id=a[1];
        if((at>=first_function||(op>=41&&op<=52))&&n>=3&&a[1]<bound&&type_at[a[1]]&&a[2]<bound)result_type[a[2]]=a[1];
        if(op==59&&n==4&&a[2]==frag){
            failure="depth input guard FragCoord pointer";
            if(a[3]!=1||!a[1]||a[1]>=bound||!type_at[a[1]])goto fail;
            const uint32_t *p=base+type_at[a[1]];
            if((p[0]&65535u)!=32||(p[0]>>16)!=4||p[2]!=1||!p[3]||p[3]>=bound||!type_at[p[3]])goto fail;
            const uint32_t *t=base+type_at[p[3]];
            if((t[0]&65535u)!=23||(t[0]>>16)!=4||t[2]!=float_id||t[3]!=4)goto fail;
            vec4=p[3];
        }
    }
    failure="depth input guard invalid direct FragCoord";if(!vec4)goto fail;
    for(size_t at=5;at<words;at+=in[at]>>16){const uint32_t *a=in+at;
        if((a[0]&65535u)==61&&(a[0]>>16)==4&&a[3]==basic.variable_id)loaded[a[2]]=1;}
    uint32_t found=0;
    for(size_t at=5;at<words;at+=in[at]>>16){const uint32_t *a=in+at;
        if((a[0]&65535u)==87&&(a[0]>>16)>=5&&a[3]<bound&&loaded[a[3]]){
            failure="depth input guard coordinate identity";if(a[4]>=bound)goto fail;
            uint32_t t=result_type[a[4]];failure="depth input guard sample coordinate must be float vec2";
            if(!t||!type_at[t])goto fail;
            const uint32_t *v=base+type_at[t];
            if((v[0]&65535u)!=23||(v[0]>>16)!=4||v[2]!=float_id||v[3]!=2||(vec2&&vec2!=t))goto fail;
            vec2=t;coord[a[2]]=a[4];if(!first_coord)first_coord=a[4];found++;
        }
    }
    failure="depth input guard sample accounting";if(found!=basic.sample_count)goto fail;
    uint32_t next=bound;int add_bool=0,add_uint=0,add_bool2=0,add_uint_ptr=0,add_uint2=0;
    if(!bool_id){bool_id=next++;add_bool=1;}
    if(!uint_id){uint_id=next++;add_uint=1;}
    for(size_t at=5;at<base_words;at+=base[at]>>16){const uint32_t *a=base+at;uint32_t op=a[0]&65535u,n=a[0]>>16;
        if(op==23&&n==4&&a[2]==bool_id&&a[3]==2)bool2=a[1];
        if(op==23&&n==4&&a[2]==uint_id&&a[3]==2)uint2=a[1];
        if(op==32&&n==4&&a[2]==12&&a[3]==uint_id)uint_ptr=a[1];
    }
    if(!bool2){bool2=next++;add_bool2=1;}
    if(!uint_ptr){uint_ptr=next++;add_uint_ptr=1;}
    if(!uint2){uint2=next++;add_uint2=1;}
    uint32_t block_type=next++,block_ptr=next++,flag=next++,zero=next++,one=next++;
    uint32_t wx=next++,hy=next++,extent=next++,thx=next++,thy=next++,threshold=next++;
    uint32_t mask=next++,maskvec=next++,onevec=next++,infinity=next++,infinityvec=next++;
    uint32_t exact_type=0,exact_scalar[NVN_DI_CONST_COUNT][2]={{0}},exact_vector[NVN_DI_CONST_COUNT]={0};
    if(exact){exact_type=next++;for(unsigned i=0;i<NVN_DI_CONST_COUNT;i++){
        exact_scalar[i][0]=next++;exact_scalar[i][1]=next++;exact_vector[i]=next++;
    }}
    uint32_t guards_start=next;next+=basic.sample_count*stride;
    size_t capacity=base_words+global_words+(size_t)basic.sample_count*guard_words,dst=5;
    out=malloc(capacity*4);failure="depth input guard output allocation";if(!out)goto fail;
    memcpy(out,base,20);out[3]=next;
#define NVN_DEPTH_GUARD_EMIT(...) do {const uint32_t z_[]={__VA_ARGS__};size_t count_=sizeof z_/4; \
    if(count_>capacity-dst){failure="depth input guard output bounds";goto fail;} \
    memcpy(out+dst,z_,sizeof z_);dst+=count_;} while(0)
    uint32_t guard_index=0;
    for(size_t at=5;at<base_words;at+=base[at]>>16){
        const uint32_t *a=base+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(at==first_type){
            NVN_DEPTH_GUARD_EMIT(3u<<16|71,block_type,2,5u<<16|72,block_type,0,35,0,
                4u<<16|71,flag,34,guard->descriptor_set,4u<<16|71,flag,33,guard->binding);
            for(uint32_t i=0;!exact&&i<basic.sample_count;i++){
                uint32_t id=guards_start+i*26u;
                NVN_DEPTH_GUARD_EMIT(3u<<16|71,id+2,42,3u<<16|71,id+3,42,
                    3u<<16|71,id+9,42,3u<<16|71,id+13,42);
            }
        }
        if(at==first_function){
            if(add_bool)NVN_DEPTH_GUARD_EMIT(2u<<16|20,bool_id);
            if(add_uint)NVN_DEPTH_GUARD_EMIT(4u<<16|21,uint_id,32,0);
            if(add_bool2)NVN_DEPTH_GUARD_EMIT(4u<<16|23,bool2,bool_id,2);
            if(add_uint_ptr)NVN_DEPTH_GUARD_EMIT(4u<<16|32,uint_ptr,12,uint_id);
            if(add_uint2)NVN_DEPTH_GUARD_EMIT(4u<<16|23,uint2,uint_id,2);
            NVN_DEPTH_GUARD_EMIT(3u<<16|30,block_type,uint_id,4u<<16|32,block_ptr,12,block_type,
                4u<<16|59,block_ptr,flag,12,4u<<16|43,uint_id,zero,0,4u<<16|43,uint_id,one,1);
            float width=(float)guard->width,height=(float)guard->height;uint32_t width_bits,height_bits;
            memcpy(&width_bits,&width,4);memcpy(&height_bits,&height,4);
            NVN_DEPTH_GUARD_EMIT(4u<<16|43,float_id,wx,width_bits,4u<<16|43,float_id,hy,height_bits,
                5u<<16|44,vec2,extent,wx,hy,4u<<16|43,float_id,thx,tx,4u<<16|43,float_id,thy,ty,
                5u<<16|44,vec2,threshold,thx,thy,4u<<16|43,uint_id,mask,0x7fffffffu,
                5u<<16|44,uint2,maskvec,mask,mask,5u<<16|44,uint2,onevec,one,one,
                4u<<16|43,float_id,infinity,0x7f800000u,5u<<16|44,vec2,infinityvec,infinity,infinity);
            if(exact){
                NVN_DEPTH_GUARD_EMIT(4u<<16|30,exact_type,uint2,uint2);
                const uint32_t values[NVN_DI_CONST_COUNT][2]={{0,0},{1,1},{8,8},{23,23},{31,31},{32,32},
                    {115,115},{127,127},{126,126},{137,137},{149,149},{150,150},
                    {0x7fffffu,0x7fffffu},{0x800000u,0x800000u},{1280*512,720*512},{1280*2,720*2}};
                for(unsigned i=0;i<NVN_DI_CONST_COUNT;i++)NVN_DEPTH_GUARD_EMIT(
                    4u<<16|43,uint_id,exact_scalar[i][0],values[i][0],
                    4u<<16|43,uint_id,exact_scalar[i][1],values[i][1],
                    5u<<16|44,uint2,exact_vector[i],exact_scalar[i][0],exact_scalar[i][1]);
            }
        }
        if(op==98&&n>=5&&a[2]<bound&&coord[a[2]]){
            uint32_t id=guards_start+guard_index++*stride;
            if(exact){
                size_t emitted=nvn_vk_depth_input_exact_guard_emit(out+dst,capacity-dst,id,vec2,vec4,uint2,bool2,
                    bool_id,uint_id,exact_type,frag,coord[a[2]],flag,uint_ptr,zero,one,exact_vector,exact==2);
                failure="depth exact guard output bounds";if(!emitted)goto fail;dst+=emitted;
            }else{
            NVN_DEPTH_GUARD_EMIT(4u<<16|61,vec4,id,frag,7u<<16|79,vec2,id+1,id,id,0,1,
                5u<<16|133,vec2,id+2,coord[a[2]],extent,5u<<16|131,vec2,id+3,id+2,id+1,
                4u<<16|124,uint2,id+4,id+2,5u<<16|199,uint2,id+5,id+4,maskvec,
                4u<<16|124,vec2,id+6,id+5,5u<<16|128,uint2,id+7,id+5,onevec,
                4u<<16|124,vec2,id+8,id+7,5u<<16|131,vec2,id+9,id+8,id+6,
                4u<<16|124,uint2,id+10,id+3,5u<<16|199,uint2,id+11,id+10,maskvec,
                4u<<16|124,vec2,id+12,id+11,5u<<16|129,vec2,id+13,id+12,id+9,
                4u<<16|124,uint2,id+14,id+13,5u<<16|128,uint2,id+15,id+14,onevec,
                4u<<16|124,vec2,id+16,id+15,5u<<16|184,bool2,id+17,id+6,infinityvec,
                5u<<16|184,bool2,id+18,id+13,infinityvec,5u<<16|184,bool2,id+19,id+16,threshold,
                5u<<16|167,bool2,id+20,id+17,id+18,5u<<16|167,bool2,id+21,id+20,id+19,
                4u<<16|155,bool_id,id+22,id+21,6u<<16|169,uint_id,id+23,id+22,zero,one,
                5u<<16|65,uint_ptr,id+24,flag,zero,7u<<16|241,uint_id,id+25,id+24,one,zero,id+23);
            }
        }
        if(op==15&&base[1]>=0x10400u){
            failure="depth input guard entry point interface overflow";if(n==65535u)goto fail;
            if(n+1>capacity-dst)goto fail;
            memcpy(out+dst,a,n*4);out[dst]=(n+1)<<16|15;dst+=n;out[dst++]=flag;
        }else{if(n>capacity-dst)goto fail;memcpy(out+dst,a,n*4);dst+=n;}
    }
#undef NVN_DEPTH_GUARD_EMIT
    failure="depth input guard output sample accounting";if(guard_index!=basic.sample_count)goto fail;
    *info=(NvnVkDepthInputGuardInfo){.transform=basic,.descriptor_set=guard->descriptor_set,.binding=guard->binding,
        .flag_variable_id=flag,.fragcoord_variable_id=frag,.coordinate_type_id=vec2,
        .first_coordinate_id=first_coord,.guard_count=guard_index};
    *out_words=dst;free(base);free(type_at);free(result_type);free(coord);free(loaded);return out;
fail:
    free(base);free(out);free(type_at);free(result_type);free(coord);free(loaded);
    if(error)*error=failure;
    return NULL;
}
static uint32_t *nvn_vk_depth_input_guarded_variant(const uint32_t *in,size_t words,
        uint32_t descriptor_set,uint32_t binding,uint32_t attachment_index,
        const NvnVkDepthInputGuard *guard,size_t *out_words,
        NvnVkDepthInputGuardInfo *info,const char **error) {
    return nvn_vk_depth_input_guarded_variant_impl(in,words,descriptor_set,binding,attachment_index,guard,out_words,info,error,0);
}
/* Device experiment only: exact comparison of binary32 UV*extent with the
 * strict open interval FragCoord.xy +/- 1/512. No floating-point arithmetic,
 * shaderInt64 or float64 is used. Positive normal UV exponent bits must be
 * 115..127. FragCoord must be an exact in-bounds positive half-integer center.
 * Uses the same SSBO observation/fence contract as the conservative variant.
 * This establishes the interval predicate, NOT a portable filtering theorem. */
static inline uint32_t *nvn_vk_depth_input_exact_guarded_variant(const uint32_t *in,size_t words,
        uint32_t descriptor_set,uint32_t binding,uint32_t attachment_index,
        const NvnVkDepthInputGuard *guard,size_t *out_words,
        NvnVkDepthInputGuardInfo *info,const char **error) {
    return nvn_vk_depth_input_guarded_variant_impl(in,words,descriptor_set,binding,attachment_index,guard,out_words,info,error,1);
}
/* Separate, explicitly requested DEVICE-CALIBRATED experiment. This accepts
 * equality at exactly +/-1/512, using the same exact integer interval; every
 * value outside that CLOSED interval still fails. Requires independent host
 * authorization for that precise device/driver's measured tie selection and
 * an explicit separate marker. Does not change the strict or outward APIs,
 * prove a portable filtering theorem, or remove the clear/fence/check contract.
 * Extent and threshold restrictions are identical to the exact strict API. */
static inline uint32_t *nvn_vk_depth_input_calibrated_ties_guarded_variant(const uint32_t *in,size_t words,
        uint32_t descriptor_set,uint32_t binding,uint32_t attachment_index,
        const NvnVkDepthInputGuard *guard,size_t *out_words,
        NvnVkDepthInputGuardInfo *info,const char **error) {
    return nvn_vk_depth_input_guarded_variant_impl(in,words,descriptor_set,binding,attachment_index,guard,out_words,info,error,2);
}
#endif
