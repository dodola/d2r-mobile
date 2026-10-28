/* 使用合成常量直接运行 Vulkan 相机接入，覆盖预先发布 GPU 写入、剔除例外和帧状态。
 * Synthetic constants exercise the Vulkan camera integration directly, including GPU-write publication, culling exceptions and frame state. */
#define main camera_math_main
#include "t_cam.cpp"
#undef main
#include <assert.h>
#include <stdlib.h>
typedef struct {uint64_t addr,size;} Range;
typedef struct {unsigned kind;int slot;} Binding;
typedef struct {char key[17];struct {unsigned nb,stage;} h;Binding b[2];} VShader;
static struct {uint64_t present_count;struct {Range uniform[6][16];} state;} G;
static struct {int nwrites;void *write_cpu[2];Range writes[2];} S;
static int mode=1,publications,barriers,commits,begins,turns;
static double timestamp=1;
static uint64_t dm_android_nvn_clock_ns(void){return uint64_t(timestamp*1e9);}
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return a<b+m&&b<a+n;}
static void all_barrier(void){barriers++;}
static void mtl_sub_commit(void){commits++;}
static void mtl_sub_begin(void){begins++;S.nwrites=0;}
extern "C" {
int dm_cam_mode(void){return mode;}
int dm_cam_want_dir(float *,float *){return 0;}
float dm_cam_take_turn(void){turns++;return .1f;}
double dm_cam_turn_age(void){return 0;}
float dm_cam_tilt_get(void){return 0;}
void dm_cam_tilt_clamp(float,float){}
int dm_cam_clearance(float,float,float *d,float *,float *){*d=5;return 1;}
void dm_cam_publish(const float *,const float *,const float *,const float *,float,float,int m){assert(m==mode);publications++;}
}
#include "../nvn/nvn_vk_cam_runtime.inc"
int main(void){
    uint8_t source[608],bytes[608];original(source,100,200);memcpy(bytes,source,sizeof bytes);
    Range range={(uint64_t)(uintptr_t)bytes,sizeof bytes};assert(camera_candidate(range,1));assert(!camera_candidate(range,0));
    assert(camera_patch(bytes,sizeof bytes)&&memcmp(source,bytes,sizeof bytes));assert(publications==1&&turns==1&&CAM.state.have_eye);
    memcpy(bytes,source,sizeof bytes);assert(camera_patch(bytes,sizeof bytes)&&turns==1);
    G.present_count++;timestamp+=1./60;memcpy(bytes,source,sizeof bytes);assert(camera_patch(bytes,sizeof bytes)&&turns==2);
    uint8_t scene[1440]={0};const float dir[3]={-.61237f,-.5f,-.61237f};memcpy(scene+32,dir,12);assert(camera_patch(scene,sizeof scene));
    mode=2;assert(camera_mode()==2&&!CAM.state.have_eye);memcpy(bytes,source,sizeof bytes);assert(camera_patch(bytes,sizeof bytes));
    uint8_t invalid[608]={0};assert(!camera_patch(invalid,sizeof invalid));
    VShader shader={};strcpy(shader.key,"aaaaaaaaaaaaaaaa");shader.h.nb=1;shader.b[0]={2,0};G.state.uniform[0][0]=range;
    S.nwrites=1;S.writes[0]=range;camera_preflight(&shader,NULL);assert(commits==1&&begins==1&&barriers==1);
    S.nwrites=1;strcpy(shader.key,"1188eaedc8db3285");camera_preflight(&shader,NULL);assert(commits==1);
    strcpy(shader.key,"aaaaaaaaaaaaaaaa");S.write_cpu[0]=bytes;camera_preflight(&shader,NULL);assert(commits==1);
    mode=0;assert(!camera_candidate(range,1));S.write_cpu[0]=NULL;camera_preflight(&shader,NULL);assert(commits==1);
    puts("CAM_RUNTIME PASS main_scene frame mode culling cpu_gpu_preflight");
}
