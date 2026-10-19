/* Actual NVN -> Vulkan replay and GPU pixels using externally supplied
 * original D2R shaders. Synthetic geometry/texture; never a game-frame claim. */
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_vk_shader_index.h"
#include "../../gfx/tex/nvn_deswizzle.h"
#include <stdatomic.h>
#include <math.h>
struct dm_flags g_flag;uint64_t g_t0_ns;
void *dm_android_native_window(void){return NULL;}
void dm_vk_set_shader_root(const char *);
void mtl_set_window_origin_mode(int);
void dm_vk_ui_ghost_request(int,float,float,const float *,float,float,int,const _Atomic uint64_t *,uint64_t);
static unsigned checks;
#define REQUIRE(x) do{checks++;if(!(x))die("UI ghost fixture line=%d",__LINE__);}while(0)
static uint8_t *memory,*command;static size_t used;
static void record(unsigned op,const void *p,size_t n){size_t size=(8+n+7)&~7u;REQUIRE(used+size<32768);op_hdr h={op,(uint32_t)size};memcpy(command+used,&h,8);memset(command+used+8,0,size-8);if(n)memcpy(command+used+8,p,n);used+=size;}
static NVNshaderData shader(const char *root,const char *key){
    char path[1200];snprintf(path,sizeof path,"%s/shader_index.bin",root);FILE *f=fopen(path,"rb");REQUIRE(f);
    REQUIRE(!fseek(f,0,SEEK_END));long size=ftell(f);REQUIRE(size>12);rewind(f);uint8_t *data=malloc(size);REQUIRE(data&&fread(data,1,size,f)==(size_t)size);fclose(f);
    unsigned h[3];memcpy(h,data,12);size_t cursor=12;nvn_shader_index_record r;
    for(unsigned i=0;i<h[2];i++){REQUIRE(nvn_shader_index_read(data,size,h[1],&cursor,&r));
        if(!memcmp(r.entry,key,16)){mtl_pool_add(data,size);return (NVNshaderData){(uint64_t)(uintptr_t)r.code,r.control};}}
    free(data);
    die("UI ghost original shader control missing %s",key);
}
static tex_t texture(unsigned offset,unsigned width,unsigned height,uint64_t serial){
    tex_t t={.d={.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=width,.height=height,.depth=1,.levels=1},.mem=memory+offset,.serial=serial};
    nvn_tex_layout(&t.d,&t.L);REQUIRE(t.L.size<=32768);return t;
}
static void draw_record(unsigned vertex,unsigned matrix){
    p_bind_vbuf v={.n=1,.r={{(uint64_t)(uintptr_t)(memory+vertex),80}}};record(OP_BIND_VBUF,&v,sizeof v);
    p_bind_buf u={0,0,(uint64_t)(uintptr_t)(memory+matrix),64};record(OP_BIND_UBO,&u,sizeof u);
    p_draw d={NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP,0,4};record(OP_DRAW,&d,sizeof d);
}
static void begin(const tex_t *target,const prog_t *program){
    used=0;record(OP_BEGIN,NULL,0);uint64_t counter=NVN_COUNTER_TYPE_SAMPLES_PASSED;record(OP_RESET_COUNTER,&counter,8);p_set_rt rt={.n=1,.color={target}};record(OP_SET_RT,&rt,sizeof rt);
    p_clear_color clear={.mask=15,.rgba={0,0,0,1}};record(OP_CLEAR_COLOR,&clear,sizeof clear);
    p_viewports vp={.n=1,.v={{0,0,64,64,0,1}}};record(OP_VIEWPORTS,&vp,sizeof vp);
    p_scissor sc={0,0,64,64};record(OP_SCISSOR,&sc,sizeof sc);
    p_bind_program p={program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT};record(OP_BIND_PROGRAM,&p,sizeof p);
    va_st attr[2]={{.fmt=NVN_FORMAT_RGBA32F},{.fmt=NVN_FORMAT_R32F,.offset=16}};
    p_bind_vattr va={.n=2};memcpy(va.st,attr,sizeof attr);record(OP_BIND_VATTR,&va,sizeof va);
    p_bind_vstream vs={.n=1,.st={{20,0}}};record(OP_BIND_VSTREAM,&vs,sizeof vs);
    p_bind_handles tex={.stage=1,.first=10,.n=1,.h={1}};record(OP_BIND_TEX,&tex,sizeof tex);
    p_bind_handles sampler={.stage=1,.first=0,.n=1,.h={1u<<20}};record(OP_BIND_SAMP,&sampler,sizeof sampler);
}
static void submit(void){record(OP_END,NULL,0);mtl_exec(command);mtl_sub_commit();}
static void read_pixels(const tex_t *t,uint8_t p[64*64*4]){NVNcopyRegion r={0,0,0,64,64,1};mtl_read_texels(t,0,0,&r,p,64*4,0);}
static int equal_pixel(const uint8_t *a,const uint8_t *b){return !memcmp(a,b,4);}
int main(int argc,char **argv){
    if(argc!=2)return 2;g_t0_ns=now_ns();REQUIRE(!posix_memalign((void **)&memory,16384,262144));memset(memory,0,262144);command=memory;
    dm_vk_set_shader_root(argv[1]);mtl_init();mtl_set_window_origin_mode(NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);mtl_pool_add(memory,262144);
    tex_t target=texture(65536,64,64,991),source=texture(98304,1,1,992);
    uint8_t color[4]={200,100,40,255};REQUIRE(!nvn_swizzle(&source.d,0,0,color,4,source.mem));mtl_set_tex_pool_n(2);mtl_reg_tex(1,&source,NULL);
    samp_par sampler={.minf=NVN_MIN_FILTER_NEAREST,.magf=NVN_MAG_FILTER_NEAREST,.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE,.lod_max=1000};mtl_reg_samp(1,&sampler);
    prog_t program={.nstage=2,.serial=991,.sd={shader(argv[1],"f88d27f131d71049"),shader(argv[1],"3b08a1bafbd9e2bf")}};
    float *m=(void *)(memory+32768);for(unsigned i=0;i<4;i++)m[i*5]=1;
    const float quad[20]={-.75f,-.75f,0,1,1,-.25f,-.75f,0,1,1,-.75f,-.25f,0,1,1,-.25f,-.25f,0,1,1};memcpy(memory+36864,quad,sizeof quad);
    uint8_t baseline[64*64*4],actual[sizeof baseline];
    mtl_sub_begin();begin(&target,&program);draw_record(36864,32768);submit();read_pixels(&target,baseline);
    unsigned lit=0,x0=64,y0=64,x1=0,y1=0;for(unsigned i=0;i<4096;i++)if(baseline[i*4]||baseline[i*4+1]||baseline[i*4+2]){lit++;if(i%64<x0)x0=i%64;if(i/64<y0)y0=i/64;if(i%64>x1)x1=i%64;if(i/64>y1)y1=i/64;}
    fprintf(stderr,"UI_GHOST_BASELINE_RECT %u,%u,%u,%u\n",x0,y0,x1,y1);
    fprintf(stderr,"UI_GHOST_BASELINE lit=%u sample=%u,%u,%u,%u\n",lit,baseline[(16*64+16)*4],baseline[(16*64+16)*4+1],baseline[(16*64+16)*4+2],baseline[(16*64+16)*4+3]);REQUIRE(lit==256&&x0==8&&y0==8&&x1==23&&y1==23);
    _Atomic uint64_t live=1;const float grid[4]={.125f,.125f,.25f,.25f};
    for(unsigned test=0;test<4;test++){
        dm_vk_ui_ghost_request(1,.25f,.25f,grid,.75f,.75f,test==1,&live,1);
        mtl_sub_begin();begin(&target,&program);draw_record(36864,32768);record(OP_END,NULL,0);mtl_exec(command);
        if(test==2)atomic_store(&live,0); /* physical UP before the render pass ends */
        if(test==3){/* CPU storage may be reused before pass-tail replay. */memset(memory+36864,0,80);memset(memory+32768,0,64);memset(source.mem,0,source.L.size);}
        mtl_sub_counter((uint64_t)(uintptr_t)(memory+45056),NVN_COUNTER_TYPE_SAMPLES_PASSED);
        mtl_sub_commit();REQUIRE(*(uint64_t *)(memory+45056)==256);read_pixels(&target,actual);
        unsigned source_pixels=0,ghost_pixels=0;
        for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
            const uint8_t *p=actual+(y*64+x)*4,*b=baseline+(y*64+x)*4;
            if(x>=40&&x<56&&y>=40&&y<56){const uint8_t *expected=baseline+((y-32)*64+x-32)*4;
                REQUIRE(test==2?equal_pixel(p,b):equal_pixel(p,expected));ghost_pixels++;}
            else if(test==1&&x>=8&&x<24&&y>=8&&y<24){for(unsigned c=0;c<3;c++)REQUIRE(abs((int)p[c]-(int)lroundf(b[c]*.18f))<=1);REQUIRE(p[3]==b[3]);source_pixels++;}
            else REQUIRE(equal_pixel(p,b));
        }
        if(test==1)REQUIRE(source_pixels==256);REQUIRE(ghost_pixels==256);atomic_store(&live,1);
        memcpy(memory+36864,quad,sizeof quad);memset(m,0,64);for(unsigned i=0;i<4;i++)m[i*5]=1;REQUIRE(!nvn_swizzle(&source.d,0,0,color,4,source.mem));
    }
    /* Reuse program, viewport, scissor, blend and bindings without rebinding:
     * only clear the target and issue the following normal game's draw. */
    dm_vk_ui_ghost_request(0,0,0,NULL,0,0,0,NULL,0);mtl_sub_begin();used=0;p_clear_color clear={.mask=15,.rgba={0,0,0,1}};record(OP_CLEAR_COLOR,&clear,sizeof clear);draw_record(36864,32768);submit();read_pixels(&target,actual);REQUIRE(!memcmp(actual,baseline,sizeof actual));
    printf("ANDROID_UI_GHOST_GPU_OK checks=%u original_shader_draw=1 synthetic_fixture=1 game_frame=0 translated_pixels=256 shelf_dim_alpha=.82 physical_up_cancels=1 immutable_vertex_uniform=1 resident_texture_retained=1 guest_sample_counter_preserved=1 state_restored_without_rebind=1\n",checks);
    return 0;
}
