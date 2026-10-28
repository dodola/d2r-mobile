/* D2R 镜头常量副本，改编自 upstream dc5778d，原始客体内存与剔除矩阵保持原样。
 * D2R camera constant copies, adapted from upstream dc5778d; guest memory and culling matrices retain their original values. */
#include "nvn_vk_cam.h"
extern int dm_cam_mode(void) __attribute__((weak));
extern int dm_cam_want_dir(float *,float *) __attribute__((weak));
extern float dm_cam_take_turn(void) __attribute__((weak));
extern double dm_cam_turn_age(void) __attribute__((weak));
extern float dm_cam_tilt_get(void) __attribute__((weak));
extern void dm_cam_tilt_clamp(float,float) __attribute__((weak));
extern int dm_cam_clearance(float,float,float *,float *,float *) __attribute__((weak));
extern void dm_cam_publish(const float *,const float *,const float *,const float *,float,float,int) __attribute__((weak));
static struct {int mode; xbg_cam_state state;xbg_cam_params params;uint64_t patches;} CAM;
static int camera_mode(void) {
    int m=dm_cam_mode?dm_cam_mode():0;
    if(m<0||m>2)m=0;
    if(m!=CAM.mode){CAM.mode=m;xbg_cam_init(&CAM.state);CAM.params=xbg_cam_default_params(m);
#define CAM_ENV(field,key) do{const char *v=getenv(key);if(v){float value=strtof(v,NULL);if(isfinite(value))CAM.params.field=value;}}while(0)
        CAM_ENV(dist,"DM_CAM_DIST");CAM_ENV(h,"DM_CAM_H");CAM_ENV(look,"DM_CAM_LOOK");CAM_ENV(lookh,"DM_CAM_LOOKH");
        CAM_ENV(eye,"DM_CAM_EYE");CAM_ENV(fwd,"DM_CAM_FWD");CAM_ENV(pitch,"DM_CAM_PITCH");CAM_ENV(fov,"DM_CAM_FOV");
        CAM_ENV(tau,"DM_CAM_TAU");CAM_ENV(near_,"DM_CAM_NEAR");CAM_ENV(reach,"DM_CAM_REACH");
#undef CAM_ENV
    }
    return m;
}
static int camera_candidate(Range r,int allowed) {
    if(!allowed||camera_mode()<=0||!r.addr||r.size<608||r.size>4096)return 0;
    /* 签名只在 GPU 冲突等待、CPU 写入覆盖之后检查。
     * Check signatures only after GPU conflict waits and CPU write overlays. */
    return 1;
}
static int camera_patch(uint8_t *bytes,size_t size) {
    int main=xbg_cam_sig(bytes,size,size),scene=!main&&xbg_cam_sig10(bytes,size,size);
    if(!main&&!scene)return 0;
    if(main){
        uint8_t original[4096];memcpy(original,bytes,size);
        float px,py,pz,wx=0,wz=0;xbg_cam_ground(original,&px,&py,&pz);
        int want=dm_cam_want_dir?dm_cam_want_dir(&wx,&wz):0;
        uint64_t frame=G.present_count;float turn=0;
        if(frame!=CAM.state.frame){turn=dm_cam_take_turn?dm_cam_take_turn():0;CAM.state.hold=dm_cam_turn_age&&dm_cam_turn_age()<1;}
        double prior=CAM.state.t;
        int fresh=xbg_cam_follow(&CAM.state,px,pz,frame,dm_android_nvn_clock_ns()*1e-9,want,wx,wz,CAM.mode==2?1e9f:CAM.params.tau);
        if(fresh&&turn)xbg_cam_turn(&CAM.state,turn);
        if(fresh&&CAM.mode==1){float clear=0,hx=0,hz=0;
            if(dm_cam_clearance)dm_cam_clearance(-CAM.state.fx,-CAM.state.fz,&clear,&hx,&hz);
            xbg_cam_pull(&CAM.state,CAM.params.dist,clear,prior?CAM.state.t-prior:0);
        }
        CAM.state.tilt=dm_cam_tilt_get?dm_cam_tilt_get():0;
        float vp[16],inverse[16];xbg_cam_build(&CAM.state,original,bytes,CAM.mode,&CAM.params,vp,inverse);
        if(fresh&&dm_cam_tilt_clamp)dm_cam_tilt_clamp(CAM.state.tilt_lo,CAM.state.tilt_hi);
        if(dm_cam_publish)dm_cam_publish((const float*)(original+336),(const float*)(original+400),vp,inverse,px,pz,CAM.mode);
        if(CAM.patches++%600==0)fprintf(stderr,"[nvn-vulkan] CAMERA mode=%d frame=%llu player=%.1f,%.1f height=%.1f\n",CAM.mode,(unsigned long long)frame,px,pz,py);
    }else if(CAM.state.have_eye)xbg_cam_build10(&CAM.state,bytes);
    else return 0;
    return 1;
}
/* CPU 改矩阵前必须发布先前 GPU 写入，禁止用过期的 CPU 字节替代有效常量。
 * Publish earlier GPU writes before CPU matrix rewriting; never replace effective constants with stale CPU bytes. */
static void camera_preflight(VShader *a,VShader *b) {
    if(camera_mode()<=0)return;
    VShader *shaders[]={a,b};int wait=0;
    for(unsigned k=0;k<2&&!wait;k++)if(shaders[k]&&strcmp(shaders[k]->key,"1188eaedc8db3285")){
        VShader *s=shaders[k];
        for(unsigned i=0;i<s->h.nb&&!wait;i++)if(s->b[i].kind==2&&s->b[i].slot>=0&&s->b[i].slot<16){
            Range r=G.state.uniform[s->h.stage][s->b[i].slot];if(r.size<608||r.size>4096)continue;
            for(int j=0;j<S.nwrites&&!wait;j++)if(!S.write_cpu[j]&&overlap(r.addr,r.size,S.writes[j].addr,S.writes[j].size))wait=1;
        }
    }
    if(wait){all_barrier();mtl_sub_commit();mtl_sub_begin();}
}
