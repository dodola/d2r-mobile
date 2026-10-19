#include "../../gfx/nvn/nvn_vk_depth_input.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>

#define I(n,op) (((uint32_t)(n)<<16)|(op))
typedef struct {uint32_t w[2048];size_t n;} Module;
static unsigned checks;
static uint64_t fingerprint(const uint32_t *w,size_t n) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<n;i++)for(unsigned b=0;b<4;b++)hash=(hash^((w[i]>>(b*8))&255u))*UINT64_C(1099511628211);
    return hash;
}
static Module fixture(void) {
    /* Independently authored, complete Vulkan1.1 Fragment shader. Both
     * textures share all original opaque/pointer types and contribute output. */
    const uint32_t a[]={0x07230203,0x10300,0,23,0,
        I(2,17),1,I(3,14),0,1,I(6,15),4,16,0x6e69616d,0,12,I(3,16),16,7,
        I(4,71),10,34,1,I(4,71),10,33,3,I(4,71),11,34,1,I(4,71),11,33,4,
        I(4,71),12,30,0,
        I(2,19),1,I(3,33),2,1,I(3,22),3,32,I(4,23),4,3,4,I(4,23),5,3,2,
        I(9,25),6,3,1,0,0,0,1,0,I(3,27),7,6,I(4,32),8,0,7,I(4,32),9,3,4,
        I(4,59),8,10,0,I(4,59),8,11,0,I(4,59),9,12,3,
        I(4,43),3,13,0x3e800000,I(4,43),3,14,0x3f400000,I(5,44),5,15,13,14,
        I(5,54),1,16,0,2,I(2,248),17,I(4,61),7,18,10,I(4,61),7,19,11,
        I(5,87),4,20,18,15,I(5,87),4,21,19,15,I(5,129),4,22,20,21,
        I(3,62),12,22,I(1,253),I(1,56)};
    Module m={{0},sizeof a/4};memcpy(m.w,a,sizeof a);return m;
}
static size_t find(const Module *m,unsigned op,unsigned occurrence) {
    for(size_t at=5;at<m->n;at+=m->w[at]>>16)if((m->w[at]&65535)==op){if(!occurrence--)return at;}
    assert(0);return 0;
}
static void insert(Module *m,size_t at,const uint32_t *a,size_t n) {
    assert(m->n+n<=2048);memmove(m->w+at+n,m->w+at,(m->n-at)*4);memcpy(m->w+at,a,n*4);m->n+=n;
}
#define INSERT(m,at,...) do {const uint32_t a[]={__VA_ARGS__};insert(&(m),(at),a,sizeof a/4);}while(0)
static void emit(const char *dir,const char *name,const uint32_t *w,size_t n) {
    if(!dir)return;
    char path[2048];int k=snprintf(path,sizeof path,"%s/%s.spv",dir,name);assert(k>0&&(size_t)k<sizeof path);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(w,4,n,f)==n);assert(!fclose(f));
}
static void reject(Module m,const char *dir,const char *name,int emit_valid) {
    uint32_t before[2048];memcpy(before,m.w,m.n*4);size_t n=99;NvnVkDepthInputInfo info;memset(&info,0xff,sizeof info);const char *error=NULL;
    uint32_t *out=nvn_vk_depth_input_variant(m.w,m.n,1,3,0,&n,&info,&error);
    assert(!out&&!n&&error&&!info.sample_count&&!memcmp(before,m.w,m.n*4));checks++;
    if(emit_valid)emit(dir,name,m.w,m.n);
}
static void accept(Module m,const char *dir,const char *name,unsigned loads,unsigned samples) {
    uint32_t before[2048];memcpy(before,m.w,m.n*4);size_t n=0;NvnVkDepthInputInfo info;const char *error="old";
    uint32_t *out=nvn_vk_depth_input_variant(m.w,m.n,1,3,2,&n,&info,&error);
    if(!out)fprintf(stderr,"Unexpected rejection: %s\n",error);
    assert(out&&!error&&n>m.n&&!memcmp(before,m.w,m.n*4));
    assert(info.descriptor_set==1&&info.binding==3&&info.input_attachment_index==2&&info.variable_id==10);
    assert(info.load_count==loads&&info.sample_count==samples&&info.new_id_count>=2&&info.new_id_count<=6);
    assert(out[3]==m.w[3]+info.new_id_count&&info.source_pointer_type==8&&info.source_sampled_image_type==7&&info.source_image_type==6);
    unsigned kept_other=0,rewritten=0,kept_shared=0;
    for(size_t at=5;at<n;at+=out[at]>>16){const uint32_t *a=out+at;unsigned op=a[0]&65535;
        if(op==87&&a[2]==21){assert(a[3]==19&&a[4]==15);kept_other++;}
        if(op==98){assert(a[3]==18&&a[4]==out[3]-1);rewritten++;}
        if((op==25&&a[1]==6)||(op==27&&a[1]==7)||(op==32&&a[1]==8)){
            size_t old=find(&m,op,0);assert(!memcmp(a,m.w+old,(a[0]>>16)*4));kept_shared++;
        }
        if(op==59&&a[2]==11)assert(a[1]==8);
        if(op==61&&a[2]==19)assert(a[1]==7);
    }
    assert(kept_other==1&&rewritten==samples&&kept_shared==3);checks++;
    emit(dir,name,m.w,m.n);char converted[256];snprintf(converted,sizeof converted,"%s-converted",name);emit(dir,converted,out,n);free(out);
}
static void unit_tests(const char *dir) {
    Module base=fixture(),m=base;accept(m,dir,"shared-textures",1,1);
    /* Optional explicit None mask is emitted by the actual translator. */
    size_t at=find(&m,87,0);m.w[at]=I(6,87);INSERT(m,at+5,0);accept(m,dir,"explicit-none-mask",1,1);
    m=base;INSERT(m,find(&m,71,0),I(3,16),16,9);reject(m,dir,"reject-early-fragment-tests",1);
    m=base;at=find(&m,87,0);m.w[at]=I(7,87);INSERT(m,at+5,1,13);reject(m,dir,"reject-bias",1);
    m=base;m.w[3]=28;INSERT(m,find(&m,54,0),I(4,21),23,32,1,I(4,23),24,23,2,
        I(4,43),23,25,0,I(4,43),23,26,1,I(5,44),24,27,26,25);
    at=find(&m,87,0);m.w[at]=I(7,87);INSERT(m,at+5,8,27);reject(m,dir,"reject-neighbor-offset",1);
    m=base;m.w[3]=24;at=find(&m,87,0);INSERT(m,at,I(4,83),7,23,18);m.w[at+4+3]=23;reject(m,dir,"reject-image-alias",1);
    m=base;m.w[3]=24;m.w[find(&m,25,0)+4]=1;at=find(&m,87,0);
    m.w[at]=I(6,89);m.w[at+1]=3;INSERT(m,at+5,13);
    INSERT(m,at+6,I(7,80),4,23,20,20,20,20);m.w[find(&m,129,0)+3]=23;
    reject(m,dir,"reject-dref",1);
    m=base;m.w[3]=25;INSERT(m,find(&m,54,0),I(4,21),23,32,1,I(4,43),23,24,0);
    at=find(&m,87,0);m.w[at]=I(6,96);INSERT(m,at+5,24);reject(m,dir,"reject-gather",1);
    m=base;m.w[3]=24;INSERT(m,5,I(2,17),50);INSERT(m,find(&m,87,0),I(5,105),5,23,18,15);
    reject(m,dir,"reject-query-lod",1);
    m=base;m.w[3]=28;INSERT(m,find(&m,54,0),I(4,21),24,32,1,I(4,23),25,24,2,
        I(4,43),24,26,0,I(5,44),25,27,26,26);
    at=find(&m,87,0);INSERT(m,at,I(4,100),6,23,18);at+=4;m.w[at]=I(7,95);m.w[at+3]=23;m.w[at+4]=27;INSERT(m,at+5,2,26);
    reject(m,dir,"reject-fetch",1);
    m=base;m.w[3]=25;INSERT(m,find(&m,59,0),I(4,21),23,32,1,I(4,23),24,23,2);accept(m,dir,"reuse-int-vector-types",1,1);
    m=base;INSERT(m,5,I(2,17),40);accept(m,dir,"reuse-input-capability",1,1);
    m=base;m.w[3]=26;INSERT(m,5,I(2,17),40);
    INSERT(m,find(&m,19,0),I(4,71),25,34,1,I(4,71),25,33,9,I(4,71),25,43,5);
    INSERT(m,find(&m,59,0),I(9,25),23,3,6,0,0,0,2,0,I(4,32),24,0,23,I(4,59),24,25,0);
    accept(m,dir,"reuse-subpass-types",1,1);
    m=base;m.w[find(&m,25,0)+5]=1;reject(m,dir,"array",0);
    m=base;m.w[find(&m,25,0)+6]=1;reject(m,dir,"multisample",0);
    m=base;m.w[find(&m,25,0)+3]=2;reject(m,dir,"dim3d",0);
    m=base;m.w[find(&m,25,0)+7]=2;reject(m,dir,"storage-image",0);
    m=base;m.w[find(&m,15,0)+1]=0;reject(m,dir,"vertex-stage",0);
    m=base;INSERT(m,find(&m,19,0),I(4,71),10,43,0);reject(m,dir,"already-input",0);
    m=base;INSERT(m,find(&m,19,0),I(4,71),10,33,3);reject(m,dir,"duplicate-binding",0);
    m=base;at=find(&m,71,3);m.w[at+3]=3;reject(m,dir,"duplicate-target",0);
    m=base;INSERT(m,find(&m,19,0),I(2,73),22);reject(m,dir,"decoration-group",0);
    m=base;m.w[find(&m,61,0)]=I(3,61);reject(m,dir,"truncated-load",0);
    m=base;m.w[m.n-1]=0;reject(m,dir,"zero-word-count",0);
    m=base;m.w[3]=1000000;reject(m,dir,"new-id-budget",0);
    for(size_t n=0;n<base.n;n++){m=base;m.n=n;reject(m,NULL,"truncated",0);}
    uint32_t rng=0x12345678u;
    for(unsigned i=0;i<1024;i++){
        m=base;rng=rng*1664525u+1013904223u;size_t pos=rng%m.n;
        rng=rng*1664525u+1013904223u;m.w[pos]^=rng;
        size_t n=0;NvnVkDepthInputInfo info;const char *error=NULL;
        uint32_t *out=nvn_vk_depth_input_variant(m.w,m.n,1,3,0,&n,&info,&error);
        if(out){assert(n>=m.n&&out[3]==m.w[3]+info.new_id_count);free(out);}
        else assert(!n&&error);
        checks++;
    }
    printf("depth input syntax variant: %u checks passed; no runtime path enabled\n",checks);
}
static uint32_t *read_words(const char *path,size_t *n) {
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);assert(bytes>=20&&bytes%4==0);rewind(f);
    uint32_t *p=malloc((size_t)bytes);assert(p);*n=(size_t)bytes/4;assert(fread(p,4,*n,f)==*n);assert(!fclose(f));return p;
}
static Module guard_fixture(void) {
    Module m=fixture();m.w[3]=25;
    INSERT(m,find(&m,19,0),I(4,71),23,11,15);
    INSERT(m,find(&m,59,0),I(4,32),24,1,4,I(4,59),24,23,1);
    size_t at=find(&m,15,0);m.w[at]=I(7,15);INSERT(m,at+6,23);return m;
}
/* Execute the generated guard's scalar/vector SSA without executing the source
 * shader. Compare it with a separately written nextafterf reference, including
 * nonfinite and strict-boundary inputs; this catches opcode/order mistakes. */
static unsigned execute_guard(const uint32_t *w,size_t n,const NvnVkDepthInputGuardInfo *info,
        const float uv[2],const float fc[2]) {
    uint32_t (*v)[4]=calloc(w[3],sizeof *v),*width=calloc(w[3],4);
    unsigned stride=26;for(size_t at=5;at<n;at+=w[at]>>16)if((w[at]&65535u)==151)stride=65;
    assert(v&&width);uint32_t added=w[3]-stride*info->guard_count,flag=99;
    memcpy(v[info->first_coordinate_id],uv,8);
    for(size_t at=5;at<n;at+=w[at]>>16){const uint32_t *a=w+at;unsigned op=a[0]&65535u;
        if(op==20||op==21||op==22)width[a[1]]=1;
        if(op==23)width[a[1]]=a[3];
        if(op==30&&(a[0]>>16)==4)width[a[1]]=width[a[2]]+width[a[3]];
        if(op==43)v[a[2]][0]=a[3];
        if(op==44)for(unsigned j=0;j<width[a[1]];j++)v[a[2]][j]=v[a[j+3]][0];
        if((a[0]>>16)<3||a[2]<added)continue;
        unsigned count=width[a[1]];
        if(op==61&&a[3]==info->fragcoord_variable_id){memcpy(v[a[2]],fc,8);memcpy(v[info->first_coordinate_id],uv,8);continue;}
        if(op==79){for(unsigned j=0;j<count;j++)v[a[2]][j]=v[a[3]][a[j+5]];continue;}
        if(op==124){memcpy(v[a[2]],v[a[3]],count*4);continue;}
        if(op==155){v[a[2]][0]=v[a[3]][0]&&v[a[3]][1];continue;}
        if(op==169){for(unsigned j=0;j<count;j++)v[a[2]][j]=v[a[v[a[3]][j]?4:5]][j];continue;}
        if(op==151){for(unsigned j=0;j<2;j++){
            uint64_t product=(uint64_t)v[a[3]][j]*v[a[4]][j];
            v[a[2]][j]=(uint32_t)product;v[a[2]][j+2]=(uint32_t)(product>>32);
        }continue;}
        if(op==81){assert(a[4]<2);memcpy(v[a[2]],v[a[3]]+a[4]*2,8);continue;}
        if(op==241){flag=v[a[6]][0];continue;}
        if(op!=133&&op!=131&&op!=129&&op!=199&&op!=128&&op!=184&&op!=167&&
           op!=130&&op!=194&&op!=196&&op!=197&&op!=174&&op!=178&&op!=176&&op!=170&&op!=166)continue;
        for(unsigned j=0;j<count;j++){
            uint32_t left=v[a[3]][j],right=v[a[4]][j],r=0;float x,y,z;
            memcpy(&x,&left,4);memcpy(&y,&right,4);
            if(op==133||op==131||op==129){volatile float rounded=op==133?x*y:op==131?x-y:x+y;z=rounded;memcpy(&r,&z,4);}
            if(op==199)r=left&right;
            if(op==128)r=left+right;
            if(op==184)r=x<y;
            if(op==167)r=left&&right;
            if(op==130)r=left-right;
            if(op==194||op==196){assert(right<32);r=op==194?left>>right:left<<right;}
            if(op==197)r=left|right;
            if(op==174)r=left>=right;
            if(op==178)r=left<=right;
            if(op==176)r=left<right;
            if(op==170)r=left==right;
            if(op==166)r=left||right;
            v[a[2]][j]=r;
        }
    }
    free(v);free(width);assert(flag<=1);return flag;
}
static void guard_numeric_tests(const uint32_t *out,size_t n,const NvnVkDepthInputGuardInfo *info,
        const NvnVkDepthInputGuard *g) {
    float uv[2]={.5f,.5f},fc[2]={640,360};
    assert(execute_guard(out,n,info,uv,fc)==0);checks++;
    fc[0]=640-g->threshold_x;assert(execute_guard(out,n,info,uv,fc)==1);checks++;
    uint32_t rng=0x7fa12345;
    for(unsigned i=0;i<512;i++){
        for(unsigned axis=0;axis<2;axis++){
            rng=rng*1664525u+1013904223u;
            uint32_t bits=i<256?rng:(i%16)*0x08000000u;
            memcpy(uv+axis,&bits,4);
            volatile float product=uv[axis]*(float)(axis?g->height:g->width);
            fc[axis]=i%3?product:0;
            if(i%4==0)fc[axis]+=g->threshold_x;
        }
        int pass=1;
        for(unsigned axis=0;axis<2;axis++){
            volatile float p=uv[axis]*(float)(axis?g->height:g->width),delta=p-fc[axis];
            volatile float ulp=nextafterf(fabsf(p),INFINITY)-fabsf(p),sum=fabsf(delta)+ulp;
            float bound=nextafterf(sum,INFINITY);
            pass&=isfinite(p)&&isfinite(sum)&&bound<(axis?g->threshold_y:g->threshold_x);
        }
        assert(execute_guard(out,n,info,uv,fc)==(unsigned)!pass);checks++;
    }
}
static void guard_tests(const char *dir) {
    Module m=guard_fixture();NvnVkDepthInputGuard g={1,63,1280,720,1.0f/512,1.0f/512};
    NvnVkDepthInputGuardInfo info;size_t n=0;const char *error=NULL;
    uint32_t *out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);
    if(!out)fprintf(stderr,"Unexpected guard rejection: %s\n",error);
    assert(out&&!error&&info.guard_count==1&&info.first_coordinate_id==15&&info.fragcoord_variable_id==23);
    assert(info.binding==63&&info.descriptor_set==1&&info.transform.binding==3);
    /* Frozen before the calibrated-ties API was added: preserve old bytes. */
    assert(n==424&&fingerprint(out,n)==UINT64_C(0xb0a9135cada45d3c));checks++;
    unsigned atomic=0,precise=0,read=0,kept_other=0,mul=0;
    for(size_t at=5;at<n;at+=out[at]>>16){const uint32_t *a=out+at;unsigned op=a[0]&65535;
        if(op==241)atomic++;
        if(op==71&&(a[0]>>16)==3&&a[2]==42)precise++;
        if(op==98)read++;
        if(op==133){assert(a[3]==15);mul++;}
        if(op==87){assert(a[2]==21&&a[3]==19&&a[4]==15);kept_other++;}
        assert(op!=252);
    }
    assert(atomic==1&&precise==4&&read==1&&kept_other==1&&mul==1);checks++;
    guard_numeric_tests(out,n,&info,&g);
    emit(dir,"guarded-original",m.w,m.n);emit(dir,"guarded-converted",out,n);free(out);
    for(unsigned variant=0;variant<3;variant++){
        m=guard_fixture();
        const char *name=variant==0?"guarded-multiple-samples":variant==1?"guarded-spv14-interface":"guarded-type-reuse";
        if(variant==0){
            m.w[3]=27;
            INSERT(m,find(&m,129,0),I(4,61),7,25,10,I(5,87),4,26,25,15);
        }else if(variant==1){
            m.w[1]=0x10400;size_t at=find(&m,15,0);m.w[at]=I(9,15);INSERT(m,at+7,10,11);
        }else{
            m.w[3]=30;INSERT(m,find(&m,54,0),I(2,20),25,I(4,21),26,32,0,
                I(4,23),27,25,2,I(4,23),28,26,2,I(4,32),29,12,26);
        }
        out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);
        assert(out&&!error&&info.guard_count==(variant==0?2u:1u));
        unsigned writes=0,interface_flags=0;
        for(size_t at=5;at<n;at+=out[at]>>16){const uint32_t *a=out+at;
            if((a[0]&65535u)==241)writes++;
            if((a[0]&65535u)==15)for(unsigned j=5;j<(a[0]>>16);j++)interface_flags+=a[j]==info.flag_variable_id;
        }
        assert(writes==info.guard_count&&interface_flags==(variant==1?1u:0u));checks++;
        emit(dir,name,out,n);free(out);
    }
    m=fixture();out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);assert(!out&&!n&&error);checks++;
    m=guard_fixture();g.binding=4;out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);assert(!out&&!n&&error);checks++;
    g.binding=63;g.threshold_x=0;out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);assert(!out&&!n&&error);checks++;
    uint32_t nan=0x7fc00000;memcpy(&g.threshold_x,&nan,4);out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);assert(!out&&!n&&error);checks++;
    g.threshold_x=-.001f;out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);assert(!out&&!n&&error);checks++;
    g.threshold_x=.001f;g.width=0;out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);assert(!out&&!n&&error);checks++;
    g.width=1280;m.w[1]=0x10000;out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);assert(!out&&!n&&error);checks++;
    g.threshold_x=1.0f/512;m=guard_fixture();
    for(size_t cut=0;cut<m.n;cut++){
        out=nvn_vk_depth_input_guarded_variant(m.w,cut,1,3,2,&g,&n,&info,&error);
        assert(!out&&!n&&error);checks++;
    }
    uint32_t rng=0x87654321;
    for(unsigned i=0;i<1024;i++){
        m=guard_fixture();rng=rng*1664525u+1013904223u;size_t pos=rng%m.n;
        rng=rng*1664525u+1013904223u;m.w[pos]^=rng;
        out=nvn_vk_depth_input_guarded_variant(m.w,m.n,1,3,2,&g,&n,&info,&error);
        if(out){assert(n>=m.n&&info.guard_count==1);free(out);}else assert(!n&&error);
        checks++;
    }
}
static int integer_reference(const float uv[2],const float fc[2],int calibrated_ties) {
    for(unsigned axis=0;axis<2;axis++){
        uint32_t bits;memcpy(&bits,uv+axis,4);unsigned exponent=bits>>23;
        unsigned extent=axis?720:1280;
        if(exponent<115||exponent>127||!isfinite(fc[axis])||fc[axis]<.5f||fc[axis]>=extent)return 0;
        double half=(double)fc[axis]*2;
        if(half!=floor(half)||!((uint32_t)half&1u))return 0;
        /* A binary32 significand times a <=11-bit integer fits exactly in
         * binary64; subtracting a nearby half-integer is also exact here. */
        double delta=(double)uv[axis]*extent-(double)fc[axis];
        if(calibrated_ties?!(delta>=-1.0/512&&delta<=1.0/512):!(delta>-1.0/512&&delta<1.0/512))return 0;
    }return 1;
}
static int exact_reference(const float uv[2],const float fc[2]) {return integer_reference(uv,fc,0);}
static void exact_guard_tests(const char *dir) {
    Module m=guard_fixture();NvnVkDepthInputGuard g={1,63,1280,720,1.0f/512,1.0f/512};
    NvnVkDepthInputGuardInfo info;size_t n=0;const char *error=NULL;
    uint32_t *out=nvn_vk_depth_input_exact_guarded_variant(m.w,m.n,1,3,0,&g,&n,&info,&error);
    if(!out)fprintf(stderr,"Unexpected exact guard rejection: %s\n",error);
    assert(out&&!error&&info.guard_count==1&&info.first_coordinate_id==15);
    assert(n==828&&fingerprint(out,n)==UINT64_C(0x0d38105a057cdc7d));checks++;
    unsigned mul=0,atomic=0;
    for(size_t at=5;at<n;at+=out[at]>>16){const uint32_t *a=out+at;unsigned op=a[0]&65535;
        mul+=op==151;atomic+=op==241;
        if((a[0]>>16)>=3&&a[2]>=out[3]-65*info.guard_count)
            assert(op!=129&&op!=131&&op!=133&&op!=109&&op!=110);
        assert(op!=252);
        if(op==21||op==22)assert(a[2]!=64);
    }
    assert(mul==1&&atomic==1);checks++;
    emit(dir,"exact-guarded",out,n);
    unsigned boundaries=0,inside=0,outside=0;
    for(unsigned axis=0;axis<2;axis++)for(unsigned pixel=0;pixel<(axis?720u:1280u);pixel++){
        float fc[2]={640.5f,360.5f},uv[2]={640.5f/1280,360.5f/720};fc[axis]=pixel+.5f;
        unsigned extent=axis?720:1280;uv[axis]=fc[axis]/extent;
        assert(execute_guard(out,n,&info,uv,fc)==0);checks++;
        for(int edge=-1;edge<=1;edge+=2){
            float nearest=(float)(((double)fc[axis]+edge/512.0)/extent);
            float candidates[]={nextafterf(nearest,-INFINITY),nearest,nextafterf(nearest,INFINITY)};
            for(unsigned j=0;j<3;j++){
                uv[axis]=candidates[j];int pass=exact_reference(uv,fc);
                double delta=(double)uv[axis]*extent-fc[axis];
                if(fabs(delta)==1.0/512){assert(!pass);boundaries++;}
                inside+=pass;outside+=!pass;
                assert(execute_guard(out,n,&info,uv,fc)==(unsigned)!pass);checks++;
            }
        }
    }
    assert(boundaries&&inside&&outside);
    { /* APK116 actual draw: x lies exactly on the negative strict boundary.
       * GPU sample==fetch here does not authorize accepting the boundary. */
        uint32_t bits[2]={1057495424u,1059353023u};float uv[2],fc[2]={680.5f,462.5f};
        memcpy(uv,bits,8);assert((double)uv[0]*1280-fc[0]==-1.0/512);
        assert(!exact_reference(uv,fc)&&execute_guard(out,n,&info,uv,fc)==1);checks++;
    }
    uint32_t rng=0x13fae914;
    for(unsigned i=0;i<1024;i++){
        float uv[2],fc[2];
        for(unsigned axis=0;axis<2;axis++){
            rng=rng*1664525u+1013904223u;uint32_t bits=rng;memcpy(uv+axis,&bits,4);
            rng=rng*1664525u+1013904223u;
            if(i<512){bits=rng;memcpy(fc+axis,&bits,4);}else fc[axis]=(rng%(axis?720:1280))+.5f;
        }
        assert(execute_guard(out,n,&info,uv,fc)==(unsigned)!exact_reference(uv,fc));checks++;
    }
    float uv[2]={.5f,.5f},fc[2]={640.5f,360.5f};
    const float bad_fc[]={0,-.5f,1,640.25f,1280.5f,INFINITY,NAN};
    for(unsigned i=0;i<sizeof bad_fc/sizeof *bad_fc;i++){
        fc[0]=bad_fc[i];assert(execute_guard(out,n,&info,uv,fc)==1);checks++;
    }
    free(out);
    for(unsigned variant=0;variant<3;variant++){
        m=guard_fixture();
        const char *name=variant==0?"exact-multiple-samples":variant==1?"exact-spv14-interface":"exact-type-reuse";
        if(variant==0){m.w[3]=27;INSERT(m,find(&m,129,0),I(4,61),7,25,10,I(5,87),4,26,25,15);}
        if(variant==1){m.w[1]=0x10400;size_t at=find(&m,15,0);m.w[at]=I(9,15);INSERT(m,at+7,10,11);}
        if(variant==2){m.w[3]=30;INSERT(m,find(&m,54,0),I(2,20),25,I(4,21),26,32,0,
            I(4,23),27,25,2,I(4,23),28,26,2,I(4,32),29,12,26);}
        out=nvn_vk_depth_input_exact_guarded_variant(m.w,m.n,1,3,0,&g,&n,&info,&error);
        assert(out&&!error&&info.guard_count==(variant==0?2u:1u));checks++;
        emit(dir,name,out,n);free(out);
    }
    m=guard_fixture();
    for(unsigned invalid=0;invalid<3;invalid++){
        g=(NvnVkDepthInputGuard){1,63,1280,720,1.0f/512,1.0f/512};
        if(invalid==0)g.width=1279;
        if(invalid==1)g.height=721;
        if(invalid==2)g.threshold_x=nextafterf(g.threshold_x,INFINITY);
        out=nvn_vk_depth_input_exact_guarded_variant(m.w,m.n,1,3,0,&g,&n,&info,&error);
        assert(!out&&!n&&error);checks++;
    }
    g=(NvnVkDepthInputGuard){1,63,1280,720,1.0f/512,1.0f/512};
    for(unsigned i=0;i<1024;i++){
        m=guard_fixture();rng=rng*1664525u+1013904223u;size_t pos=rng%m.n;
        rng=rng*1664525u+1013904223u;m.w[pos]^=rng;
        out=nvn_vk_depth_input_exact_guarded_variant(m.w,m.n,1,3,0,&g,&n,&info,&error);
        if(out){assert(n>=m.n&&info.guard_count==1);free(out);}else assert(!n&&error);
        checks++;
    }
    printf("exact integer interval guard: %u strict equality cases rejected; %u inside, %u outside boundary probes\n",boundaries,inside,outside);
}
static void calibrated_ties_tests(const char *dir) {
    Module m=guard_fixture();NvnVkDepthInputGuard g={1,63,1280,720,1.0f/512,1.0f/512};
    NvnVkDepthInputGuardInfo info,strict_info;size_t n=0,strict_n=0;const char *error=NULL;
    uint32_t *out=nvn_vk_depth_input_calibrated_ties_guarded_variant(m.w,m.n,1,3,0,&g,&n,&info,&error);
    uint32_t *strict=nvn_vk_depth_input_exact_guarded_variant(m.w,m.n,1,3,0,&g,&strict_n,&strict_info,&error);
    assert(out&&strict&&!error&&n==strict_n);unsigned differences=0;
    for(size_t at=5;at<n;at+=out[at]>>16)for(unsigned j=0;j<(out[at]>>16);j++)if(out[at+j]!=strict[at+j]){
        assert(j==0&&strict[at]==I(5,176)&&out[at]==I(5,178));differences++;
    }
    assert(differences==2);checks++;
    emit(dir,"calibrated-ties",out,n);
    unsigned ties=0,inside=0,outside=0,next_outside=0;
    for(unsigned axis=0;axis<2;axis++)for(unsigned pixel=0;pixel<(axis?720u:1280u);pixel++){
        float fc[2]={640.5f,360.5f},uv[2]={640.5f/1280,360.5f/720};fc[axis]=pixel+.5f;
        unsigned extent=axis?720:1280;
        for(int edge=-1;edge<=1;edge+=2){
            float nearest=(float)(((double)fc[axis]+edge/512.0)/extent);
            float candidates[]={nextafterf(nearest,-INFINITY),nearest,nextafterf(nearest,INFINITY)};
            for(unsigned j=0;j<3;j++){
                uv[axis]=candidates[j];int pass=integer_reference(uv,fc,1);
                double delta=(double)uv[axis]*extent-fc[axis];
                if(fabs(delta)==1.0/512){
                    assert(pass&&execute_guard(out,n,&info,uv,fc)==0&&execute_guard(strict,strict_n,&strict_info,uv,fc)==1);
                    float saved=uv[axis];uv[axis]=nextafterf(saved,edge<0?-INFINITY:INFINITY);
                    assert(!integer_reference(uv,fc,1)&&execute_guard(out,n,&info,uv,fc)==1);
                    uv[axis]=saved;ties++;next_outside++;
                }
                inside+=pass;outside+=!pass;
                assert(execute_guard(out,n,&info,uv,fc)==(unsigned)!pass);checks++;
            }
        }
    }
    assert(ties==544&&next_outside==544&&inside&&outside);
    {uint32_t bits[2]={1057495424u,1059353023u};float uv[2],fc[2]={680.5f,462.5f};memcpy(uv,bits,8);
        assert(execute_guard(out,n,&info,uv,fc)==0&&execute_guard(strict,strict_n,&strict_info,uv,fc)==1);checks++;}
    uint32_t rng=0x48a7c5ba;
    for(unsigned i=0;i<1024;i++){
        float uv[2],fc[2];
        for(unsigned axis=0;axis<2;axis++){
            rng=rng*1664525u+1013904223u;uint32_t bits=rng;memcpy(uv+axis,&bits,4);
            rng=rng*1664525u+1013904223u;
            if(i<512){bits=rng;memcpy(fc+axis,&bits,4);}else fc[axis]=(rng%(axis?720:1280))+.5f;
        }
        assert(execute_guard(out,n,&info,uv,fc)==(unsigned)!integer_reference(uv,fc,1));checks++;
    }
    free(strict);free(out);
    for(unsigned invalid=0;invalid<4;invalid++){
        g=(NvnVkDepthInputGuard){1,63,1280,720,1.0f/512,1.0f/512};
        if(invalid==0)g.width=1281;
        if(invalid==1)g.height=719;
        if(invalid==2)g.threshold_x=nextafterf(g.threshold_x,INFINITY);
        if(invalid==3)g.threshold_y=nextafterf(g.threshold_y,0);
        out=nvn_vk_depth_input_calibrated_ties_guarded_variant(m.w,m.n,1,3,0,&g,&n,&info,&error);
        assert(!out&&!n&&error);checks++;
    }
    g=(NvnVkDepthInputGuard){1,63,1280,720,1.0f/512,1.0f/512};
    for(unsigned i=0;i<1024;i++){
        m=guard_fixture();rng=rng*1664525u+1013904223u;size_t pos=rng%m.n;
        rng=rng*1664525u+1013904223u;m.w[pos]^=rng;
        out=nvn_vk_depth_input_calibrated_ties_guarded_variant(m.w,m.n,1,3,0,&g,&n,&info,&error);
        if(out){assert(n>=m.n&&info.guard_count==1);free(out);}else assert(!n&&error);
        checks++;
    }
    printf("calibrated ties guard: %u exact ties accepted; %u adjacent outside floats rejected; %u inside, %u outside boundary probes\n",ties,next_outside,inside,outside);
}
int main(int argc,char **argv) {
    if(argc==7&&!strcmp(argv[1],"--transform")){
        size_t n=0,outn=0;uint32_t *in=read_words(argv[2],&n);NvnVkDepthInputInfo info;const char *error=NULL;
        uint32_t *out=nvn_vk_depth_input_variant(in,n,(uint32_t)strtoul(argv[4],NULL,0),(uint32_t)strtoul(argv[5],NULL,0),(uint32_t)strtoul(argv[6],NULL,0),&outn,&info,&error);
        if(!out){fprintf(stderr,"%s\n",error);free(in);return 1;}FILE *f=fopen(argv[3],"wb");assert(f);assert(fwrite(out,4,outn,f)==outn);assert(!fclose(f));
        printf("variant set=%u binding=%u index=%u loads=%u samples=%u new_ids=%u\n",info.descriptor_set,info.binding,info.input_attachment_index,info.load_count,info.sample_count,info.new_id_count);free(in);free(out);return 0;
    }
    if(argc==7&&(!strcmp(argv[1],"--guard-transform")||!strcmp(argv[1],"--exact-guard-transform")||!strcmp(argv[1],"--calibrated-ties-transform"))){
        size_t n=0,outn=0;uint32_t *in=read_words(argv[2],&n);NvnVkDepthInputGuardInfo info;const char *error=NULL;
        NvnVkDepthInputGuard guard={(uint32_t)strtoul(argv[6],NULL,0),63,1280,720,1.0f/512,1.0f/512};
        int mode=!strcmp(argv[1],"--calibrated-ties-transform")?2:!strcmp(argv[1],"--exact-guard-transform");
        uint32_t *out=nvn_vk_depth_input_guarded_variant_impl(in,n,(uint32_t)strtoul(argv[4],NULL,0),(uint32_t)strtoul(argv[5],NULL,0),0,&guard,&outn,&info,&error,mode);
        if(!out){fprintf(stderr,"%s\n",error);free(in);return 1;}FILE *f=fopen(argv[3],"wb");assert(f);assert(fwrite(out,4,outn,f)==outn);assert(!fclose(f));
        printf("guard variant binding=%u guards=%u original_coord=%u\n",info.binding,info.guard_count,info.first_coordinate_id);free(in);free(out);return 0;
    }
    assert(argc<=2);unit_tests(argc==2?argv[1]:NULL);guard_tests(argc==2?argv[1]:NULL);exact_guard_tests(argc==2?argv[1]:NULL);calibrated_ties_tests(argc==2?argv[1]:NULL);printf("including optional guards: %u checks passed\n",checks);return 0;
}
