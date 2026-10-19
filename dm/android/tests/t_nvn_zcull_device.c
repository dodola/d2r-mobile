/* Standalone Android GPU regression through real NVN thunks and replay.
 * Requires the original offline D2R fixture shader cache, supplied externally.
 * Fixture geometry is not a game frame. See android_nvn_zcull_check.py.
 */

#include "hle.h"
#include "nvn.h"
#include "nvn_obj.h"
#include "nvn_dirty.h"
#include "nvn_count.h"
#include "nvn_vk_zcull.h"
int dm_vk_copy_test_depth(const tex_t *,float,unsigned);
#include <stdatomic.h>
#include <unistd.h>
struct dm_flags g_flag;
uint64_t g_t0_ns;
/* The standalone probe has no NativeActivity or presentation surface. */
void *dm_android_native_window(void) {return NULL;}
void crash_thread_init(void);
void dm_vk_set_shader_root(const char *);
static PFNNVNBOOTSTRAPLOADERPROC boot;
extern const hle_force_ent __start_dm_force[],__stop_dm_force[];
typedef struct {_Atomic uint64_t count;void *fn;} Slot;
extern Slot nvn_slot[NVN_NFUNC];
extern const char *nvn_names[NVN_NFUNC];
static void *forced(const char *name) {
    for(const hle_force_ent *e=__start_dm_force;e<__stop_dm_force;e++)if(!strcmp(e->name,name))return e->fn;
    die("NVN API fixture missing original forced entry %s",name);
}
static PFNNVNCOMMANDBUFFERADDCOMMANDMEMORYPROC p_CommandBufferAddCommandMemory;
static PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC p_CommandBufferBeginRecording;
static PFNNVNCOMMANDBUFFERBINDDEPTHSTENCILSTATEPROC p_CommandBufferBindDepthStencilState;
static PFNNVNCOMMANDBUFFERBINDIMAGESPROC p_CommandBufferBindImages;
static PFNNVNCOMMANDBUFFERBINDPROGRAMPROC p_CommandBufferBindProgram;
static PFNNVNCOMMANDBUFFERBINDSEPARATESAMPLERSPROC p_CommandBufferBindSeparateSamplers;
static PFNNVNCOMMANDBUFFERBINDSEPARATETEXTURESPROC p_CommandBufferBindSeparateTextures;
static PFNNVNCOMMANDBUFFERBINDSTORAGEBUFFERPROC p_CommandBufferBindStorageBuffer;
static PFNNVNCOMMANDBUFFERBINDUNIFORMBUFFERPROC p_CommandBufferBindUniformBuffer;
static PFNNVNCOMMANDBUFFERBINDVERTEXATTRIBSTATEPROC p_CommandBufferBindVertexAttribState;
static PFNNVNCOMMANDBUFFERBINDVERTEXBUFFERSPROC p_CommandBufferBindVertexBuffers;
static PFNNVNCOMMANDBUFFERBINDVERTEXSTREAMSTATEPROC p_CommandBufferBindVertexStreamState;
static PFNNVNCOMMANDBUFFERCLEARCOLORPROC p_CommandBufferClearColor;
static PFNNVNCOMMANDBUFFERCLEARDEPTHSTENCILPROC p_CommandBufferClearDepthStencil;
static PFNNVNCOMMANDBUFFERCLEARTEXTUREPROC p_CommandBufferClearTexture;
static PFNNVNCOMMANDBUFFERCOPYBUFFERTOBUFFERPROC p_CommandBufferCopyBufferToBuffer;
static PFNNVNCOMMANDBUFFERCOPYTEXTURETOTEXTUREPROC p_CommandBufferCopyTextureToTexture;
static PFNNVNCOMMANDBUFFERDISPATCHCOMPUTEPROC p_CommandBufferDispatchCompute;
static PFNNVNCOMMANDBUFFERDISPATCHCOMPUTEINDIRECTPROC p_CommandBufferDispatchComputeIndirect;
static PFNNVNCOMMANDBUFFERDRAWARRAYSPROC p_CommandBufferDrawArrays;
static PFNNVNCOMMANDBUFFERENDRECORDINGPROC p_CommandBufferEndRecording;
static PFNNVNCOMMANDBUFFERFINALIZEPROC p_CommandBufferFinalize;
static PFNNVNCOMMANDBUFFERINITIALIZEPROC p_CommandBufferInitialize;
static PFNNVNCOMMANDBUFFERRESTOREZCULLDATAPROC p_CommandBufferRestoreZCullData;
static PFNNVNCOMMANDBUFFERSAVEZCULLDATAPROC p_CommandBufferSaveZCullData;
static PFNNVNCOMMANDBUFFERSETDEPTHRANGEPROC p_CommandBufferSetDepthRange;
static PFNNVNCOMMANDBUFFERSETRENDERTARGETSPROC p_CommandBufferSetRenderTargets;
static PFNNVNCOMMANDBUFFERSETSCISSORPROC p_CommandBufferSetScissor;
static PFNNVNCOMMANDBUFFERSETVIEWPORTSPROC p_CommandBufferSetViewports;
static PFNNVNDEPTHSTENCILSTATESETDEFAULTSPROC p_DepthStencilStateSetDefaults;
static PFNNVNDEPTHSTENCILSTATESETDEPTHFUNCPROC p_DepthStencilStateSetDepthFunc;
static PFNNVNDEPTHSTENCILSTATESETDEPTHTESTENABLEPROC p_DepthStencilStateSetDepthTestEnable;
static PFNNVNDEPTHSTENCILSTATESETDEPTHWRITEENABLEPROC p_DepthStencilStateSetDepthWriteEnable;
static PFNNVNDEVICEBUILDERSETDEFAULTSPROC p_DeviceBuilderSetDefaults;
static PFNNVNDEVICEFINALIZEPROC p_DeviceFinalize;
static PFNNVNDEVICEINITIALIZEPROC p_DeviceInitialize;
static PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC p_MemoryPoolBuilderSetDefaults;
static PFNNVNMEMORYPOOLBUILDERSETDEVICEPROC p_MemoryPoolBuilderSetDevice;
static PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC p_MemoryPoolBuilderSetFlags;
static PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC p_MemoryPoolBuilderSetStorage;
static PFNNVNMEMORYPOOLFINALIZEPROC p_MemoryPoolFinalize;
static PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC p_MemoryPoolGetBufferAddress;
static PFNNVNMEMORYPOOLINITIALIZEPROC p_MemoryPoolInitialize;
static PFNNVNPROGRAMFINALIZEPROC p_ProgramFinalize;
static PFNNVNPROGRAMINITIALIZEPROC p_ProgramInitialize;
static PFNNVNPROGRAMSETSHADERSPROC p_ProgramSetShaders;
static PFNNVNQUEUEBUILDERSETDEFAULTSPROC p_QueueBuilderSetDefaults;
static PFNNVNQUEUEBUILDERSETDEVICEPROC p_QueueBuilderSetDevice;
static PFNNVNQUEUEFENCESYNCPROC p_QueueFenceSync;
static PFNNVNQUEUEFINALIZEPROC p_QueueFinalize;
static PFNNVNQUEUEFINISHPROC p_QueueFinish;
static PFNNVNQUEUEINITIALIZEPROC p_QueueInitialize;
static PFNNVNQUEUESUBMITCOMMANDSPROC p_QueueSubmitCommands;
static PFNNVNSYNCFINALIZEPROC p_SyncFinalize;
static PFNNVNSYNCINITIALIZEPROC p_SyncInitialize;
static PFNNVNSYNCWAITPROC p_SyncWait;
static PFNNVNTEXTUREBUILDERGETZCULLSTORAGESIZEPROC p_TextureBuilderGetZCullStorageSize;
static PFNNVNTEXTUREBUILDERSETDEFAULTSPROC p_TextureBuilderSetDefaults;
static PFNNVNTEXTUREBUILDERSETDEPTHPROC p_TextureBuilderSetDepth;
static PFNNVNTEXTUREBUILDERSETDEVICEPROC p_TextureBuilderSetDevice;
static PFNNVNTEXTUREBUILDERSETFORMATPROC p_TextureBuilderSetFormat;
static PFNNVNTEXTUREBUILDERSETHEIGHTPROC p_TextureBuilderSetHeight;
static PFNNVNTEXTUREBUILDERSETSTORAGEPROC p_TextureBuilderSetStorage;
static PFNNVNTEXTUREBUILDERSETTARGETPROC p_TextureBuilderSetTarget;
static PFNNVNTEXTUREBUILDERSETWIDTHPROC p_TextureBuilderSetWidth;
static PFNNVNTEXTUREFINALIZEPROC p_TextureFinalize;
static PFNNVNTEXTUREGETZCULLSTORAGESIZEPROC p_TextureGetZCullStorageSize;
static PFNNVNTEXTUREGETSTORAGESIZEPROC p_TextureGetStorageSize;
static PFNNVNTEXTUREINITIALIZEPROC p_TextureInitialize;
static PFNNVNTEXTUREREADTEXELSPROC p_TextureReadTexels;
static PFNNVNTEXTUREWRITETEXELSSTRIDEDPROC p_TextureWriteTexelsStrided;
static PFNNVNVERTEXATTRIBSTATESETDEFAULTSPROC p_VertexAttribStateSetDefaults;
static PFNNVNVERTEXATTRIBSTATESETFORMATPROC p_VertexAttribStateSetFormat;
static PFNNVNVERTEXATTRIBSTATESETSTREAMINDEXPROC p_VertexAttribStateSetStreamIndex;
static PFNNVNVERTEXSTREAMSTATESETDEFAULTSPROC p_VertexStreamStateSetDefaults;
static PFNNVNVERTEXSTREAMSTATESETSTRIDEPROC p_VertexStreamStateSetStride;
static void *control(const char *root,const char *key) {
    char path[1200];snprintf(path,sizeof path,"%s/shader_index.bin",root);FILE *f=fopen(path,"rb");if(!f)abort();
    unsigned h[3];if(fread(h,4,3,f)!=3)abort();
    for(unsigned i=0;i<h[2];i++){char k[16];unsigned stage;unsigned char *c=malloc(0x880);
        if(fread(k,1,16,f)!=16 || fread(&stage,4,1,f)!=1 || fread(c,1,0x880,f)!=0x880)abort();
        if(!memcmp(k,key,16)){fclose(f);return c;}free(c);}
    abort();
}
static void *allocate(unsigned long n,unsigned long alignment,void *user) {
    (void)user;void *p=NULL;return posix_memalign(&p,alignment,n)?NULL:p;
}
static void release(void *p,void *user){(void)user;free(p);}
static void *resize(void *p,unsigned long n,void *user){(void)user;return realloc(p,n);}
int main(int argc,char **argv) {
    if(argc!=4)return 2;crash_thread_init();g_t0_ns=now_ns();
    boot=(PFNNVNBOOTSTRAPLOADERPROC)forced("nvnBootstrapLoader");
    p_CommandBufferAddCommandMemory=(PFNNVNCOMMANDBUFFERADDCOMMANDMEMORYPROC)boot("nvnCommandBufferAddCommandMemory");
p_CommandBufferBeginRecording=(PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC)boot("nvnCommandBufferBeginRecording");
p_CommandBufferBindDepthStencilState=(PFNNVNCOMMANDBUFFERBINDDEPTHSTENCILSTATEPROC)boot("nvnCommandBufferBindDepthStencilState");
p_CommandBufferBindImages=(PFNNVNCOMMANDBUFFERBINDIMAGESPROC)boot("nvnCommandBufferBindImages");
p_CommandBufferBindProgram=(PFNNVNCOMMANDBUFFERBINDPROGRAMPROC)boot("nvnCommandBufferBindProgram");
p_CommandBufferBindSeparateSamplers=(PFNNVNCOMMANDBUFFERBINDSEPARATESAMPLERSPROC)boot("nvnCommandBufferBindSeparateSamplers");
p_CommandBufferBindSeparateTextures=(PFNNVNCOMMANDBUFFERBINDSEPARATETEXTURESPROC)boot("nvnCommandBufferBindSeparateTextures");
p_CommandBufferBindStorageBuffer=(PFNNVNCOMMANDBUFFERBINDSTORAGEBUFFERPROC)boot("nvnCommandBufferBindStorageBuffer");
p_CommandBufferBindUniformBuffer=(PFNNVNCOMMANDBUFFERBINDUNIFORMBUFFERPROC)boot("nvnCommandBufferBindUniformBuffer");
p_CommandBufferBindVertexAttribState=(PFNNVNCOMMANDBUFFERBINDVERTEXATTRIBSTATEPROC)boot("nvnCommandBufferBindVertexAttribState");
p_CommandBufferBindVertexBuffers=(PFNNVNCOMMANDBUFFERBINDVERTEXBUFFERSPROC)boot("nvnCommandBufferBindVertexBuffers");
p_CommandBufferBindVertexStreamState=(PFNNVNCOMMANDBUFFERBINDVERTEXSTREAMSTATEPROC)boot("nvnCommandBufferBindVertexStreamState");
p_CommandBufferClearColor=(PFNNVNCOMMANDBUFFERCLEARCOLORPROC)boot("nvnCommandBufferClearColor");
p_CommandBufferClearDepthStencil=(PFNNVNCOMMANDBUFFERCLEARDEPTHSTENCILPROC)boot("nvnCommandBufferClearDepthStencil");
p_CommandBufferClearTexture=(PFNNVNCOMMANDBUFFERCLEARTEXTUREPROC)boot("nvnCommandBufferClearTexture");
p_CommandBufferCopyBufferToBuffer=(PFNNVNCOMMANDBUFFERCOPYBUFFERTOBUFFERPROC)boot("nvnCommandBufferCopyBufferToBuffer");
p_CommandBufferCopyTextureToTexture=(PFNNVNCOMMANDBUFFERCOPYTEXTURETOTEXTUREPROC)boot("nvnCommandBufferCopyTextureToTexture");
p_CommandBufferDispatchCompute=(PFNNVNCOMMANDBUFFERDISPATCHCOMPUTEPROC)boot("nvnCommandBufferDispatchCompute");
p_CommandBufferDispatchComputeIndirect=(PFNNVNCOMMANDBUFFERDISPATCHCOMPUTEINDIRECTPROC)boot("nvnCommandBufferDispatchComputeIndirect");
p_CommandBufferDrawArrays=(PFNNVNCOMMANDBUFFERDRAWARRAYSPROC)boot("nvnCommandBufferDrawArrays");
p_CommandBufferEndRecording=(PFNNVNCOMMANDBUFFERENDRECORDINGPROC)boot("nvnCommandBufferEndRecording");
p_CommandBufferFinalize=(PFNNVNCOMMANDBUFFERFINALIZEPROC)boot("nvnCommandBufferFinalize");
p_CommandBufferInitialize=(PFNNVNCOMMANDBUFFERINITIALIZEPROC)boot("nvnCommandBufferInitialize");
p_CommandBufferRestoreZCullData=(PFNNVNCOMMANDBUFFERRESTOREZCULLDATAPROC)boot("nvnCommandBufferRestoreZCullData");
p_CommandBufferSaveZCullData=(PFNNVNCOMMANDBUFFERSAVEZCULLDATAPROC)boot("nvnCommandBufferSaveZCullData");
p_CommandBufferSetDepthRange=(PFNNVNCOMMANDBUFFERSETDEPTHRANGEPROC)boot("nvnCommandBufferSetDepthRange");
p_CommandBufferSetRenderTargets=(PFNNVNCOMMANDBUFFERSETRENDERTARGETSPROC)boot("nvnCommandBufferSetRenderTargets");
p_CommandBufferSetScissor=(PFNNVNCOMMANDBUFFERSETSCISSORPROC)boot("nvnCommandBufferSetScissor");
p_CommandBufferSetViewports=(PFNNVNCOMMANDBUFFERSETVIEWPORTSPROC)boot("nvnCommandBufferSetViewports");
p_DepthStencilStateSetDefaults=(PFNNVNDEPTHSTENCILSTATESETDEFAULTSPROC)boot("nvnDepthStencilStateSetDefaults");
p_DepthStencilStateSetDepthFunc=(PFNNVNDEPTHSTENCILSTATESETDEPTHFUNCPROC)boot("nvnDepthStencilStateSetDepthFunc");
p_DepthStencilStateSetDepthTestEnable=(PFNNVNDEPTHSTENCILSTATESETDEPTHTESTENABLEPROC)boot("nvnDepthStencilStateSetDepthTestEnable");
p_DepthStencilStateSetDepthWriteEnable=(PFNNVNDEPTHSTENCILSTATESETDEPTHWRITEENABLEPROC)boot("nvnDepthStencilStateSetDepthWriteEnable");
p_DeviceBuilderSetDefaults=(PFNNVNDEVICEBUILDERSETDEFAULTSPROC)boot("nvnDeviceBuilderSetDefaults");
p_DeviceFinalize=(PFNNVNDEVICEFINALIZEPROC)boot("nvnDeviceFinalize");
p_DeviceInitialize=(PFNNVNDEVICEINITIALIZEPROC)boot("nvnDeviceInitialize");
p_MemoryPoolBuilderSetDefaults=(PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC)boot("nvnMemoryPoolBuilderSetDefaults");
p_MemoryPoolBuilderSetDevice=(PFNNVNMEMORYPOOLBUILDERSETDEVICEPROC)boot("nvnMemoryPoolBuilderSetDevice");
p_MemoryPoolBuilderSetFlags=(PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC)boot("nvnMemoryPoolBuilderSetFlags");
p_MemoryPoolBuilderSetStorage=(PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC)boot("nvnMemoryPoolBuilderSetStorage");
p_MemoryPoolFinalize=(PFNNVNMEMORYPOOLFINALIZEPROC)boot("nvnMemoryPoolFinalize");
p_MemoryPoolGetBufferAddress=(PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC)boot("nvnMemoryPoolGetBufferAddress");
p_MemoryPoolInitialize=(PFNNVNMEMORYPOOLINITIALIZEPROC)boot("nvnMemoryPoolInitialize");
p_ProgramFinalize=(PFNNVNPROGRAMFINALIZEPROC)boot("nvnProgramFinalize");
p_ProgramInitialize=(PFNNVNPROGRAMINITIALIZEPROC)boot("nvnProgramInitialize");
p_ProgramSetShaders=(PFNNVNPROGRAMSETSHADERSPROC)boot("nvnProgramSetShaders");
p_QueueBuilderSetDefaults=(PFNNVNQUEUEBUILDERSETDEFAULTSPROC)boot("nvnQueueBuilderSetDefaults");
p_QueueBuilderSetDevice=(PFNNVNQUEUEBUILDERSETDEVICEPROC)boot("nvnQueueBuilderSetDevice");
p_QueueFenceSync=(PFNNVNQUEUEFENCESYNCPROC)boot("nvnQueueFenceSync");
p_QueueFinalize=(PFNNVNQUEUEFINALIZEPROC)boot("nvnQueueFinalize");
p_QueueFinish=(PFNNVNQUEUEFINISHPROC)boot("nvnQueueFinish");
p_QueueInitialize=(PFNNVNQUEUEINITIALIZEPROC)boot("nvnQueueInitialize");
p_QueueSubmitCommands=(PFNNVNQUEUESUBMITCOMMANDSPROC)boot("nvnQueueSubmitCommands");
p_SyncFinalize=(PFNNVNSYNCFINALIZEPROC)boot("nvnSyncFinalize");
p_SyncInitialize=(PFNNVNSYNCINITIALIZEPROC)boot("nvnSyncInitialize");
p_SyncWait=(PFNNVNSYNCWAITPROC)boot("nvnSyncWait");
p_TextureBuilderGetZCullStorageSize=(PFNNVNTEXTUREBUILDERGETZCULLSTORAGESIZEPROC)boot("nvnTextureBuilderGetZCullStorageSize");
p_TextureBuilderSetDefaults=(PFNNVNTEXTUREBUILDERSETDEFAULTSPROC)boot("nvnTextureBuilderSetDefaults");
p_TextureBuilderSetDepth=(PFNNVNTEXTUREBUILDERSETDEPTHPROC)boot("nvnTextureBuilderSetDepth");
p_TextureBuilderSetDevice=(PFNNVNTEXTUREBUILDERSETDEVICEPROC)boot("nvnTextureBuilderSetDevice");
p_TextureBuilderSetFormat=(PFNNVNTEXTUREBUILDERSETFORMATPROC)boot("nvnTextureBuilderSetFormat");
p_TextureBuilderSetHeight=(PFNNVNTEXTUREBUILDERSETHEIGHTPROC)boot("nvnTextureBuilderSetHeight");
p_TextureBuilderSetStorage=(PFNNVNTEXTUREBUILDERSETSTORAGEPROC)boot("nvnTextureBuilderSetStorage");
p_TextureBuilderSetTarget=(PFNNVNTEXTUREBUILDERSETTARGETPROC)boot("nvnTextureBuilderSetTarget");
p_TextureBuilderSetWidth=(PFNNVNTEXTUREBUILDERSETWIDTHPROC)boot("nvnTextureBuilderSetWidth");
p_TextureFinalize=(PFNNVNTEXTUREFINALIZEPROC)boot("nvnTextureFinalize");
p_TextureGetZCullStorageSize=(PFNNVNTEXTUREGETZCULLSTORAGESIZEPROC)boot("nvnTextureGetZCullStorageSize");
p_TextureGetStorageSize=(PFNNVNTEXTUREGETSTORAGESIZEPROC)boot("nvnTextureGetStorageSize");
p_TextureInitialize=(PFNNVNTEXTUREINITIALIZEPROC)boot("nvnTextureInitialize");
p_TextureReadTexels=(PFNNVNTEXTUREREADTEXELSPROC)boot("nvnTextureReadTexels");
p_TextureWriteTexelsStrided=(PFNNVNTEXTUREWRITETEXELSSTRIDEDPROC)boot("nvnTextureWriteTexelsStrided");
p_VertexAttribStateSetDefaults=(PFNNVNVERTEXATTRIBSTATESETDEFAULTSPROC)boot("nvnVertexAttribStateSetDefaults");
p_VertexAttribStateSetFormat=(PFNNVNVERTEXATTRIBSTATESETFORMATPROC)boot("nvnVertexAttribStateSetFormat");
p_VertexAttribStateSetStreamIndex=(PFNNVNVERTEXATTRIBSTATESETSTREAMINDEXPROC)boot("nvnVertexAttribStateSetStreamIndex");
p_VertexStreamStateSetDefaults=(PFNNVNVERTEXSTREAMSTATESETDEFAULTSPROC)boot("nvnVertexStreamStateSetDefaults");
p_VertexStreamStateSetStride=(PFNNVNVERTEXSTREAMSTATESETSTRIDEPROC)boot("nvnVertexStreamStateSetStride");
    if(!strcmp(argv[3],"unsupported")){p_CommandBufferDispatchComputeIndirect(NULL,0);return 9;}
    dm_vk_set_shader_root(argv[1]);
    typedef void (*SetAlloc)(void *(*)(unsigned long,unsigned long,void *),void (*)(void *,void *),void *(*)(void *,unsigned long,void *),void *);
    ((SetAlloc)forced("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_"))(allocate,release,resize,NULL);
    unsigned char *memory=allocate(262144,4096,NULL);if(!memory)return 3;memset(memory,0,262144);
    ((void (*)(void *,unsigned long))forced("_ZN2nv18InitializeGraphicsEPvm"))(memory,262144);
    NVNdeviceBuilder db={0};NVNdevice device={0};p_DeviceBuilderSetDefaults(&db);if(!p_DeviceInitialize(&device,&db))return 4;
    NVNmemoryPoolBuilder pb={0};NVNmemoryPool pool={0};p_MemoryPoolBuilderSetDefaults(&pb);p_MemoryPoolBuilderSetDevice(&pb,&device);
    p_MemoryPoolBuilderSetStorage(&pb,memory,262144);p_MemoryPoolBuilderSetFlags(&pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    if(!p_MemoryPoolInitialize(&pool,&pb))return 5;
    NVNbufferAddress base=p_MemoryPoolGetBufferAddress(&pool);
    /* Real mprotect write-fault recovery through root diagnostics, not a fake hook. */
    uint64_t faults0,preps0;nvn_dirty_stats(&faults0,&preps0);nvn_dirty_protect(memory+8192,4096);
    ((volatile unsigned *)(memory+8192))[0]=0xdeadbeef;((unsigned *)(memory+8192))[1]=0xcafebabe;
    uint64_t faults1,preps1;nvn_dirty_stats(&faults1,&preps1);
    int dirty_ok=faults1==faults0+1;
    NVNqueueBuilder qb={0};NVNqueue queue={0};p_QueueBuilderSetDefaults(&qb);p_QueueBuilderSetDevice(&qb,&device);
    if(!p_QueueInitialize(&queue,&qb))return 6;
    NVNcommandBuffer cb={0};if(!p_CommandBufferInitialize(&cb,&device))return 7;
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);
    NVNprogram compute={0};p_ProgramInitialize(&compute,&device);NVNshaderData cs={0,control(argv[1],"6ec4bfde22385501")};
    p_ProgramSetShaders(&compute,1,&cs);p_CommandBufferBeginRecording(&cb);
    p_CommandBufferBindProgram(&cb,&compute,NVN_SHADER_STAGE_COMPUTE_BIT);
    p_CommandBufferBindStorageBuffer(&cb,NVN_SHADER_STAGE_COMPUTE,3,base+8192,64);p_CommandBufferDispatchCompute(&cb,1,1,1);
    NVNcommandHandle handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);
    NVNsync sync={0};p_SyncInitialize(&sync,&device);p_QueueFenceSync(&queue,&sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    p_QueueFinish(&queue); /* Async queues need not have completed at FenceSync return. */
    int compute_ok=((unsigned *)(memory+8192))[0]==0 && ((unsigned *)(memory+8192))[1]==0xcafebabe && p_SyncWait(&sync,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    /* An unbound storage binding must not retain the previous real buffer.
     * Seed it on GPU, dispatch with the null binding, then copy its GPU bytes
     * back. The queued write prevents a CPU upload from hiding a bad store. */
    ((uint32_t *)(memory+12288))[0]=0x778899aa;
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
    p_CommandBufferCopyBufferToBuffer(&cb,base+12288,base+8192,4,0);
    p_CommandBufferBindStorageBuffer(&cb,NVN_SHADER_STAGE_COMPUTE,3,0,0);
    p_CommandBufferDispatchCompute(&cb,1,1,1);
    p_CommandBufferCopyBufferToBuffer(&cb,base+8192,base+16384,4,0);
    handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
    if(((uint32_t *)(memory+16384))[0]!=0x778899aa)return 60;
    float *matrix=(void *)(memory+4096);for(int i=0;i<4;i++)matrix[i*5]=1;
    const float vertices[24]={-.75f,-.75f,.5f,1,1,.25f,.125f,1,.75f,-.75f,.5f,1,1,.25f,.125f,1,0,.75f,.5f,1,1,.25f,.125f,1};
    memcpy(memory+131072,vertices,sizeof vertices);
    NVNtextureBuilder tb={0};NVNtexture target={0};p_TextureBuilderSetDefaults(&tb);p_TextureBuilderSetDevice(&tb,&device);
    p_TextureBuilderSetWidth(&tb,64);p_TextureBuilderSetHeight(&tb,64);p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);p_TextureBuilderSetStorage(&tb,&pool,16384);
    if(!p_TextureInitialize(&target,&tb))return 8;
    NVNprogram graphics={0};p_ProgramInitialize(&graphics,&device);
    NVNshaderData shaders[2]={{0,control(argv[1],"8ac93c54cfb3cbe3")},{0,control(argv[1],"0d721a0acc36c0bf")}};p_ProgramSetShaders(&graphics,2,shaders);
    NVNvertexAttribState attributes[2]={0};for(int i=0;i<2;i++){p_VertexAttribStateSetDefaults(&attributes[i]);p_VertexAttribStateSetFormat(&attributes[i],NVN_FORMAT_RGBA32F,i*16);p_VertexAttribStateSetStreamIndex(&attributes[i],0);}
    NVNvertexStreamState stream={0};p_VertexStreamStateSetDefaults(&stream);p_VertexStreamStateSetStride(&stream,32);
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
    const NVNtexture *targets[1]={&target};p_CommandBufferSetRenderTargets(&cb,1,targets,NULL,NULL,NULL);p_CommandBufferSetScissor(&cb,0,0,64,64);
    const float clear[4]={0,0,0,1},viewport[4]={0,0,64,64};p_CommandBufferClearColor(&cb,0,clear,15);
    p_CommandBufferSetDepthRange(&cb,0,1);p_CommandBufferSetViewports(&cb,0,1,viewport);
    p_CommandBufferBindProgram(&cb,&graphics,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
    p_CommandBufferBindUniformBuffer(&cb,NVN_SHADER_STAGE_VERTEX,0,base+4096,65536);
    p_CommandBufferBindVertexAttribState(&cb,2,attributes);p_CommandBufferBindVertexStreamState(&cb,1,&stream);
    const NVNbufferRange vertex_range={base+131072,sizeof vertices};p_CommandBufferBindVertexBuffers(&cb,0,1,&vertex_range);
    NVNseparateTextureHandle tex_handle={0};NVNseparateSamplerHandle sampler_handle={0};NVNimageHandle image_handle=0;
    p_CommandBufferBindSeparateTextures(&cb,NVN_SHADER_STAGE_FRAGMENT,0,1,&tex_handle);
    p_CommandBufferBindSeparateSamplers(&cb,NVN_SHADER_STAGE_FRAGMENT,0,1,&sampler_handle);
    p_CommandBufferBindImages(&cb,NVN_SHADER_STAGE_COMPUTE,0,1,&image_handle);
    p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);handle=p_CommandBufferEndRecording(&cb);
    /* Verify the actual recorder emitted compact array records, not maximum fixtures. */
    int compact=0;for(const unsigned char *r=(const void *)(uintptr_t)handle;;){op_hdr h;memcpy(&h,r,8);if(h.op==OP_BIND_VATTR && h.size==24)compact|=1;if(h.op==OP_BIND_VSTREAM && h.size==24)compact|=2;if(h.op==OP_BIND_VBUF && h.size==32)compact|=4;if(h.op==OP_BIND_TEX && h.size==32)compact|=8;if(h.op==OP_END)break;r+=h.size;}
    p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
    unsigned char pixels[64*64*4];NVNcopyRegion region={0,0,0,64,64,1};p_TextureReadTexels(&target,NULL,&region,pixels);
    unsigned colored=0,black=0;for(unsigned i=0;i<4096;i++){unsigned char *p=pixels+i*4;if(p[0]==255 && p[1]>=60 && p[1]<=68 && p[2]>=28 && p[2]<=36 && p[3]==255)colored++;else if(!p[0]&&!p[1]&&!p[2]&&p[3]==255)black++;}
    FILE *f=fopen(argv[2],"wb");if(!f || fwrite(pixels,1,sizeof pixels,f)!=sizeof pixels)return 10;fclose(f);
    /* Null uniform reads zero; disabled vertex attributes use (0,0,0,1).
     * Check the actual rendered pixels through the same original shaders. */
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
    p_CommandBufferClearColor(&cb,0,clear,15);p_CommandBufferBindUniformBuffer(&cb,NVN_SHADER_STAGE_VERTEX,0,0,0);
    p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
    unsigned char null_pixels[64*64*4];p_TextureReadTexels(&target,NULL,&region,null_pixels);
    for(unsigned i=0;i<4096;i++)if(null_pixels[i*4]||null_pixels[i*4+1]||null_pixels[i*4+2]||null_pixels[i*4+3]!=255)return 61;
    p_VertexAttribStateSetDefaults(&attributes[1]);const float blue[4]={0,0,1,1};
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
    p_CommandBufferClearColor(&cb,0,blue,15);p_CommandBufferBindUniformBuffer(&cb,NVN_SHADER_STAGE_VERTEX,0,base+4096,65536);
    p_CommandBufferBindVertexAttribState(&cb,2,attributes);p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    p_VertexAttribStateSetFormat(&attributes[1],NVN_FORMAT_RGBA32F,16);p_VertexAttribStateSetStreamIndex(&attributes[1],0);
    p_CommandBufferBindVertexAttribState(&cb,2,attributes);
    handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
    p_TextureReadTexels(&target,NULL,&region,null_pixels);unsigned defaults=0,background=0;
    for(unsigned i=0;i<4096;i++){const unsigned char *p=null_pixels+i*4;if(!p[0]&&!p[1]&&!p[2]&&p[3]==255)defaults++;else if(!p[0]&&!p[1]&&p[2]==255&&p[3]==255)background++;}
    if(defaults!=colored||background!=black)return 62;
    fprintf(stderr,"NULL_RESOURCE_DEVICE storage_store_discarded=1 uniform_load_zero=1 disabled_attribute_defaults=1 checked_pixels=8192 passed=1\n");
    uint64_t calls=0;unsigned called=0;for(unsigned i=0;i<NVN_NFUNC;i++){uint64_t n=atomic_load(&nvn_slot[i].count);calls+=n;if(n)called++;}

    NVNtexture za={0},zb={0};NVNtextureBuilder ztb={0};
    p_TextureBuilderSetDefaults(&ztb);p_TextureBuilderSetDevice(&ztb,&device);
    p_TextureBuilderSetWidth(&ztb,16);p_TextureBuilderSetHeight(&ztb,8);
    p_TextureBuilderSetFormat(&ztb,NVN_FORMAT_DEPTH24_STENCIL8);
    if(p_TextureBuilderGetZCullStorageSize(&ztb)!=64)return 40;
    /* Native D24S8 payload: low 24 depth bits, high 8 stencil bits. Check
     * both real GPU aspects before these textures become render targets. */
    for(unsigned i=163840;i<180224;i+=4){uint32_t packed=0x9a400000;memcpy(memory+i,&packed,4);}
    p_TextureBuilderSetStorage(&ztb,&pool,163840);if(!p_TextureInitialize(&za,&ztb))return 41;
    if(!dm_vk_copy_test_depth((const tex_t *)&za,.25f,154))return 59;
    p_TextureBuilderSetStorage(&ztb,&pool,180224);if(!p_TextureInitialize(&zb,&ztb))return 42;
    if(p_TextureGetZCullStorageSize(&za)!=64 || p_TextureGetZCullStorageSize(&target)!=0)return 43;
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
    p_CommandBufferSetRenderTargets(&cb,0,NULL,NULL,&za,NULL);p_CommandBufferSetScissor(&cb,0,0,16,8);
    p_CommandBufferClearDepthStencil(&cb,0.25f,1,17,255);
    p_CommandBufferSaveZCullData(&cb,base+200704,64);
    p_CommandBufferCopyBufferToBuffer(&cb,base+200704,base+204800,64,0);
    p_CommandBufferSetRenderTargets(&cb,0,NULL,NULL,&zb,NULL);p_CommandBufferClearDepthStencil(&cb,0.75f,1,201,255);
    p_CommandBufferSetRenderTargets(&cb,0,NULL,NULL,&za,NULL);
    p_CommandBufferRestoreZCullData(&cb,base+204800,64);
    handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);
    if(memcmp(memory+200704,memory+204800,64))return 44;
    NvnVkZCullRecord expected=nvn_vk_zcull_record(53,16,8,1,0,1);
    if(!nvn_vk_zcull_valid((const void *)(memory+204800),&expected))return 45;
    if(!dm_vk_copy_test_depth((const tex_t *)&za,.25f,17)||!dm_vk_copy_test_depth((const tex_t *)&zb,.75f,201))return 46;
    /* A record restores optimization state, never an old depth buffer. */
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
    p_CommandBufferSetRenderTargets(&cb,0,NULL,NULL,&za,NULL);p_CommandBufferClearDepthStencil(&cb,.5f,1,99,255);

    if(!strcmp(argv[3],"zcull-corrupt"))memory[200704]^=1;
    if(!strcmp(argv[3],"zcull-incompatible")){
        NvnVkZCullRecord wrong=nvn_vk_zcull_record(53,8,8,1,0,1);memcpy(memory+200704,&wrong,64);
    }
    if(!strcmp(argv[3],"zcull-bounds"))p_CommandBufferRestoreZCullData(&cb,base+262144,64);
    if(!strcmp(argv[3],"zcull-alignment"))p_CommandBufferRestoreZCullData(&cb,base+200705,64);
    if(!strcmp(argv[3],"zcull-size"))p_CommandBufferRestoreZCullData(&cb,base+200704,32);
    p_CommandBufferRestoreZCullData(&cb,base+200704,64);

    handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);
    if(!dm_vk_copy_test_depth((const tex_t *)&za,.5f,99))return 47;
    fprintf(stderr,"ZCULL_DEVICE save_copy_restore=1 cross_submission=1 depth_stencil_preserved=1 no_depth_rollback=1 checked_pixels=384 passed=1\n");

    NVNtexture zcolor={0};NVNtextureBuilder zcb={0};
    p_TextureBuilderSetDefaults(&zcb);p_TextureBuilderSetDevice(&zcb,&device);
    p_TextureBuilderSetWidth(&zcb,16);p_TextureBuilderSetHeight(&zcb,8);p_TextureBuilderSetFormat(&zcb,NVN_FORMAT_RGBA8);
    p_TextureBuilderSetStorage(&zcb,&pool,229376);if(!p_TextureInitialize(&zcolor,&zcb))return 48;
    NVNdepthStencilState test_depth={0};p_DepthStencilStateSetDefaults(&test_depth);
    p_DepthStencilStateSetDepthTestEnable(&test_depth,1);p_DepthStencilStateSetDepthWriteEnable(&test_depth,0);
    p_DepthStencilStateSetDepthFunc(&test_depth,NVN_DEPTH_FUNC_LESS);
    /* za currently contains .5 and the triangle depth is .5: LESS rejects.
     * zb contains .75: the same triangle passes. This checks real depth tests,
     * including driver HiZ, rather than only copying clear values back. */
    for(int k=0;k<2;k++){
        p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
        const NVNtexture *color[1]={&zcolor};p_CommandBufferSetRenderTargets(&cb,1,color,NULL,k?&zb:&za,NULL);
        const float view[4]={0,0,16,8};p_CommandBufferSetViewports(&cb,0,1,view);
        p_CommandBufferBindDepthStencilState(&cb,&test_depth);p_CommandBufferClearColor(&cb,0,clear,15);
        p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
        handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);
        uint8_t depth_color[16*8*4];NVNcopyRegion read={0,0,0,16,8,1};p_TextureReadTexels(&zcolor,NULL,&read,depth_color);
        unsigned lit=0;for(unsigned i=0;i<128;i++)if(depth_color[i*4])lit++;
        if((k==0&&lit)||(k==1&&!lit))return 49;
        fprintf(stderr,"ZCULL_DEPTH_TEST image=%d colored_pixels=%u passed=1\n",k,lit);
    }
    p_TextureFinalize(&zcolor);
    /* Real nearest scaled depth/stencil copy, with no additional shaders.
     * Last 16 KiB of the pool: volume starts at 212992 and zcolor at 229376.
     * A different destination clear ensures an omitted blit cannot pass. */
    NVNtexture zsmall={0};NVNtextureBuilder zsb={0};
    p_TextureBuilderSetDefaults(&zsb);p_TextureBuilderSetDevice(&zsb,&device);
    p_TextureBuilderSetWidth(&zsb,8);p_TextureBuilderSetHeight(&zsb,4);
    p_TextureBuilderSetFormat(&zsb,NVN_FORMAT_DEPTH24_STENCIL8);
    p_TextureBuilderSetStorage(&zsb,&pool,245760);if(!p_TextureInitialize(&zsmall,&zsb))return 63;
    int small_size=p_TextureGetStorageSize(&zsmall);
    if(small_size<=0||small_size>16384-64)return 64;
    memset(memory+245760-64,0x5a,64);memset(memory+245760+small_size,0xa5,64);
    struct {uint64_t before[2];NVNcopyRegion src,dst;uint64_t after[2];} copy_regions;
    memset(&copy_regions,0x96,sizeof(copy_regions));
    copy_regions.src=(NVNcopyRegion){0,0,0,16,8,1};copy_regions.dst=(NVNcopyRegion){0,0,0,8,4,1};
    unsigned char saved_regions[sizeof(copy_regions)];memcpy(saved_regions,&copy_regions,sizeof(copy_regions));
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
    p_CommandBufferSetRenderTargets(&cb,0,NULL,NULL,&za,NULL);p_CommandBufferSetScissor(&cb,0,0,16,8);
    p_CommandBufferClearDepthStencil(&cb,.25f,1,154,255);
    p_CommandBufferSetRenderTargets(&cb,0,NULL,NULL,&zsmall,NULL);p_CommandBufferSetScissor(&cb,0,0,8,4);
    p_CommandBufferClearDepthStencil(&cb,.875f,1,211,255);
    int copy_flags=!strcmp(argv[3],"depth-linear")?NVN_COPY_FLAGS_LINEAR_FILTER_BIT:NVN_COPY_FLAGS_NONE;
    p_CommandBufferCopyTextureToTexture(&cb,&za,NULL,&copy_regions.src,&zsmall,NULL,&copy_regions.dst,copy_flags);
    handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
    if(copy_flags){fprintf(stderr,"DEPTH_STENCIL_COPY_DEVICE linear_filter_unexpectedly_accepted=1\n");return 65;}
    if(!dm_vk_copy_test_depth((const tex_t *)&zsmall,.25f,154)||
       !dm_vk_copy_test_depth((const tex_t *)&za,.25f,154)||
       !dm_vk_copy_test_depth((const tex_t *)&zb,.75f,201))return 66;
    if(memcmp(saved_regions,&copy_regions,sizeof(copy_regions)))return 67;
    for(unsigned i=0;i<64;i++)if(memory[245760-64+i]!=0x5a||memory[245760+small_size+i]!=0xa5)return 68;
    fprintf(stderr,"DEPTH_STENCIL_COPY_DEVICE nearest_scaled=1 source_extent=16,8 destination_extent=8,4 depth_stencil_preserved=1 checked_pixels=288 guard_bytes=128 regions_unchanged=1 passed=1\n");
    p_TextureFinalize(&zsmall);
    NVNtexture volume={0};NVNtextureBuilder vb={0};
    p_TextureBuilderSetDefaults(&vb);p_TextureBuilderSetDevice(&vb,&device);
    p_TextureBuilderSetTarget(&vb,NVN_TEXTURE_TARGET_3D);p_TextureBuilderSetWidth(&vb,32);
    p_TextureBuilderSetHeight(&vb,16);p_TextureBuilderSetDepth(&vb,4);p_TextureBuilderSetFormat(&vb,NVN_FORMAT_RGBA8);
    p_TextureBuilderSetStorage(&vb,&pool,212992);if(!p_TextureInitialize(&volume,&vb))return 50;
    uint8_t upload[4*17*140];memset(upload,0xcc,sizeof(upload));
    for(unsigned z=0;z<4;z++)for(unsigned y=0;y<16;y++)for(unsigned x=0;x<32;x++){
        uint8_t *p=upload+z*17*140+y*140+x*4;p[0]=x;p[1]=y;p[2]=z;p[3]=255;
    }
    NVNcopyRegion whole={0,0,0,32,16,4};p_TextureWriteTexelsStrided(&volume,NULL,&whole,upload,140,17*140);
    uint8_t slice[32*16*4];unsigned write_checked=0;
    for(unsigned z=0;z<4;z++){
        NVNcopyRegion region={0,0,z,32,16,1};p_TextureReadTexels(&volume,NULL,&region,slice);
        for(unsigned y=0;y<16;y++)for(unsigned x=0;x<32;x++){
            const uint8_t *p=slice+(y*32+x)*4;
            if(p[0]!=x||p[1]!=y||p[2]!=z||p[3]!=255)return 51;write_checked++;
        }
    }
    /* GPU clear deliberately diverges from RAM. CPU partial writes must not
     * upload stale RAM over any GPU-owned pixel outside the updated box. */
    p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
    const float green[4]={0,1,0,1};p_CommandBufferClearTexture(&cb,&volume,NULL,&whole,green,15);
    handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);
    uint8_t patch[2*60];memset(patch,0xee,sizeof(patch));
    for(unsigned z=0;z<2;z++)for(unsigned y=0;y<2;y++)for(unsigned x=0;x<3;x++){
        uint8_t *p=patch+z*60+y*20+x*4;p[0]=200;p[1]=20;p[2]=10;p[3]=255;
    }
    NVNcopyRegion box={15,7,1,3,2,2};p_TextureWriteTexelsStrided(&volume,NULL,&box,patch,20,60);
    for(unsigned z=0;z<4;z++){
        NVNcopyRegion region={0,0,z,32,16,1};p_TextureReadTexels(&volume,NULL,&region,slice);
        for(unsigned y=0;y<16;y++)for(unsigned x=0;x<32;x++){
            const uint8_t *p=slice+(y*32+x)*4;int changed=z>=1&&z<=2&&x>=15&&x<18&&y>=7&&y<9;
            if(p[0]!=(changed?200:0)||p[1]!=(changed?20:255)||p[2]!=(changed?10:0)||p[3]!=255)return 52;write_checked++;
        }
    }
    fprintf(stderr,"CPU_TEXTURE_WRITE_DEVICE volume_strided=1 partial_box=1 gpu_owned_pixels_preserved=1 checked_pixels=%u passed=1\n",write_checked);
    p_TextureFinalize(&volume);
    p_TextureFinalize(&za);
p_TextureFinalize(&zb);
    p_TextureFinalize(&target);
p_ProgramFinalize(&graphics);p_ProgramFinalize(&compute);p_SyncFinalize(&sync);p_CommandBufferFinalize(&cb);p_QueueFinalize(&queue);p_MemoryPoolFinalize(&pool);p_DeviceFinalize(&device);
    ((void (*)(void))forced("_ZN2nv16FinalizeGraphicsEv"))();free((void *)cs.control);free((void *)shaders[0].control);free((void *)shaders[1].control);free(memory);
    int draw_ok=colored>0 && black>0 && colored+black==4096;
    printf("{\"compute_passed\":%s,\"draw_passed\":%s,\"real_write_fault_hook_passed\":%s,\"compact_records_passed\":%s,\"static_thunk_calls\":%llu,\"static_thunk_functions\":%u,\"colored_pixels\":%u,\"black_pixels\":%u,\"fixture_context\":true,\"original_game_frame\":false}\n",compute_ok?"true":"false",draw_ok?"true":"false",dirty_ok?"true":"false",compact==15?"true":"false",(unsigned long long)calls,called,colored,black);
    return compute_ok&&draw_ok&&dirty_ok&&compact==15?0:11;
}
