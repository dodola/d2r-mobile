#include "../../gfx/nvn/nvn_vk_redundant_clip.h"
#include <assert.h>
#include <stdio.h>

static uint32_t *read_module(const char *name,size_t *words) {
    FILE *f=fopen(name,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}long bytes=ftell(f);rewind(f);
    if(bytes<20||bytes%4){fclose(f);return NULL;}
    uint32_t *p=malloc((size_t)bytes);if(!p){fclose(f);return NULL;}
    if(fread(p,1,(size_t)bytes,f)!=(size_t)bytes){free(p);fclose(f);return NULL;}
    fclose(f);*words=(size_t)bytes/4;return p;
}

static void mutation_tests(uint32_t *module,size_t words) {
    size_t count=0;int guard=0;const char *error=NULL;
    uint32_t *out=nvn_vk_redundant_clip(module,words,&count,&guard,&error);
    assert(out&&!error&&guard==1&&count<words);free(out);
    /* Each negative test changes a specific part of the real proven shader.
     * The transform must preserve it byte-for-byte instead of guessing. */
    uint32_t *bad=malloc(words*4);assert(bad);memcpy(bad,module,words*4);
    size_t last_load=0;for(size_t at=5;at<words;at+=bad[at]>>16)if((bad[at]&65535u)==61)last_load=at;
    assert(last_load);uint32_t saved=bad[last_load+3],pointer_type=0,other_local=0;
    for(size_t at=5;at<words;at+=bad[at]>>16)if((bad[at]&65535u)==59&&bad[at+2]==saved)pointer_type=bad[at+1];
    assert(pointer_type);
    for(size_t at=5;at<words;at+=bad[at]>>16)if((bad[at]&65535u)==59&&bad[at]>>16==4&&bad[at+1]==pointer_type&&bad[at+2]!=saved&&bad[at+3]==7)other_local=bad[at+2];
    assert(other_local);bad[last_load+3]=other_local;
    out=nvn_vk_redundant_clip(bad,words,&count,&guard,&error);
    assert(out&&!guard&&count==words&&!memcmp(out,bad,words*4));free(out);
    memcpy(bad,module,words*4);bad[last_load]=4u<<16|83u; /* Pointer alias/copy is unsupported. */
    out=nvn_vk_redundant_clip(bad,words,&count,&guard,&error);
    assert(out&&!guard&&count==words&&!memcmp(out,bad,words*4));free(out);
    memcpy(bad,module,words*4);bad[5]=0;
    assert(!nvn_vk_redundant_clip(bad,words,&count,&guard,&error)&&error&&!count&&!guard);
    free(bad);puts("real redundant clip proof and non-equivalent/alias/malformed mutations passed");
}

static void assert_unchanged(const uint32_t *input,size_t words) {
    size_t count=0;int guard=123;const char *error=NULL;
    uint32_t *out=nvn_vk_redundant_clip(input,words,&count,&guard,&error);
    assert(out&&!error&&!guard&&count==words&&!memcmp(out,input,words*4));free(out);
}
/* Semantic mutations against an actual, spirv-val checked source module.
 * Optional output files allow external validation of the negative inputs. */
static void soundness_tests(const uint32_t *module,size_t words,const char *prefix) {
    uint32_t *bad=malloc((words+16)*4);assert(bad);
    uint32_t *base=malloc(words*4);assert(base);memcpy(base,module,words*4);
    size_t count=0;int guard=0;const char *error=NULL;
    uint32_t *out=nvn_vk_redundant_clip(module,words,&count,&guard,&error);
    assert(out&&!error&&guard&&count<words);free(out);
    size_t last_load=0;
    for(size_t at=5;at<words;at+=module[at]>>16)if((module[at]&65535u)==61)last_load=at;
    assert(last_load);uint32_t local=module[last_load+3];
    size_t local_store=0,clip_store=0,first_function=0,first_type=0,entrypoint=0,position_store=0;
    uint32_t clip_pointer=0;
    for(size_t at=5;at<words;at+=module[at]>>16){
        uint32_t op=module[at]&65535u;
        if(op==54&&!first_function)first_function=at;
        if(op>=19&&op<=39&&!first_type)first_type=at;
        if(op==15)entrypoint=at;
        if(op==62&&module[at+1]==local&&at<last_load)local_store=at;
        if(op==62&&at>last_load){clip_store=at;clip_pointer=module[at+1];}
    }
    assert(local_store&&clip_store&&entrypoint&&first_function);
    for(size_t at=5;at<last_load;at+=module[at]>>16)if((module[at]&65535u)==62&&module[at+2]<module[3]){
        uint32_t id=module[at+2];
        for(size_t k=5;k<at;k+=module[k]>>16)if((module[k]&65535u)==61&&module[k+2]==id&&module[k+3]==local)position_store=at;
    }
    assert(position_store);
    const char *labels[]={"volatile-clip-store","volatile-position-overwrite","volatile-local-overwrite","uninitialized-z-local","geometry-entry","relaxed-clip-load","relaxed-clip-member","relaxed-position-member","relaxed-output-root","relaxed-clip-pointer"};
    for(unsigned test=0;test<10;test++){
        memcpy(bad,base,words*4);size_t n=words;
        if(test==0){
            memmove(bad+clip_store+4,bad+clip_store+3,(words-clip_store-3)*4);
            bad[clip_store]=4u<<16|62u;bad[clip_store+3]=1;n++;
        }else if(test==1){
            /* An extra volatile Position.z store after the allegedly final
             * equivalent write changes the actual native clipping plane. */
            size_t at=clip_store+3;memmove(bad+at+4,bad+at,(words-at)*4);
            bad[at]=4u<<16|62u;bad[at+1]=module[position_store+1];bad[at+2]=module[local_store+2];bad[at+3]=1;n+=4;
        }else if(test==2){
            /* A volatile local write between the two compared loads must
             * never be overlooked because OpStore is not exactly 3 words. */
            size_t at=last_load;memmove(bad+at+4,bad+at,(words-at)*4);
            bad[at]=4u<<16|62u;bad[at+1]=local;bad[at+2]=module[local_store+2];bad[at+3]=1;n+=4;
        }else if(test==3){
            /* Delete all initializations of this function local. The module
             * remains valid, but separate undefined loads are not equal. */
            size_t dst=5;
            for(size_t at=5;at<words;at+=module[at]>>16){size_t k=module[at]>>16;
                if((module[at]&65535u)==62&&module[at+1]==local)continue;
                memcpy(bad+dst,module+at,k*4);dst+=k;}
            n=dst;
        }else if(test==4){bad[entrypoint+1]=3;} /* Never reason about geometry emissions. */
        else if(test==5){
            memmove(bad+first_type+3,bad+first_type,(words-first_type)*4);
            bad[first_type]=3u<<16|71u;bad[first_type+1]=module[last_load+2];bad[first_type+2]=0;n+=3;
        }else{
            uint32_t structure=0,clip_member=0,position_member=0,root=0;
            for(size_t at=5;at<words;at+=module[at]>>16)if((module[at]&65535u)==72&&module[at]>>16==5&&module[at+3]==11){
                if(module[at+4]==3){structure=module[at+1];clip_member=module[at+2];}
                if(module[at+4]==0)position_member=module[at+2];
            }
            for(size_t at=5;at<words;at+=module[at]>>16)if((module[at]&65535u)==65&&module[at+2]==clip_pointer)root=module[at+3];
            assert(structure&&root);
            size_t extra=test<8?4:3;
            memmove(bad+first_type+extra,bad+first_type,(words-first_type)*4);
            if(test<8){bad[first_type]=4u<<16|72u;bad[first_type+1]=structure;bad[first_type+2]=test==6?clip_member:position_member;bad[first_type+3]=0;}
            else {bad[first_type]=3u<<16|71u;bad[first_type+1]=test==8?root:clip_pointer;bad[first_type+2]=0;}
            n+=extra;
        }
        assert_unchanged(bad,n);
        if(prefix){char name[1024];int written=snprintf(name,sizeof name,"%s-%s.spv",prefix,labels[test]);assert(written>0&&(size_t)written<sizeof name);
            FILE *f=fopen(name,"wb");assert(f);assert(fwrite(bad,4,n,f)==n);assert(!fclose(f));}
    }
    (void)clip_pointer;free(base);free(bad);
    puts("redundant clip soundness: volatile writes, uninitialized local, non-vertex entry and relaxed-precision load preserved");
}

/* Valid structured CFG fixtures, constructed without a runtime assembler.
 * Positive: entry -> z-definition -> selection(left/right) -> unique return.
 * Negatives add a return path bypassing z, or an independent write in right.
 * Every emitted negative is spirv-val checked by the external audit runner. */
static void cfg_tests(const char *prefix) {
    static const uint32_t globals[]={0x07230203,0x10300,0,128,0,
        2u<<16|17,1,2u<<16|17,32,3u<<16|14,0,1,
        6u<<16|15,0,30,0x6e69616d,0,10,
        3u<<16|71,8,2,
        5u<<16|72,8,0,11,0,5u<<16|72,8,1,11,1,5u<<16|72,8,2,11,3,
        2u<<16|19,1,3u<<16|33,2,1,3u<<16|22,3,32,4u<<16|23,4,3,4,
        4u<<16|21,5,32,0,4u<<16|43,5,6,8,4u<<16|28,7,3,6,
        5u<<16|30,8,4,3,7,4u<<16|32,9,3,8,4u<<16|32,11,3,3,4u<<16|32,12,7,3,
        4u<<16|43,5,13,0,4u<<16|43,5,14,1,4u<<16|43,5,15,2,4u<<16|43,5,16,3,
        4u<<16|43,5,17,4,4u<<16|43,5,18,5,4u<<16|43,5,19,6,4u<<16|43,5,20,7,
        4u<<16|43,3,21,0,4u<<16|43,3,22,0x3f800000,4u<<16|43,3,23,0x3e800000,
        2u<<16|20,24,3u<<16|41,24,25,4u<<16|59,9,10,3};
    const char *names[]={"cfg-positive","cfg-bypass-z","cfg-branch-local-write","cfg-branch-position-write"};
    for(unsigned test=0;test<4;test++){
        uint32_t module[512];memcpy(module,globals,sizeof globals);size_t words=sizeof globals/4;
#define EMIT(...) do {const uint32_t instruction[]={__VA_ARGS__};memcpy(module+words,instruction,sizeof instruction);words+=sizeof instruction/4;}while(0)
        EMIT(5u<<16|54,1,30,0,2);EMIT(2u<<16|248,40);EMIT(4u<<16|59,12,31,7);
        for(unsigned c=0;c<8;c++){EMIT(6u<<16|65,11,50+c,10,15,13+c);EMIT(3u<<16|62,50+c,21);}
        for(unsigned c=0;c<4;c++){EMIT(6u<<16|65,11,70+c,10,13,13+c);EMIT(3u<<16|62,70+c,c==3?22:21);}
        EMIT(5u<<16|65,11,74,10,14);EMIT(3u<<16|62,74,22);
        if(test==1){EMIT(3u<<16|247,46,0);EMIT(4u<<16|250,25,41,46);}
        else EMIT(2u<<16|249,41);
        EMIT(2u<<16|248,41);EMIT(3u<<16|62,31,23);EMIT(4u<<16|61,3,61,31);
        EMIT(6u<<16|65,11,60,10,13,15);EMIT(3u<<16|62,60,61);EMIT(2u<<16|249,42);
        EMIT(2u<<16|248,42);EMIT(3u<<16|247,45,0);EMIT(4u<<16|250,25,43,44);
        EMIT(2u<<16|248,43);EMIT(2u<<16|249,45);EMIT(2u<<16|248,44);
        if(test==2)EMIT(3u<<16|62,31,22);
        if(test==3){EMIT(6u<<16|65,11,75,10,13,15);EMIT(3u<<16|62,75,22);}
        EMIT(2u<<16|249,45);EMIT(2u<<16|248,45);EMIT(2u<<16|249,46);
        EMIT(2u<<16|248,46);EMIT(4u<<16|61,3,62,31);EMIT(6u<<16|65,11,63,10,15,13);
        EMIT(3u<<16|62,63,62);EMIT(1u<<16|253);EMIT(1u<<16|56);
#undef EMIT
        if(test)assert_unchanged(module,words);
        else {size_t count=0;int guard=0;const char *error=NULL;uint32_t *out=nvn_vk_redundant_clip(module,words,&count,&guard,&error);
            assert(out&&!error&&guard&&count<words);free(out);}
        if(prefix){char name[1024];int n=snprintf(name,sizeof name,"%s-%s.spv",prefix,names[test]);assert(n>0&&(size_t)n<sizeof name);
            FILE *f=fopen(name,"wb");assert(f);assert(fwrite(module,4,words,f)==words);assert(!fclose(f));}
    }
    puts("redundant clip CFG: dominating branch positive; bypass-z, branch-local-write and branch-Position.z-write preserved");
}

int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"--cfg-tests")){cfg_tests(argv[2]);return 0;}
    if(argc<3||argc>5)return 2;
    size_t words=0;uint32_t *input=read_module(argv[1],&words);if(!input)return 3;
    if(argc>=4){mutation_tests(input,words);soundness_tests(input,words,argc==5?argv[4]:NULL);cfg_tests(argc==5?argv[4]:NULL);}
    size_t count=0;int guard=0;const char *error=NULL;
    uint32_t *output=nvn_vk_redundant_clip(input,words,&count,&guard,&error);free(input);
    if(!output){fprintf(stderr,"%s\n",error);return 4;}
    FILE *f=fopen(argv[2],"wb");if(!f){free(output);return 3;}
    int written=fwrite(output,4,count,f)==count;int closed=fclose(f)==0;free(output);
    if(!written||!closed)return 3;
    printf("require_native_depth_clip=%d words=%zu\n",guard,count);return 0;
}
