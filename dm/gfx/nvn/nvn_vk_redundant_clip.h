/* Prove and remove redundant vertex ClipDistance output stores. This narrow
 * transform accepts only a single function with one return, direct constant
 * output access chains, and scalar stores without aliases or reads. It never
 * removes a general user clipping plane. A z-equivalent plane additionally
 * requires native Vulkan near clipping at every draw (depth clamp disabled,
 * zero-to-one depth clipping enabled); the caller must enforce that flag.
 * An unused-distance pass can subsequently remove the now-unaccessed member.
 * Input must otherwise have passed SPIR-V validation. */
#ifndef NVN_VK_REDUNDANT_CLIP_H
#define NVN_VK_REDUNDANT_CLIP_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t at;
    uint32_t block;
} NvnClipDefinition;

static int nvn_clip_id_operand(const uint32_t *a,uint32_t n,uint32_t j) {
    uint32_t op=a[0]&65535u;
    /* Valid global type/constant declarations cannot consume an Output
     * variable or a function-local access pointer. Their widths, counts and
     * literal values can still equal one of those IDs. */
    if(op>=19&&op<=52)return 0;
    if(op==2||op==3||op==4||op==7||op==10||op==11||op==17)return 0;
    if(op==5||op==6||op==71||op==72)return j==1;
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
/* Does every path from entry to exit pass through required? Only the direct
 * branch forms are accepted. Unsupported terminators leave the module native.
 * Removing required and checking reachability is a dominance proof, including
 * loops; instruction ordering within a block is checked separately. */
static int nvn_clip_dominates(const uint32_t *in,size_t words,uint32_t entry,uint32_t exit,uint32_t required) {
    uint32_t bound=in[3],block=0;int result=0;
    uint32_t *first=calloc(bound,4),*second=calloc(bound,4),*queue=calloc(bound,4);
    uint8_t *label=calloc(bound,1),*seen=calloc(bound,1);
    if(!first||!second||!queue||!label||!seen)goto done;
    for(size_t at=5;at<words;at+=in[at]>>16){
        const uint32_t *a=in+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op==248){block=a[1];label[block]=1;}
        if(op==56)block=0;
        if(op==251||op==252||op==254||op==255||op==4416||op==4448)goto done;
        if(op==249){if(n!=2||!block||!a[1]||a[1]>=bound)goto done;first[block]=a[1];}
        if(op==250){if((n!=4&&n!=6)||!block||!a[2]||a[2]>=bound||!a[3]||a[3]>=bound)goto done;first[block]=a[2];second[block]=a[3];}
    }
    if(!label[entry]||!label[exit]||!label[required])goto done;
    /* First require a real reachable exit, rather than vacuous dominance. */
    for(unsigned pass=0;pass<2;pass++){
        memset(seen,0,bound);size_t head=0,tail=0;
        if(!pass||entry!=required){queue[tail++]=entry;seen[entry]=1;}
        while(head<tail){uint32_t b=queue[head++];uint32_t next[2]={first[b],second[b]};
            for(unsigned j=0;j<2;j++){uint32_t id=next[j];if(!id||(pass&&id==required)||seen[id])continue;
                if(!label[id])goto done;seen[id]=1;queue[tail++]=id;}
        }
        if(!pass&&!seen[exit])goto done;
        if(pass)result=!seen[exit];
    }
done:
    free(first);free(second);free(queue);free(label);free(seen);return result;
}
static uint32_t *nvn_vk_redundant_clip(const uint32_t *input,size_t word_count,
        size_t *out_count,int *require_native_depth_clip,const char **error) {
    if(out_count)*out_count=0;
    if(require_native_depth_clip)*require_native_depth_clip=0;
    if(error)*error=NULL;
    NvnClipDefinition *definitions=NULL;
    uint8_t *remove=NULL,*zero=NULL;
    uint32_t *values=NULL,*pointee=NULL,*path=NULL;
    uint32_t *output=NULL;
    const char *failure="redundant clip arguments";
    if(!input||!out_count||!require_native_depth_clip||word_count<5)goto fail;
    failure="redundant clip module bounds";
    if(input[0]!=0x07230203u||!input[3]||input[3]>1000000u||word_count>SIZE_MAX/4)goto fail;
    uint32_t bound=input[3];
    definitions=calloc(bound,sizeof *definitions);zero=calloc(bound,1);
    values=calloc(bound,sizeof *values);pointee=calloc(bound,sizeof *pointee);
    path=calloc(bound,sizeof *path);remove=calloc(word_count,1);
    failure="redundant clip allocation";
    if(!definitions||!zero||!values||!pointee||!path||!remove)goto fail;
    uint32_t block=0,entry=0,return_block=0,functions=0,returns=0,entries=0,entry_function=0,function=0;
    uint32_t struct_id=0,clip_member=0,position_member=UINT32_MAX;
    size_t return_at=0;
    int supported=1;
    for(size_t at=5;at<word_count;){
        uint32_t n=input[at]>>16,op=input[at]&65535u;const uint32_t *a=input+at;
        failure="redundant clip instruction bounds";
        if(!n||n>word_count-at)goto fail;
        if(op==15){
            if(n<4||a[1]!=0||!a[2]||a[2]>=bound)supported=0;
            entries++;entry_function=n>=3?a[2]:0;
        }
        if(op==54){if(n!=5)goto fail;functions++;function=a[2];block=0;}
        if(op==248){if(n!=2||!a[1]||a[1]>=bound)goto fail;block=a[1];if(!entry)entry=block;}
        if(op==253){returns++;return_block=block;return_at=at;}
        if(op==57||op==254||op==252)supported=0;
        if(op==21||op==22||op==28||op==30||op==32){
            if(n<2||!a[1]||a[1]>=bound)goto fail;
            if((op==21&&n!=4)||(op==22&&n!=3)||(op==28&&n!=4))goto fail;
            definitions[a[1]]=(NvnClipDefinition){at,block};
            if(op==32){if(n!=4||a[3]>=bound)goto fail;pointee[a[1]]=a[3];}
        }
        if(op==43){
            if(n<4||!a[1]||a[1]>=bound||!a[2]||a[2]>=bound)goto fail;
            definitions[a[2]]=(NvnClipDefinition){at,block};
            const uint32_t *t=input+definitions[a[1]].at;
            if(n==4&&(t[0]&65535u)==21&&t[2]==32){values[a[2]]=a[3];zero[a[2]]=2;}
            if(n==4&&(t[0]&65535u)==22&&t[2]==32&&(a[3]&0x7fffffffu)==0)zero[a[2]]=1;
        }
        if(op==59||op==61||op==65){
            if(n<4||!a[2]||a[2]>=bound)goto fail;
            definitions[a[2]]=(NvnClipDefinition){at,block};
        }
        if(op==72&&n==5&&a[3]==11&&a[4]==3){
            if(struct_id)supported=0;struct_id=a[1];clip_member=a[2];
        }
        at+=n;
    }
    if(!supported||!struct_id||struct_id>=bound||functions!=1||returns!=1||entries!=1||entry_function!=function||!entry||!return_block)goto unchanged;
    const uint32_t *st=input+definitions[struct_id].at;
    if((st[0]&65535u)!=30||clip_member>=(st[0]>>16)-2)goto unchanged;
    uint32_t array_id=st[2+clip_member];if(!array_id||array_id>=bound)goto unchanged;
    const uint32_t *array=input+definitions[array_id].at;
    if((array[0]&65535u)!=28||array[0]>>16!=4||array[2]>=bound||array[3]>=bound||zero[array[3]]!=2)goto unchanged;
    uint32_t components=values[array[3]];
    const uint32_t *scalar=input+definitions[array[2]].at;
    if(!components||components>8||(scalar[0]&65535u)!=22||scalar[0]>>16!=3||scalar[2]!=32)goto unchanged;
    for(size_t at=5;at<word_count;at+=input[at]>>16){
        const uint32_t *a=input+at;
        if((a[0]&65535u)==72&&a[0]>>16==5&&a[1]==struct_id&&a[3]==11&&a[4]==0){
            if(position_member!=UINT32_MAX)goto unchanged;position_member=a[2];
        }
    }
    if(position_member==UINT32_MAX)goto unchanged;
    uint32_t root=0;
    for(uint32_t id=1;id<bound;id++)if(definitions[id].at){
        const uint32_t *a=input+definitions[id].at;
        if((a[0]&65535u)==59&&a[0]>>16>=4&&a[1]<bound&&a[3]==3&&pointee[a[1]]==struct_id){if(root)goto unchanged;root=id;}
    }
    if(!root)goto unchanged;
    /* path: 1..8 clip component, 16..19 Position components; every pointer derives directly
     * from the output root. Dynamic or whole-position stores are rejected. */
    for(size_t at=5;at<word_count;at+=input[at]>>16){
        const uint32_t *a=input+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op!=65||n<4||a[3]!=root)continue;
        if(n<5||a[4]>=bound||zero[a[4]]!=2)goto unchanged;
        uint32_t member=values[a[4]];
        if(member>=(st[0]>>16)-2)goto unchanged;
        path[a[2]]=32; /* Track other output pointers too, to reject aliases. */
        if(member==clip_member){
            if(n!=6||a[5]>=bound||zero[a[5]]!=2||values[a[5]]>=components)goto unchanged;
            path[a[2]]=values[a[5]]+1;
        }else if(member==position_member){
            if(n!=6||a[5]>=bound||zero[a[5]]!=2||values[a[5]]>=4)goto unchanged;
            path[a[2]]=16+values[a[5]];
        }
    }
    size_t initial[8]={0},clip_final=0,z_final=0;uint32_t clip_value=0,z_value=0,z_block=0;
    block=0;
    for(size_t at=5;at<word_count;at+=input[at]>>16){
        const uint32_t *a=input+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        if(op==248)block=a[1];
        if(op==56)block=0;
        if(!block)continue;
        for(uint32_t j=1;j<n;j++)if(nvn_clip_id_operand(a,n,j)&&a[j]==root&&!(op==65&&j==3)&&!(op==59&&j==2))goto unchanged;
        for(uint32_t j=1;j<n;j++)if(nvn_clip_id_operand(a,n,j)&&a[j]<bound&&path[a[j]]){
            if(op==65&&j==2)continue;
            /* Optional Store memory operands require semantics beyond this
             * proof (Volatile/visibility etc.). Never ignore such a write. */
            if(op==62&&j==1&&n==3)continue;
            goto unchanged; /* A read, alias, call, or other unknown pointer use. */
        }
        if(op!=62||n!=3||a[1]>=bound)continue;
        uint32_t p=path[a[1]];
        if(p&&p<=8){
            uint32_t component=p-1;
            if(a[2]<bound&&zero[a[2]]==1&&block==entry&&!initial[component])initial[component]=at;
            else if(!component&&block==return_block&&!clip_final){clip_final=at;clip_value=a[2];}
            else goto unchanged;
        }
        if(p==18){
            if(block==entry&&a[2]<bound&&zero[a[2]]==1&&!z_final)continue;
            if(z_final)goto unchanged; /* Only one non-initial Position.z write. */
            z_final=at;z_value=a[2];z_block=block;
        }
    }
    for(uint32_t c=0;c<components;c++)if(!initial[c])goto unchanged;
    /* Check module-level references as well: only the interface declaration,
     * direct access chains, stores, names and direct decorations are known.
     * Reject decoration groups and any other unknown pointer users. */
    for(size_t at=5;at<word_count;at+=input[at]>>16){
        const uint32_t *a=input+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
        for(uint32_t j=1;j<n;j++)if(nvn_clip_id_operand(a,n,j)&&a[j]==root){
            if(op==15||(op==59&&j==2)||(op==65&&j==3)||((op==5||op==71)&&j==1))continue;
            goto unchanged;
        }
        for(uint32_t j=1;j<n;j++)if(nvn_clip_id_operand(a,n,j)&&a[j]<bound&&path[a[j]]){
            if((op==65&&j==2)||(op==62&&j==1)||((op==5||op==71)&&j==1))continue;
            goto unchanged;
        }
    }
    int need_native=0;
    if(clip_final){
        if(!z_final||z_final>=return_at||clip_final>=return_at||clip_value>=bound||z_value>=bound)goto unchanged;
        NvnClipDefinition cz=definitions[clip_value],pz=definitions[z_value];
        if(!cz.at||!pz.at||cz.block!=return_block||pz.block!=z_block)goto unchanged;
        const uint32_t *cl=input+cz.at,*pl=input+pz.at;
        if((cl[0]&65535u)!=61||cl[0]>>16!=4||(pl[0]&65535u)!=61||pl[0]>>16!=4||cl[3]!=pl[3]||cl[3]>=bound)goto unchanged;
        uint32_t local=cl[3];const uint32_t *v=input+definitions[local].at;
        if((v[0]&65535u)!=59||v[0]>>16!=4||v[3]!=7||v[1]>=bound||pointee[v[1]]!=array[2])goto unchanged;
        if(pz.at>=z_final||cz.at>=clip_final)goto unchanged;
        if(!nvn_clip_dominates(input,word_count,entry,return_block,z_block))goto unchanged;
        int initialized=0;
        /* RelaxedPrecision could independently lower either load, defeating
         * exact equality near zero. Keep such modules on their native path. */
        for(size_t at=5;at<word_count;at+=input[at]>>16){
            const uint32_t *a=input+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
            if(op==73||op==74||op==75)goto unchanged;
            if(op==71&&n>=3&&(a[1]==local||a[1]==clip_value||a[1]==z_value||a[1]==array[2]||a[1]==root||a[1]==struct_id||(a[1]<bound&&path[a[1]]))&&(a[2]==0||a[2]==39||a[2]==40))goto unchanged;
            if(op==72&&n>=4&&a[1]==struct_id&&(a[2]==clip_member||a[2]==position_member)&&(a[3]==0||a[3]==39||a[3]==40))goto unchanged;
        }
        block=0;
        for(size_t at=5;at<word_count;at+=input[at]>>16){
            const uint32_t *a=input+at;uint32_t n=a[0]>>16,op=a[0]&65535u;
            if(op==248)block=a[1];if(op==56)block=0;
            if(!block)continue;
            for(uint32_t j=1;j<n;j++)if(nvn_clip_id_operand(a,n,j)&&a[j]==local){
                if((op==59&&j==2)||(op==61&&j==3&&n==4)||(op==62&&j==1&&n==3))continue;
                goto unchanged;
            }
            if(op==62&&n==3&&a[1]==local){
                /* All definitions precede the Position load in its own block.
                 * This block dominates the return, and no later path can
                 * alter the local without updating Position.z as well. */
                if(block!=z_block||at>=pz.at||(block==cz.block&&at>=cz.at))goto unchanged;
                initialized=1;
            }
        }
        /* The local must have a definite assignment in the dominating z block.
         * Unknown aliases and writes elsewhere were rejected above. */
        if(!initialized)goto unchanged;
        need_native=1;
    }
    for(size_t at=5;at<word_count;at+=input[at]>>16){
        const uint32_t *a=input+at;uint32_t op=a[0]&65535u,n=a[0]>>16;
        if((op==65&&a[2]<bound&&path[a[2]]&&path[a[2]]<=8)||
           (op==62&&a[1]<bound&&path[a[1]]&&path[a[1]]<=8)||
           ((op==5||op==71)&&n>=2&&a[1]<bound&&path[a[1]]&&path[a[1]]<=8))remove[at]=1;
    }
    output=malloc(word_count*4);failure="redundant clip output allocation";if(!output)goto fail;
    memcpy(output,input,20);size_t cursor=5;
    for(size_t at=5;at<word_count;at+=input[at]>>16)if(!remove[at]){size_t n=input[at]>>16;memcpy(output+cursor,input+at,n*4);cursor+=n;}
    *out_count=cursor;*require_native_depth_clip=need_native;goto done;
unchanged:
    output=malloc(word_count*4);failure="redundant clip unchanged allocation";if(!output)goto fail;
    memcpy(output,input,word_count*4);*out_count=word_count;
done:
    free(definitions);free(zero);free(values);free(pointee);free(path);free(remove);return output;
fail:
    free(definitions);free(zero);free(values);free(pointee);free(path);free(remove);free(output);
    if(error)*error=failure;return NULL;
}
#endif
