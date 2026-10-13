/* First Android Vulkan replay implementation for the original NVN recorder.
 * This file preserves the recorder's mtl_* C interface to avoid guest changes.
 * Unsupported semantics abort with a named boundary; no draw/dispatch is dropped.
 * The initial queue is deliberately synchronous. Guest-visible writes/events
 * are published only after a real fence has completed.
 */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include "nvn_obj.h"
#include "../../android/nvn_platform_clock.h"
#include "nvn_mtl.h"
#include "nvn_dirty.h"
#include "../tex/nvn_deswizzle.h"
#include "../../android/nvn_platform_clock.h"
#include "../tex/nvn_bcdec.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include <math.h>
#include <dlfcn.h>
#include <unistd.h>
#include "../../android/nvn_platform_backend.h"

#define LIMIT_POOLS 512
#define LIMIT_OBJECTS 4096
#define LIMIT_BINDINGS 64
#define LIMIT_WRITES 4096
#define CHECK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS) die(#x,(long long)r_); } while(0)
static _Noreturn void die(const char *where,long long value) {
    fprintf(stderr,"[nvn-vulkan] STOP %s (%lld)\n",where,value); fflush(stderr); abort();
}
#include "nvn_vk_dispatch.h"
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *map; uint64_t size; } VBuffer;
typedef struct { const uint8_t *guest; uint64_t size; VBuffer v; } VPool;
typedef struct { uint32_t set,binding,type,kind; int32_t slot,sampler; uint32_t count,flags; } RBinding;
typedef struct { uint32_t magic,version,stage,nb,na,c1n,ncap,local[3],fout,reserved; } RHeader;
typedef struct {
    char key[17]; RHeader h; RBinding b[LIMIT_BINDINGS]; uint32_t attr[32][2],caps[32];
    VBuffer c1; VkShaderModule module; VkDescriptorSetLayout layout; uint64_t fetch_bindings,dref_bindings;
} VShader;
typedef struct { uint64_t serial; tex_t source; VkImage image; VkDeviceMemory memory;
    VkImageView view; VkFormat format; VkImageLayout layout; uint32_t width,height,depth,bpp; int cpu_owned; uint64_t upload_stamp,version;
    uint8_t *cpu_snapshot;
    VkImage shadow;VkDeviceMemory shadow_memory;VkImageView shadow_view;VkFormat shadow_format;VkImageLayout shadow_layout;uint64_t shadow_version; } VTexture;
typedef struct { int live; tex_t texture; view_t view; uint8_t has_view; } TSlot;
typedef struct { int live; samp_par source; VkSampler sampler; } SSlot;
typedef struct { uint64_t addr,size; } Range;
typedef struct { volatile uint32_t *addr; uint32_t mode,value; } Event;
typedef struct { uint32_t kind,query; uint64_t address; } CounterOp;
typedef struct { Range range; VBuffer *buffer; int written; } AlignedBinding;
typedef struct { uint64_t serial; unsigned roles; } ImageUse;
typedef struct { const uint8_t *entry;uint64_t hash; } ShaderEntry;
typedef struct { VBuffer *buffer;uint64_t bytes,offset;char path[1400]; } TraceCapture;
typedef struct { VkPipeline pipeline; VkPipelineLayout layout; uint32_t shader[2]; } Pso;
typedef struct {
    const prog_t *program; p_set_rt rt; va_st attributes[16]; vs_st streams[16]; NVNbufferRange vbuf[16];
    Range uniform[6][16],storage[6][16]; uint64_t tex[6][128],samp[6][128],img[6][32];
    color_st color; chmask_st mask; blend_st blend[8]; poly_st polygon; ds_st ds; ms_st ms;
    VkViewport viewport; VkRect2D scissor; float blend_color[4]; uint64_t copy_stride;
    const dpool_t *texture_pool,*sampler_pool; Range scratch; uint32_t stencil_compare[2],stencil_write[2],stencil_ref[2];
} ReplayState;
static struct {
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family; VkSampler fetch_sampler;
    VkPhysicalDeviceMemoryProperties memory; VkPhysicalDeviceProperties properties;
    VkCommandPool command_pool; VkCommandBuffer command; VkDescriptorPool descriptors;VkPipelineCache pipeline_cache;
    VBuffer support,zero; VPool pools[LIMIT_POOLS]; int npool;
    VShader shaders[LIMIT_OBJECTS]; int nshader; VTexture textures[LIMIT_OBJECTS]; int ntexture;
    TSlot *tslot; size_t texture_slots; SSlot sslot[LIMIT_OBJECTS]; int texture_pool_n;
    Range writes[LIMIT_WRITES]; int nwrites; Event events[LIMIT_WRITES]; int nevents;
    VBuffer **temporaries; size_t temporary_capacity; int ntemporary;
    AlignedBinding aligned[2*LIMIT_BINDINGS]; int naligned;
    ImageUse image_uses[2*LIMIT_BINDINGS]; unsigned nimage_uses;
    VkImageView temporary_views[LIMIT_WRITES]; int nview;
    VkBufferView buffer_views[LIMIT_WRITES]; int nbuffer_view;
    VkQueryPool occlusion,timestamps; uint32_t nocc,ntime,timestamp_bits; int precise_occlusion;
    CounterOp counter_ops[LIMIT_WRITES]; int ncounter; uint64_t samples_passed;
    VkPipeline pipelines[LIMIT_WRITES]; VkPipelineLayout layouts[LIMIT_WRITES]; int npipeline;
    VkRenderPass passes[LIMIT_WRITES]; VkFramebuffer framebuffers[LIMIT_WRITES]; int npass;
    int in_pass,active,feedback_layout,depth_sampled; VkPipelineCreateFlags feedback_flags; ReplayState state; VkPhysicalDeviceSubgroupProperties subgroup;
    uint8_t *shader_index; size_t index_bytes; char shader_root[1024]; ANativeWindow *window;
    ShaderEntry *shader_entries;size_t shader_entry_capacity;
    VkSurfaceKHR surface; VkSwapchainKHR swapchain; VkImage swap_images[8]; VkImageLayout swap_layouts[8];
    uint32_t nswap; VkFormat swap_format; VkExtent2D swap_extent; int display_hz;
    _Atomic uint64_t fence,completed,present_count; uint64_t draws,pass_count,uploads,pool_adds,frame;
    int trace_frame,trace_ui;_Atomic int window_origin;
    uint64_t profile_pipeline_ns,profile_wait_ns,profile_decode_ns,profile_submits,profile_start_ns,profile_draws,profile_frames;
    uint64_t profile_lookup_ns,profile_pool_ns,profile_bind_ns,profile_render_ns,profile_present_ns;unsigned trace_meshes;
    TraceCapture trace_captures[512];unsigned ntrace_captures,ntrace_textures;uint64_t trace_texture_serials[24];
    pthread_mutex_t lock;
    void *transfer_memory; size_t transfer_size; int graphics_live;
} G={.lock=PTHREAD_MUTEX_INITIALIZER};
static pthread_once_t init_once=PTHREAD_ONCE_INIT;
static VkDebugUtilsMessengerEXT validation_messenger;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;
    static struct {int32_t id;unsigned count;} seen[128];static unsigned count;
    unsigned i=0;while(i<count && seen[i].id!=data->messageIdNumber)i++;
    if(i==count){if(count==128)return VK_FALSE;seen[count++].id=data->messageIdNumber;}
    if(seen[i].count++<10)fprintf(stderr,"[nvn-vulkan] VALIDATION severity=%#x draw=%llu id=%s message=%s\n",severity,(unsigned long long)G.draws,data->pMessageIdName?data->pMessageIdName:"unknown",data->pMessage);
    return VK_FALSE;
}

static uint32_t memory_type(uint32_t mask,VkMemoryPropertyFlags properties) {
    for(uint32_t i=0;i<G.memory.memoryTypeCount;i++) if((mask&(1u<<i)) &&
        (G.memory.memoryTypes[i].propertyFlags&properties)==properties) return i;
    die("memory type",properties);
}
static VBuffer buffer_create(uint64_t size,VkBufferUsageFlags usage,int zero) {
    VBuffer b={.size=size}; if(!size || size>SIZE_MAX) die("buffer size",size);
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE}; CHECK(vkCreateBuffer(G.device,&info,NULL,&b.b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(G.device,b.b,&r);
    VkMemoryAllocateInfo a={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,
        .memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(G.device,&a,NULL,&b.m)); CHECK(vkBindBufferMemory(G.device,b.b,b.m,0));
    void *mapped=NULL; CHECK(vkMapMemory(G.device,b.m,0,size,0,&mapped)); b.map=mapped;if(zero)memset(b.map,0,size);return b;
}
static VBuffer buffer_make(uint64_t size,VkBufferUsageFlags usage) { return buffer_create(size,usage,1); }
static void buffer_free(VBuffer *b) {
    if(b->map) vkUnmapMemory(G.device,b->m); if(b->b) vkDestroyBuffer(G.device,b->b,NULL);
    if(b->m) vkFreeMemory(G.device,b->m,NULL); memset(b,0,sizeof(*b));
}
static VBuffer *temporary(uint64_t size,VkBufferUsageFlags usage) {
    if((size_t)G.ntemporary==G.temporary_capacity){size_t count=G.temporary_capacity?G.temporary_capacity*2:256;
        VBuffer **entries=realloc(G.temporaries,count*sizeof(*entries));if(!entries)die("temporary buffer registry allocation",count);G.temporaries=entries;G.temporary_capacity=count;}
    VBuffer *b=malloc(sizeof(*b));if(!b)die("temporary buffer allocation",size);
    *b=buffer_make(size,usage);G.temporaries[G.ntemporary++]=b;return b;
}
static void trace_capture_queue(VBuffer *buffer,uint64_t bytes,const char *name) {
    const char *root=getenv("DM_FRAME_DIR");if(!root)return;
    if(G.ntrace_captures==512)die("GPU trace capture capacity",512);
    TraceCapture *c=&G.trace_captures[G.ntrace_captures++];c->buffer=buffer;c->bytes=bytes;c->offset=0;
    snprintf(c->path,sizeof(c->path),"%s/%s.bin",root,name);
}
static void trace_capture_complete(void) {
    for(unsigned i=0;i<G.ntrace_captures;i++){TraceCapture *c=&G.trace_captures[i];FILE *out=fopen(c->path,"wb");if(!out)die("GPU trace output",0);
        if(fwrite(c->buffer->map+c->offset,1,c->bytes,out)!=c->bytes)die("GPU trace write",c->bytes);fclose(out);
        fprintf(stderr,"[nvn-vulkan] GPU_TRACE_CAPTURE bytes=%llu path=%s\n",(unsigned long long)c->bytes,c->path);
    }
    G.ntrace_captures=0;
}
void dm_vk_set_shader_root(const char *path) {
    if(!path || strlen(path)>=sizeof(G.shader_root) || G.nshader) die("shader root change",0);
    strcpy(G.shader_root,path);
}
void mtl_set_window_origin_mode(int mode) {
    if(mode!=NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT&&mode!=NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT)die("NVN window origin mode",mode);
    atomic_store(&G.window_origin,mode);
    fprintf(stderr,"[nvn-vulkan] WINDOW_ORIGIN mode=%d negative_viewport=%d\n",mode,mode==NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
}
static void backend_init(void) {
    void *loader=NULL;
    const char *private_driver=getenv("DM_PRIVATE_VULKAN");
    if(private_driver && !strcmp(private_driver,"turnip")) {
        Dl_info info;char directory[1024];
        if(!dladdr((void *)backend_init,&info) || !info.dli_fname || strlen(info.dli_fname)>=sizeof(directory))die("private Vulkan native library directory",0);
        strcpy(directory,info.dli_fname);char *slash=strrchr(directory,'/');if(!slash)die("private Vulkan native path",0);slash[1]=0;
        char tool_path[1100];snprintf(tool_path,sizeof tool_path,"%slibadrenotools.so",directory);
        void *tool=dlopen(tool_path,RTLD_NOW|RTLD_LOCAL);if(!tool)die("private Vulkan adrenotools load",0);
        void *(*open_driver)(int,int,const char *,const char *,const char *,const char *,const char *,void **)=dlsym(tool,"adrenotools_open_libvulkan");
        if(!open_driver)die("private Vulkan driver entry",0);
        loader=open_driver(RTLD_NOW|RTLD_LOCAL,1,NULL,directory,directory,"libd2r_turnip.so",NULL,NULL);
        if(!loader)die("private Vulkan loader failed",0);
    }else loader=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if(!loader)die("Vulkan loader unavailable",0);
    dm_vk_get=(PFN_vkGetInstanceProcAddr)dlsym(loader,"vkGetInstanceProcAddr");if(!dm_vk_get)die("Vulkan proc address unavailable",0);
    dm_vkCreateInstance=(PFN_vkCreateInstance)dm_vk_get(NULL,"vkCreateInstance");if(!dm_vkCreateInstance)die("Vulkan create instance unavailable",0);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="D2R Android",.apiVersion=VK_API_VERSION_1_1};
    int validation=getenv("DM_VULKAN_VALIDATION")!=NULL;
    const char *ie[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char *layers[]={"VK_LAYER_KHRONOS_validation"};
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,.pfnUserCallback=validation_message};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,
        .enabledExtensionCount=validation?3u:2u,.ppEnabledExtensionNames=ie,.enabledLayerCount=validation?1u:0u,.ppEnabledLayerNames=validation?layers:NULL,.pNext=validation?&debug:NULL}; CHECK(vkCreateInstance(&ii,NULL,&G.instance));
    if(validation){PFN_vkCreateDebugUtilsMessengerEXT create=(PFN_vkCreateDebugUtilsMessengerEXT)dm_vk_get(G.instance,"vkCreateDebugUtilsMessengerEXT");if(!create)die("validation debug messenger unavailable",0);CHECK(create(G.instance,&debug,NULL,&validation_messenger));fprintf(stderr,"[nvn-vulkan] VALIDATION_ENABLED\n");}
    dm_vk_resolve(G.instance);
    uint32_t n=0; CHECK(vkEnumeratePhysicalDevices(G.instance,&n,NULL)); if(!n) die("Vulkan physical device",0);
    VkPhysicalDevice *physical=calloc(n,sizeof(*physical)); if(!physical) die("allocate physical list",n);
    CHECK(vkEnumeratePhysicalDevices(G.instance,&n,physical)); G.physical=physical[0]; free(physical);
    vkGetPhysicalDeviceProperties(G.physical,&G.properties); vkGetPhysicalDeviceMemoryProperties(G.physical,&G.memory);
    G.subgroup=(VkPhysicalDeviceSubgroupProperties){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver_properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,.pNext=&G.subgroup};
    VkPhysicalDeviceProperties2 properties2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver_properties};
    vkGetPhysicalDeviceProperties2(G.physical,&properties2);
    if(private_driver && !strcmp(private_driver,"turnip") && driver_properties.driverID!=VK_DRIVER_ID_MESA_TURNIP)die("requested private driver was not loaded",driver_properties.driverID);
    fprintf(stderr,"[nvn-vulkan] driver id=%u name=%s info=%s\n",driver_properties.driverID,driver_properties.driverName,driver_properties.driverInfo);
    fprintf(stderr,"[nvn-vulkan] subgroup size=%u stages=%#x operations=%#x\n",G.subgroup.subgroupSize,G.subgroup.supportedStages,G.subgroup.supportedOperations);
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,NULL);
    VkQueueFamilyProperties *families=calloc(n,sizeof(*families)); if(!families) die("queue list",n);
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,families); G.family=UINT32_MAX;
    for(uint32_t i=0;i<n;i++) {
        if((families[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==
           (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) { G.family=i; break; }
    }
    free(families);
    if(G.family==UINT32_MAX) die("graphics/compute queue",0);
    float priority=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
       .queueFamilyIndex=G.family,.queueCount=1,.pQueuePriorities=&priority};
    const char *de[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME};
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions=(PFN_vkEnumerateDeviceExtensionProperties)dm_vk_get(G.instance,"vkEnumerateDeviceExtensionProperties");
    uint32_t extension_count=0;CHECK(enumerate_extensions(G.physical,NULL,&extension_count,NULL));
    VkExtensionProperties *extension_properties=calloc(extension_count,sizeof(*extension_properties));if(!extension_properties)die("device extension allocation",extension_count);
    CHECK(enumerate_extensions(G.physical,NULL,&extension_count,extension_properties));int has_feedback=0;
    for(uint32_t i=0;i<extension_count;i++)has_feedback|=!strcmp(extension_properties[i].extensionName,de[1]);free(extension_properties);
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    if(has_feedback){VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&feedback};
        PFN_vkGetPhysicalDeviceFeatures2 get_features=(PFN_vkGetPhysicalDeviceFeatures2)dm_vk_get(G.instance,"vkGetPhysicalDeviceFeatures2");get_features(G.physical,&features);}
    G.feedback_layout=feedback.attachmentFeedbackLoopLayout;
    VkPhysicalDeviceFeatures available,enabled={0}; vkGetPhysicalDeviceFeatures(G.physical,&available);
    enabled.shaderStorageImageReadWithoutFormat=available.shaderStorageImageReadWithoutFormat;
    enabled.shaderStorageImageWriteWithoutFormat=available.shaderStorageImageWriteWithoutFormat;
    enabled.shaderImageGatherExtended=available.shaderImageGatherExtended;
    enabled.samplerAnisotropy=available.samplerAnisotropy;
    enabled.imageCubeArray=available.imageCubeArray;
    enabled.occlusionQueryPrecise=available.occlusionQueryPrecise;
    if(!available.independentBlend)die("NVN independent attachment blending unavailable",0);
    enabled.independentBlend=VK_TRUE;
    enabled.fragmentStoresAndAtomics=available.fragmentStoresAndAtomics;
    enabled.vertexPipelineStoresAndAtomics=available.vertexPipelineStoresAndAtomics;
    G.precise_occlusion=enabled.occlusionQueryPrecise;
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,NULL);
    families=calloc(n,sizeof(*families));if(!families)die("timestamp queue properties",n);
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,families);G.timestamp_bits=families[G.family].timestampValidBits;free(families);
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
       .enabledExtensionCount=G.feedback_layout?2u:1u,.ppEnabledExtensionNames=de,.pEnabledFeatures=&enabled,.pNext=G.feedback_layout?&feedback:NULL}; CHECK(vkCreateDevice(G.physical,&di,NULL,&G.device));
    fprintf(stderr,"[nvn-vulkan] attachment_feedback_layout=%d independent_blend=%u fragment_stores=%u\n",G.feedback_layout,enabled.independentBlend,enabled.fragmentStoresAndAtomics);
    vkGetDeviceQueue(G.device,G.family,0,&G.queue);
    /* The backend/device live for the process lifetime. Keep driver compilation
     * results across submissions while preserving every pipeline's full state.
     * An empty cache belongs only to this device; no cross-device data is loaded. */
    if(!getenv("DM_DISABLE_PIPELINE_CACHE")){
        VkPipelineCacheCreateInfo cache={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        CHECK(vkCreatePipelineCache(G.device,&cache,NULL,&G.pipeline_cache));
    }
    fprintf(stderr,"[nvn-vulkan] driver_pipeline_cache=%d\n",G.pipeline_cache!=VK_NULL_HANDLE);
    VkCommandPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=G.family,
       .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT}; CHECK(vkCreateCommandPool(G.device,&ci,NULL,&G.command_pool));
    G.support=buffer_make(1392,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); G.zero=buffer_make(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    for(int i=0;i<73;i++) { float one=1; memcpy(G.support.map+192+16*i,&one,4); }
    G.state.mask.mask=UINT32_MAX; G.state.polygon.mode=NVN_POLYGON_MODE_FILL;
    G.state.color.logic_op=NVN_LOGIC_OP_COPY;G.state.color.alpha_func=NVN_ALPHA_FUNC_ALWAYS;
    G.state.ds.df=NVN_DEPTH_FUNC_LESS; G.state.viewport=(VkViewport){0,0,1,1,0,1};
    G.state.ds.sf_f=G.state.ds.sf_b=NVN_STENCIL_FUNC_ALWAYS;
    G.state.ds.fail_f=G.state.ds.fail_b=G.state.ds.zfail_f=G.state.ds.zfail_b=G.state.ds.zpass_f=G.state.ds.zpass_b=NVN_STENCIL_OP_KEEP;
    G.state.stencil_compare[0]=G.state.stencil_compare[1]=G.state.stencil_write[0]=G.state.stencil_write[1]=UINT32_MAX;
    fprintf(stderr,"[nvn-vulkan] initialized %s\n",G.properties.deviceName);
    G.profile_start_ns=dm_android_nvn_clock_ns();
}
void mtl_init(void) { if(pthread_once(&init_once,backend_init))die("Vulkan initialize once",0); }
void *dm_vk_native_window(void) {pthread_mutex_lock(&G.lock);void *w=G.window;pthread_mutex_unlock(&G.lock);return w;}
int dm_vk_graphics_live(void) {pthread_mutex_lock(&G.lock);int live=G.graphics_live;pthread_mutex_unlock(&G.lock);return live;}
void dm_vk_graphics_begin(void *memory,size_t size) {
    if(!memory || !size || size>UINTPTR_MAX-(uintptr_t)memory)die("nv::InitializeGraphics transfer-memory bounds",size);
    mtl_init();pthread_mutex_lock(&G.lock);
    if(G.graphics_live)die("nv::InitializeGraphics duplicate session",size);
    G.transfer_memory=memory;G.transfer_size=size;G.graphics_live=1;
    fprintf(stderr,"[nvn-vulkan] graphics session registered transfer-memory=%p bytes=%zu; Vulkan device initialized\n",memory,size);
    pthread_mutex_unlock(&G.lock);
}
void dm_vk_graphics_end(void) {
    mtl_init();pthread_mutex_lock(&G.lock);
    if(!G.graphics_live || G.active || G.npool)die("nv::FinalizeGraphics live session/pools/submission",G.npool);
    CHECK(vkDeviceWaitIdle(G.device));G.graphics_live=0;G.transfer_memory=NULL;G.transfer_size=0;
    fprintf(stderr,"[nvn-vulkan] graphics session ended after Vulkan device idle; host device retained\n");pthread_mutex_unlock(&G.lock);
}
void mtl_pool_add(const void *mem,uint64_t size) {
    mtl_init(); pthread_mutex_lock(&G.lock);
    if(!mem || G.npool==LIMIT_POOLS || size>UINTPTR_MAX-(uintptr_t)mem) die("memory pool bounds",size);
    for(int i=0;i<G.npool;i++) if(G.pools[i].guest==mem) die("duplicate memory pool",(long long)(uintptr_t)mem);
    VPool *pool=&G.pools[G.npool++]; pool->guest=mem;
    *pool=(VPool){.guest=mem,.size=size};G.pool_adds++; pthread_mutex_unlock(&G.lock);
}
static void pool_materialize(VPool *pool) {
    if(pool->v.b)return;
    /* pool_read supplies every CPU-owned range before a GPU read, and queued
     * writes supply their own bytes. Touching the entire backing with memset
     * would commit hundreds of MiB that the game has not used. */
    pool->v=buffer_create(pool->size,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,0);
    uint64_t logical=0,resident=0;for(int i=0;i<G.npool;i++){logical+=G.pools[i].size;resident+=G.pools[i].v.size;}
    fprintf(stderr,"[nvn-vulkan] POOL_MATERIALIZE bytes=%llu resident=%llu logical=%llu pools=%d\n",(unsigned long long)pool->size,(unsigned long long)resident,(unsigned long long)logical,G.npool);
}
static VPool *pool_find(uint64_t addr,uint64_t size,uint64_t *offset) {
    if(size>UINT64_MAX-addr) return NULL;
    for(int i=0;i<G.npool;i++) { uint64_t base=(uint64_t)(uintptr_t)G.pools[i].guest;
        if(addr>=base && addr-base<=G.pools[i].size && size<=G.pools[i].size-(addr-base)) {
            if(offset) *offset=addr-base; return &G.pools[i]; } }
    return NULL;
}
int mtl_addr(uint64_t addr,uint64_t len,void **buf,uint64_t *off) {
    VPool *p=pool_find(addr,len,off); if(!p) return 0; *buf=p; return 1;
}
int mtl_gpu_addr(uint64_t addr,uint64_t len,void **buf,uint64_t *off,uint64_t *gpu) {
    if(!mtl_addr(addr,len,buf,off)) return 0; *gpu=addr; return 1; /* NVN virtual address, resolved through descriptors. */
}
void mtl_pool_del(const void *mem) {
    if(G.active) die("pool deleted during submission",(long long)(uintptr_t)mem);
    pthread_mutex_lock(&G.lock); for(int i=0;i<G.npool;i++) if(G.pools[i].guest==mem) {
        buffer_free(&G.pools[i].v); G.pools[i]=G.pools[--G.npool]; pthread_mutex_unlock(&G.lock); return; }
    pthread_mutex_unlock(&G.lock); die("unknown memory pool delete",(long long)(uintptr_t)mem);
}
static int overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn) { return a<b+bn && b<a+an; }
static VPool *pool_read(uint64_t addr,uint64_t len,uint64_t *off) {
    uint64_t started=dm_android_nvn_clock_ns();
    VPool *p=pool_find(addr,len,off); if(!p) die("unmapped NVN buffer range",addr);
    pool_materialize(p);
    /* A GPU-written range in this command stream must retain its queued value. */
    uint64_t cursor=addr,end=addr+len;
    while(cursor<end) {
        uint64_t first=end,skip=cursor;
        for(int i=0;i<G.nwrites;i++) { Range r=G.writes[i];
            if(cursor>=r.addr && cursor<r.addr+r.size && r.addr+r.size>skip)skip=r.addr+r.size;
            else if(r.addr>cursor && r.addr<first)first=r.addr;
        }
        if(skip>cursor){cursor=skip<end?skip:end;continue;}
        memcpy(p->v.map+*off+(cursor-addr),(const void *)(uintptr_t)cursor,first-cursor);cursor=first;
    }
    G.profile_pool_ns+=dm_android_nvn_clock_ns()-started;return p;
}
static void mark_write(uint64_t addr,uint64_t len) {
    if(!pool_find(addr,len,NULL)) die("unmapped write range",addr);
    if(G.nwrites==LIMIT_WRITES) die("write range limit",G.nwrites); G.writes[G.nwrites++]=(Range){addr,len};
}
static void pass_end(void) { if(G.in_pass) { vkCmdEndRenderPass(G.command); G.in_pass=0; } }
static void all_barrier(void) {
    pass_end(); VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
       .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(G.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,NULL,0,NULL);
}
void mtl_sub_begin(void) {
    mtl_init(); pthread_mutex_lock(&G.lock); if(G.active) die("nested submission",0); G.active=1;
    G.nwrites=G.nevents=G.ntemporary=G.npipeline=G.npass=0; G.in_pass=0;
    G.nocc=G.ntime=0;G.ncounter=0;G.nview=0;G.nbuffer_view=0;
    VkCommandBufferAllocateInfo a={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=G.command_pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1}; CHECK(vkAllocateCommandBuffers(G.device,&a,&G.command));
    VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(G.command,&b));
    VkQueryPoolCreateInfo query={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=LIMIT_WRITES};
    CHECK(vkCreateQueryPool(G.device,&query,NULL,&G.occlusion));
    query.queryType=VK_QUERY_TYPE_TIMESTAMP;CHECK(vkCreateQueryPool(G.device,&query,NULL,&G.timestamps));
    vkCmdResetQueryPool(G.command,G.occlusion,0,LIMIT_WRITES);vkCmdResetQueryPool(G.command,G.timestamps,0,LIMIT_WRITES);
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16384},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8192},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,16384},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4096},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,16384},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,4096},{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,4096}};
    VkDescriptorPoolCreateInfo d={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=8192,
       .poolSizeCount=7,.pPoolSizes=sizes}; CHECK(vkCreateDescriptorPool(G.device,&d,NULL,&G.descriptors));
}
void mtl_sub_copy(uint64_t src,uint64_t dst,uint64_t len) {
    all_barrier(); uint64_t so=0,doff=0; VPool *s=pool_read(src,len,&so),*d=pool_find(dst,len,&doff);
    if(!d || overlap(src,len,dst,len)) die("buffer copy bounds/overlap",dst);pool_materialize(d);
    VkBufferCopy copy={so,doff,len}; vkCmdCopyBuffer(G.command,s->v.b,d->v.b,1,&copy); mark_write(dst,len);
}
void mtl_sub_event(volatile uint32_t *addr,uint32_t mode,uint32_t value) {
    if(mode>1 || G.nevents==LIMIT_WRITES) die("event mode/count",mode);
    G.events[G.nevents++]=(Event){addr,mode,value};
}
static void counter_op(uint32_t kind,uint32_t query,uint64_t address) {
    if(G.ncounter==LIMIT_WRITES)die("counter operation limit",G.ncounter);
    G.counter_ops[G.ncounter++]=(CounterOp){kind,query,address};
}
void mtl_sub_counter(uint64_t addr,uint32_t type) {
    if(type!=NVN_COUNTER_TYPE_SAMPLES_PASSED && type!=NVN_COUNTER_TYPE_TIMESTAMP && type!=NVN_COUNTER_TYPE_TIMESTAMP_TOP)
        die("GPU counter type implementation pending",type);
    if(!pool_find(addr,16,NULL) || addr%8)die("GPU counter report range/alignment",addr);
    if(!G.timestamp_bits || G.ntime==LIMIT_WRITES)die("GPU timestamp support/count",G.ntime);
    pass_end();uint32_t q=G.ntime++;
    vkCmdWriteTimestamp(G.command,type==NVN_COUNTER_TYPE_TIMESTAMP_TOP?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,G.timestamps,q);
    counter_op(type==NVN_COUNTER_TYPE_SAMPLES_PASSED?2:3,q,addr);
}
uint64_t mtl_sub_commit(void) {
    if(!G.active) die("commit without begin",0); pass_end();
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(G.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(G.command)); VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence;
    CHECK(vkCreateFence(G.device,&fi,NULL,&fence)); VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&G.command};
    CHECK(vkQueueSubmit(G.queue,1,&si,fence)); ++G.fence;
    uint64_t wait_start=dm_android_nvn_clock_ns();CHECK(vkWaitForFences(G.device,1,&fence,VK_TRUE,UINT64_MAX));G.profile_wait_ns+=dm_android_nvn_clock_ns()-wait_start;G.profile_submits++;
    trace_capture_complete();
    for(int i=0;i<G.nwrites;i++) { uint64_t off=0; VPool *p=pool_find(G.writes[i].addr,G.writes[i].size,&off);
       memcpy((void *)(uintptr_t)G.writes[i].addr,p->v.map+off,G.writes[i].size); }
    uint64_t occ[LIMIT_WRITES],times[LIMIT_WRITES];
    if(G.nocc)CHECK(vkGetQueryPoolResults(G.device,G.occlusion,0,G.nocc,G.nocc*8,occ,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    if(G.present_count<2 || G.trace_frame)for(uint32_t i=0;i<G.nocc;++i)fprintf(stderr,"[nvn-vulkan] DRAW_SAMPLES query=%u samples=%llu\n",i,(unsigned long long)occ[i]);
    if(G.ntime)CHECK(vkGetQueryPoolResults(G.device,G.timestamps,0,G.ntime,G.ntime*8,times,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    for(int i=0;i<G.ncounter;++i) {CounterOp *op=&G.counter_ops[i];
        if(op->kind==0)G.samples_passed=0;
        else if(op->kind==1)G.samples_passed+=occ[op->query];
        else {uint64_t ticks=times[op->query];if(G.timestamp_bits<64)ticks&=(1ULL<<G.timestamp_bits)-1;
            uint64_t stamp=(uint64_t)((long double)ticks*G.properties.limits.timestampPeriod*12/625);
            uint64_t result[2]={op->kind==2?G.samples_passed:stamp,stamp},off=0;
            VPool *p=pool_find(op->address,16,&off);if(!p)die("counter report pool lifetime",op->address);
            if(p->v.map)memcpy(p->v.map+off,result,16);memcpy((void *)(uintptr_t)op->address,result,16);}
    }
    vkDestroyQueryPool(G.device,G.occlusion,NULL);vkDestroyQueryPool(G.device,G.timestamps,NULL);
    G.occlusion=G.timestamps=VK_NULL_HANDLE;
    for(int i=0;i<G.nevents;i++) { Event *e=&G.events[i]; if(e->mode) atomic_fetch_add((_Atomic uint32_t *)e->addr,e->value);
       else atomic_store((_Atomic uint32_t *)e->addr,e->value); }
    G.completed=G.fence; vkDestroyFence(G.device,fence,NULL); vkFreeCommandBuffers(G.device,G.command_pool,1,&G.command);
    for(int i=0;i<G.npipeline;i++) { vkDestroyPipeline(G.device,G.pipelines[i],NULL); vkDestroyPipelineLayout(G.device,G.layouts[i],NULL); }
    for(int i=0;i<G.npass;i++) { vkDestroyFramebuffer(G.device,G.framebuffers[i],NULL); vkDestroyRenderPass(G.device,G.passes[i],NULL); }
    for(int i=0;i<G.ntemporary;i++){buffer_free(G.temporaries[i]);free(G.temporaries[i]);} vkDestroyDescriptorPool(G.device,G.descriptors,NULL);
    for(int i=0;i<G.nview;++i)vkDestroyImageView(G.device,G.temporary_views[i],NULL);
    for(int i=0;i<G.nbuffer_view;++i)vkDestroyBufferView(G.device,G.buffer_views[i],NULL);
    G.command=VK_NULL_HANDLE; G.active=0; pthread_mutex_unlock(&G.lock); return G.fence;
}
uint64_t mtl_last_fence(void) { return G.fence; }
int mtl_last_commit_error(void) { return 0; } /* Errors terminate at CHECK before completion is published. */
int mtl_wait(uint64_t fence,uint64_t timeout) { (void)timeout; return fence<=G.completed; }
void mtl_finish(void) { mtl_init(); pthread_mutex_lock(&G.lock); CHECK(vkQueueWaitIdle(G.queue)); pthread_mutex_unlock(&G.lock); }
void mtl_drain(void) { mtl_finish(); }

static VkFormat format(int nvn) {
    switch(nvn) {
    case NVN_FORMAT_R8:return VK_FORMAT_R8_UNORM; case NVN_FORMAT_RG8:return VK_FORMAT_R8G8_UNORM;
    case NVN_FORMAT_R16:return VK_FORMAT_R16_UNORM;case NVN_FORMAT_RG16:return VK_FORMAT_R16G16_UNORM;case NVN_FORMAT_RGBA16:return VK_FORMAT_R16G16B16A16_UNORM;
    case NVN_FORMAT_R8SN:return VK_FORMAT_R8_SNORM;case NVN_FORMAT_RG8SN:return VK_FORMAT_R8G8_SNORM;case NVN_FORMAT_RGBA8SN:return VK_FORMAT_R8G8B8A8_SNORM;
    case NVN_FORMAT_R16SN:return VK_FORMAT_R16_SNORM;case NVN_FORMAT_RG16SN:return VK_FORMAT_R16G16_SNORM;case NVN_FORMAT_RGBA16SN:return VK_FORMAT_R16G16B16A16_SNORM;
    case NVN_FORMAT_R8UI:return VK_FORMAT_R8_UINT;case NVN_FORMAT_RG8UI:return VK_FORMAT_R8G8_UINT;case NVN_FORMAT_RGBA8UI:return VK_FORMAT_R8G8B8A8_UINT;
    case NVN_FORMAT_R32UI:return VK_FORMAT_R32_UINT;case NVN_FORMAT_RG32UI:return VK_FORMAT_R32G32_UINT;case NVN_FORMAT_RGBA32UI:return VK_FORMAT_R32G32B32A32_UINT;
    case NVN_FORMAT_R16F:return VK_FORMAT_R16_SFLOAT; case NVN_FORMAT_RG16F:return VK_FORMAT_R16G16_SFLOAT;
    case NVN_FORMAT_RGBA16F:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case NVN_FORMAT_R11G11B10F:return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case NVN_FORMAT_R32F:return VK_FORMAT_R32_SFLOAT; case NVN_FORMAT_RG32F:return VK_FORMAT_R32G32_SFLOAT;
    case NVN_FORMAT_RGB32F:return VK_FORMAT_R32G32B32_SFLOAT; case NVN_FORMAT_RGBA32F:return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NVN_FORMAT_RGBA8:return VK_FORMAT_R8G8B8A8_UNORM; case NVN_FORMAT_RGBA8_SRGB:return VK_FORMAT_R8G8B8A8_SRGB;
    case NVN_FORMAT_RGBA8_UI2F:return VK_FORMAT_R8G8B8A8_USCALED;case NVN_FORMAT_RGBA8_I2F:return VK_FORMAT_R8G8B8A8_SSCALED;
    case NVN_FORMAT_DEPTH16:return VK_FORMAT_D16_UNORM; case NVN_FORMAT_DEPTH32F:return VK_FORMAT_D32_SFLOAT;
    case NVN_FORMAT_DEPTH24_STENCIL8:return VK_FORMAT_D24_UNORM_S8_UINT;
    case NVN_FORMAT_DEPTH32F_STENCIL8:return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:die("NVN pixel/vertex format",nvn);
    }
}
static VkImageAspectFlags aspect(VkFormat f) {
    if(f==VK_FORMAT_D16_UNORM || f==VK_FORMAT_D32_SFLOAT) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if(f==VK_FORMAT_D24_UNORM_S8_UINT || f==VK_FORMAT_D32_SFLOAT_S8_UINT) return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}
static void texture_transition(VTexture *t,VkImageLayout layout) {
    if(t->layout==layout) return; pass_end();
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
       .srcAccessMask=t->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,
       .dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,.oldLayout=t->layout,.newLayout=layout,
       .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->image,
       .subresourceRange={aspect(t->format),0,t->source.d.levels,0,t->source.L.layers}};
    vkCmdPipelineBarrier(G.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
    t->layout=layout;
}
/* GPU transfers and CPU streaming can update different compression blocks of
 * one NVN allocation. Compare actual block bytes to the last CPU snapshot so
 * page-level dirtiness never overwrites unrelated GPU-owned contents. */
static void refresh_compressed_cpu(VTexture *t,const tex_t *source) {
    uint64_t started=dm_android_nvn_clock_ns(),stamp=nvn_dirty_now();nvn_dirty_protect(source->mem,source->L.size);
    uint8_t *snapshot=malloc(source->L.size);if(!snapshot)die("streamed BC snapshot allocation",source->L.size);
    memcpy(snapshot,source->mem,source->L.size);
    unsigned changed_regions=0;
    for(unsigned layer=0;layer<source->L.layers;layer++)for(unsigned level=0;level<source->d.levels;level++){
        unsigned width=t->width>>level,height=t->height>>level,depth=t->depth>>level;if(!width)width=1;if(!height)height=1;if(!depth)depth=1;
        int bw,bh;int block=nvn_fmt_bpb(source->d.format,&bw,&bh);unsigned columns=(width+bw-1)/bw,rows=(height+bh-1)/bh;
        uint64_t row=(uint64_t)columns*block,linear_bytes=row*rows*depth;
        uint8_t *old=malloc(linear_bytes),*next=malloc(linear_bytes);if(!old||!next)die("streamed BC block comparison allocation",linear_bytes);
        if(nvn_deswizzle(&source->d,level,layer,t->cpu_snapshot,old,row)||nvn_deswizzle(&source->d,level,layer,snapshot,next,row))die("streamed BC block layout",source->serial);
        if(!memcmp(old,next,linear_bytes)){free(old);free(next);continue;}
        uint64_t plane=((uint64_t)width*height*t->bpp+3)&~3ULL;
        VBuffer *staging=temporary(plane*depth,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for(unsigned z=0;z<depth;z++)if(nvn_bc_decode(source->d.format,next+z*row*rows,row,width,height,staging->map+z*plane,(size_t)width*t->bpp))die("streamed BC block decode",source->d.format);
        VkBufferImageCopy *regions=NULL;unsigned count=0,capacity=0;
        for(unsigned z=0;z<depth;z++)for(unsigned y=0;y<rows;y++){
            unsigned x=0;while(x<columns){uint64_t offset=((uint64_t)z*rows+y)*row+(uint64_t)x*block;
                if(!memcmp(old+offset,next+offset,block)){x++;continue;}
                unsigned first=x++;while(x<columns&&memcmp(old+((uint64_t)z*rows+y)*row+(uint64_t)x*block,next+((uint64_t)z*rows+y)*row+(uint64_t)x*block,block))x++;
                if(count==capacity){capacity=capacity?capacity*2:64;VkBufferImageCopy *larger=realloc(regions,(size_t)capacity*sizeof(*regions));if(!larger)die("streamed BC copy regions",capacity);regions=larger;}
                unsigned px=first*bw,py=y*bh,w=(x-first)*bw,h=bh;if(w>width-px)w=width-px;if(h>height-py)h=height-py;
                regions[count++]=(VkBufferImageCopy){.bufferOffset=z*plane+((uint64_t)py*width+px)*t->bpp,.bufferRowLength=width,.bufferImageHeight=height,
                    .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1},.imageOffset={px,py,z},.imageExtent={w,h,1}};
            }
        }
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(G.command,staging->b,t->image,t->layout,count,regions);changed_regions+=count;
        free(regions);free(old);free(next);
    }
    free(t->cpu_snapshot);t->cpu_snapshot=snapshot;t->upload_stamp=stamp;
    if(changed_regions){t->version++;G.uploads++;fprintf(stderr,"[nvn-vulkan] BC_STREAM_REFRESH serial=%llu changed_regions=%u gpu_blocks_preserved=1\n",(unsigned long long)t->serial,changed_regions);}
    G.profile_decode_ns+=dm_android_nvn_clock_ns()-started;
}
static VTexture *texture_get(const tex_t *source,int upload) {
    if(!source || !source->serial) die("texture serial",0);
    VTexture *t=NULL;
    for(int i=0;i<G.ntexture;i++) if(G.textures[i].serial==source->serial) {t=&G.textures[i];break;}
    if(t) {
        uint32_t decoded_format;int decoded_bpp;
        if(upload&&!t->cpu_owned&&t->cpu_snapshot&&nvn_bc_unpacked(source->d.format,&decoded_format,&decoded_bpp)&&nvn_dirty_since(source->mem,source->L.size,t->upload_stamp)){
            refresh_compressed_cpu(t,source);return t;
        }
        // GPU render targets remain resident. Only CPU-backed sampled images
        // use dirty generations to refresh streamed contents at the same serial.
        if(!upload || !t->cpu_owned || !nvn_dirty_since(source->mem,source->L.size,t->upload_stamp))return t;
        goto upload_existing;
    }
    int one=source->d.target==NVN_TEXTURE_TARGET_1D || source->d.target==NVN_TEXTURE_TARGET_1D_ARRAY;
    int layered=source->d.target==NVN_TEXTURE_TARGET_2D_ARRAY || source->d.target==NVN_TEXTURE_TARGET_1D_ARRAY;
    int volume=source->d.target==NVN_TEXTURE_TARGET_3D;
    int cube=source->d.target==NVN_TEXTURE_TARGET_CUBEMAP || source->d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    if(G.ntexture==LIMIT_OBJECTS || (source->d.target!=NVN_TEXTURE_TARGET_2D && !one && !layered && !cube && !volume) || source->d.samples>1)
        die("texture target/layers/MSAA",source->d.target);
    t=&G.textures[G.ntexture++];memset(t,0,sizeof(*t));t->serial=source->serial; t->source=*source;t->version=1;
    t->width=source->d.width; t->height=one?1:source->d.height;t->depth=volume?source->d.depth:1;t->layout=VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t metal_format=0; int decompressed_bpp=0;
    int bc=nvn_bc_unpacked(source->d.format,&metal_format,&decompressed_bpp);
    if(bc) {
        switch(metal_format) { case 10:t->format=VK_FORMAT_R8_UNORM;break;case 12:t->format=VK_FORMAT_R8_SNORM;break;
          case 30:t->format=VK_FORMAT_R8G8_UNORM;break;case 32:t->format=VK_FORMAT_R8G8_SNORM;break;
          case 70:t->format=VK_FORMAT_R8G8B8A8_UNORM;break;case 71:t->format=VK_FORMAT_R8G8B8A8_SRGB;break;
          case 115:t->format=VK_FORMAT_R16G16B16A16_SFLOAT;break;default:die("BC decoded format",metal_format); }
        t->bpp=decompressed_bpp;
    } else { t->format=format(source->d.format); t->bpp=source->L.bpb; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(G.physical,t->format,&fp);
    VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(aspect(t->format)&VK_IMAGE_ASPECT_DEPTH_BIT) usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    if(G.feedback_layout && (usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))usage|=VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=volume?VK_IMAGE_TYPE_3D:one?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D,.format=t->format,
      .flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
      .extent={t->width,t->height,t->depth},.mipLevels=source->d.levels,.arrayLayers=source->L.layers,.samples=VK_SAMPLE_COUNT_1_BIT,
      .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateImage(G.device,&ii,NULL,&t->image)); VkMemoryRequirements r; vkGetImageMemoryRequirements(G.device,t->image,&r);
    VkMemoryAllocateInfo a={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,
      .memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(G.device,&a,NULL,&t->memory)); CHECK(vkBindImageMemory(G.device,t->image,t->memory,0));
    VkImageViewType view_type=volume?VK_IMAGE_VIEW_TYPE_3D:one?(layered?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D):cube?(source->L.layers==6?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY):layered?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,.viewType=view_type,
       .format=t->format,.subresourceRange={aspect(t->format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,source->d.levels,0,source->L.layers}}; CHECK(vkCreateImageView(G.device,&vi,NULL,&t->view));
upload_existing:
    if(upload) {
        uint32_t decoded_format=0;int decoded_bpp=0;
        int upload_bc=nvn_bc_unpacked(source->d.format,&decoded_format,&decoded_bpp);
        if(!source->mem || !pool_find((uint64_t)(uintptr_t)source->mem,source->L.size,NULL)) die("packaged texture storage",source->serial);
        // Protect before reading and stamp before protection: any later CPU
        // write is newer than this snapshot, even if it races the upload.
        uint64_t stamp=nvn_dirty_now();nvn_dirty_protect(source->mem,source->L.size);
        const uint8_t *upload_memory=source->mem;
        if(upload_bc){uint8_t *snapshot=malloc(source->L.size);if(!snapshot)die("BC CPU baseline snapshot allocation",source->L.size);
            memcpy(snapshot,source->mem,source->L.size);free(t->cpu_snapshot);t->cpu_snapshot=snapshot;upload_memory=snapshot;}
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        uint64_t decode_start=dm_android_nvn_clock_ns();
        for(unsigned layer=0;layer<source->L.layers;layer++)for(unsigned level=0;level<source->d.levels;level++) {
            uint32_t w=t->width>>level,h=t->height>>level,z=t->depth>>level;if(!w)w=1;if(!h)h=1;if(!z)z=1;
            int bw=1,bh=1; int bb=nvn_fmt_bpb(source->d.format,&bw,&bh); if(!bb)die("texture block size",source->d.format);
            uint64_t row=(uint64_t)((w+bw-1)/bw)*bb,rows=(h+bh-1)/bh;
            uint8_t *linear=malloc(row*rows*z); if(!linear)die("texture decode allocation",row*rows*z);
            if(source->d.flags&NVN_TEXTURE_FLAGS_LINEAR) { free(linear); die("linear texture upload layout",source->d.flags); }
            if(nvn_deswizzle(&source->d,level,layer,upload_memory,linear,row))die("Tegra texture deswizzle",level);
            VBuffer *staging=temporary((uint64_t)w*h*z*t->bpp,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            if(upload_bc) {for(unsigned slice=0;slice<z;++slice)if(nvn_bc_decode(source->d.format,linear+slice*row*rows,row,w,h,staging->map+(size_t)slice*w*h*t->bpp,(size_t)w*t->bpp))die("BC decode",source->d.format); }
            else memcpy(staging->map,linear,(size_t)w*h*z*t->bpp); free(linear);
            VkImageAspectFlags upload_aspect=aspect(t->format);
            if(upload_aspect&VK_IMAGE_ASPECT_STENCIL_BIT)die("packaged depth/stencil upload requires separate aspect conversion",source->d.format);
            VkBufferImageCopy copy={.imageSubresource={upload_aspect,level,layer,1},.imageExtent={w,h,z}};
            vkCmdCopyBufferToImage(G.command,staging->b,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        }
        G.profile_decode_ns+=dm_android_nvn_clock_ns()-decode_start;
        t->cpu_owned=1;t->upload_stamp=stamp;t->version++; G.uploads++;
    }
    return t;
}
void mtl_tex_release(tex_t *source) {
    if(G.active) die("texture release during submission",source->serial);
    for(int i=0;i<G.ntexture;i++)if(G.textures[i].serial==source->serial){ VTexture *t=&G.textures[i];
       free(t->cpu_snapshot);
       if(t->shadow){vkDestroyImageView(G.device,t->shadow_view,NULL);vkDestroyImage(G.device,t->shadow,NULL);vkFreeMemory(G.device,t->shadow_memory,NULL);}
       vkDestroyImageView(G.device,t->view,NULL); vkDestroyImage(G.device,t->image,NULL); vkFreeMemory(G.device,t->memory,NULL);
       G.textures[i]=G.textures[--G.ntexture]; return; }
}
void mtl_reg_tex(int id,const tex_t *t,const view_t *v) {
    if(id<0 || id>=G.texture_pool_n || (size_t)id>=G.texture_slots)die("texture descriptor id",id); G.tslot[id].live=t!=NULL;
    if(t) G.tslot[id].texture=*t; G.tslot[id].has_view=v!=NULL; if(v)G.tslot[id].view=*v;
}
static VkSamplerAddressMode wrap(unsigned w) { switch(w) {
    case NVN_WRAP_MODE_REPEAT:return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_EDGE:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;default:die("sampler wrap mode",w); } }
void mtl_reg_samp(int id,const samp_par *p) {
    mtl_init(); if(id<0 || id>=LIMIT_OBJECTS || !p)die("sampler descriptor id",id); SSlot *s=&G.sslot[id];
    if(s->live) { if(G.active)die("sampler re-register in flight",id); vkDestroySampler(G.device,s->sampler,NULL); }
    s->source=*p;
    VkSamplerCreateInfo i={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
       .magFilter=p->magf==NVN_MAG_FILTER_NEAREST?VK_FILTER_NEAREST:VK_FILTER_LINEAR,
       .minFilter=(p->minf==NVN_MIN_FILTER_NEAREST || p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST || p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR)?VK_FILTER_NEAREST:VK_FILTER_LINEAR,
       .mipmapMode=(p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf==NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR)?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,
       .addressModeU=wrap(p->ws),.addressModeV=wrap(p->wt),.addressModeW=wrap(p->wr),.minLod=p->lod_min,.maxLod=p->lod_max,
       .mipLodBias=p->lod_bias,.anisotropyEnable=p->aniso>1,.maxAnisotropy=p->aniso>1?p->aniso:1,
       .compareEnable=p->cmp_mode==NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE,.compareOp=(VkCompareOp)(p->cmp_func-1),
       .borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    if(p->border[0]==0 && p->border[1]==0 && p->border[2]==0 && p->border[3]==1)
        i.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if(p->border[0]==1 && p->border[1]==1 && p->border[2]==1 && p->border[3]==1)
        i.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if(p->border[0] || p->border[1] || p->border[2] || p->border[3]) {
        fprintf(stderr,"[nvn-vulkan] unsupported sampler border rgba=%g,%g,%g,%g wrap=%u,%u,%u\n",
            p->border[0],p->border[1],p->border[2],p->border[3],p->ws,p->wt,p->wr);
        die("custom sampler border color",id);
    }
    CHECK(vkCreateSampler(G.device,&i,NULL,&s->sampler)); s->live=1;
}
void mtl_set_tex_pool_n(int n) {
    /* NVN handles encode a 20-bit texture descriptor ID. Descriptor slots are
     * CPU metadata, independent of the number of resident Vulkan images. */
    if(n<0 || n>(1<<20))die("texture pool capacity",n);
    if((size_t)n>G.texture_slots) {
        TSlot *slots=realloc(G.tslot,(size_t)n*sizeof(*slots));
        if(!slots)die("texture descriptor allocation",n);
        memset(slots+G.texture_slots,0,((size_t)n-G.texture_slots)*sizeof(*slots));
        G.tslot=slots;G.texture_slots=(size_t)n;
    }
    G.texture_pool_n=n;
    fprintf(stderr,"[nvn-vulkan] texture descriptor slots=%d resident image limit=%d\n",n,LIMIT_OBJECTS);
}

static uint8_t *read_file(const char *name,size_t *size) {
    char path[1200]; if(!G.shader_root[0] || snprintf(path,sizeof(path),"%s/%s",G.shader_root,name)>=(int)sizeof(path))die("shader cache path",0);
    FILE *f=fopen(path,"rb"); if(!f)die(path,0); if(fseek(f,0,SEEK_END))die("shader cache seek",0);
    long n=ftell(f); if(n<0 || n>64*1024*1024 || fseek(f,0,SEEK_SET))die("shader cache file size",n);
    uint8_t *bytes=malloc(n?n:1); if(!bytes || fread(bytes,1,n,f)!=(size_t)n)die("shader cache read",n);
    fclose(f);*size=n;return bytes;
}
static uint8_t *buffer_image_module(uint8_t *bytes,size_t *size,VShader *shader) {
    uint32_t *w=(uint32_t *)bytes;unsigned bound=w[3];if(bound>1000000)die("buffer image id bound",bound);
    uint32_t *definition=calloc(bound,4),*variable=calloc(bound,4),*loaded=calloc(bound,4),*pointers=calloc(bound,4);
    if(!definition||!variable||!loaded||!pointers)die("buffer image audit allocation",bound);
    for(size_t o=5;o<*size/4;) {unsigned n=w[o]>>16,op=w[o]&65535;if(!n||n>*size/4-o)die("buffer image instruction",o);
        if((op==25||op==27||op==32)&&w[o+1]<bound)definition[w[o+1]]=o;o+=n;}
    unsigned next=bound,count=0;
    for(size_t o=5;o<*size/4;o+=w[o]>>16)if((w[o]&65535)==59&&w[o+1]<bound) {
        uint32_t p=definition[w[o+1]];
        if(!p||(w[p]&65535)!=32||w[p+2]!=0)continue;
        uint32_t s=w[p+3]<bound?definition[w[p+3]]:0;
        if(!s||(w[s]&65535)!=27)continue;
        uint32_t im=w[s+2]<bound?definition[w[s+2]]:0;
        if(!im||(w[im]&65535)!=25||w[im+3]!=5)continue;
        unsigned id=w[o+2],image=w[im+1];if(id>=bound)die("buffer image variable id",id);
        variable[id]=image;if(!pointers[image])pointers[image]=next++;
        int binding=-1;
        for(size_t d=5;d<*size/4;d+=w[d]>>16)if((w[d]&65535)==71&&w[d]>>16==4&&w[d+1]==id&&w[d+2]==33)binding=w[d+3];
        int found=0;for(unsigned i=0;i<shader->h.nb;i++)if((int)shader->b[i].binding==binding){if(shader->b[i].kind!=4||shader->b[i].type!=1)die("buffer image metadata",binding);shader->b[i].kind=10;shader->b[i].type=4;found=1;}
        if(!found)die("buffer image descriptor binding",binding);count++;
    }
    if(!count){free(definition);free(variable);free(loaded);free(pointers);return bytes;}
    uint32_t *out=malloc(*size+4*(next-bound)*4);if(!out)die("buffer image module allocation",*size);memcpy(out,w,20);out[3]=next;size_t at=5;unsigned extracts=0;
    for(size_t o=5;o<*size/4;o+=w[o]>>16) {
        unsigned n=w[o]>>16,op=w[o]&65535;memcpy(out+at,w+o,n*4);
        if(op==25&&w[o+1]<bound&&pointers[w[o+1]]){at+=n;uint32_t ptr[]={4u<<16|32,pointers[w[o+1]],0,w[o+1]};memcpy(out+at,ptr,16);at+=4;continue;}
        if(op==59&&w[o+2]<bound&&variable[w[o+2]])out[at+1]=pointers[variable[w[o+2]]];
        if(op==61&&w[o+3]<bound&&variable[w[o+3]]){out[at+1]=variable[w[o+3]];if(w[o+2]>=bound)die("buffer image load id",w[o+2]);loaded[w[o+2]]=out[at+1];}
        if(op==100&&w[o+3]<bound&&loaded[w[o+3]]){if(n!=4||w[o+1]!=loaded[w[o+3]])die("buffer image extraction",w[o+2]);out[at]=4u<<16|83;extracts++;}
        if(((op>=87&&op<=94)||op==96||op==97||op==105)&&n>3&&w[o+3]<bound&&loaded[w[o+3]])die("buffer image cannot use sampler",op);
        at+=n;
    }
    if(!extracts)die("buffer image had no image extraction",count);
    fprintf(stderr,"[nvn-vulkan] samplerless buffer adaptation shader=%s descriptors=%u extractions=%u\n",shader->key,count,extracts);
    free(definition);free(variable);free(loaded);free(pointers);free(bytes);*size=at*4;return (uint8_t *)out;
}
static uint64_t control_hash(const void *control,uint32_t stage) {
    const uint8_t *bytes=control;uint64_t hash=14695981039346656037ULL^stage;
    for(unsigned i=0;i<0x880;i++){hash^=bytes[i];hash*=1099511628211ULL;}return hash;
}
static const uint8_t *shader_entry(const void *control,uint32_t stage) {
    if(!control)die("null shader control",stage);
    if(!G.shader_index) { G.shader_index=read_file("shader_index.bin",&G.index_bytes);
        if(G.index_bytes<12)die("shader index header",G.index_bytes); uint32_t *h=(uint32_t *)G.shader_index;
        if(h[0]!=0x494b5644 || h[1]!=1 || G.index_bytes!=12+(size_t)h[2]*(20+0x880))die("shader index schema",h[0]); }
    if(!G.shader_entries){
        uint32_t count=((uint32_t *)G.shader_index)[2];if(count>1000000)die("shader control index count",count);
        size_t capacity=16;while(capacity<(size_t)count*2)capacity*=2;
        G.shader_entries=calloc(capacity,sizeof(*G.shader_entries));if(!G.shader_entries)die("shader control index allocation",capacity);
        G.shader_entry_capacity=capacity;const uint8_t *entry=G.shader_index+12;
        for(uint32_t i=0;i<count;i++,entry+=20+0x880){uint32_t st;memcpy(&st,entry+16,4);uint64_t hash=control_hash(entry+20,st);size_t slot=hash&(capacity-1);
            while(G.shader_entries[slot].entry){const uint8_t *old=G.shader_entries[slot].entry;
                if(G.shader_entries[slot].hash==hash&&!memcmp(old+16,entry+16,4+0x880))break;
                slot=(slot+1)&(capacity-1);}
            /* Preserve the first matching verified entry, including duplicates. */
            if(!G.shader_entries[slot].entry)G.shader_entries[slot]=(ShaderEntry){entry,hash};
        }
        fprintf(stderr,"[nvn-vulkan] SHADER_CONTROL_INDEX entries=%u slots=%zu bytes=%zu full_control_verified=1\n",count,capacity,capacity*sizeof(*G.shader_entries));
    }
    uint64_t hash=control_hash(control,stage);size_t slot=hash&(G.shader_entry_capacity-1);
    while(G.shader_entries[slot].entry){ShaderEntry *candidate=&G.shader_entries[slot];uint32_t st;memcpy(&st,candidate->entry+16,4);
        if(candidate->hash==hash&&st==stage&&!memcmp(candidate->entry+20,control,0x880))return candidate->entry;
        slot=(slot+1)&(G.shader_entry_capacity-1);}
    return NULL;
}
static VShader *shader_get(const void *control,uint32_t stage) {
    const uint8_t *entry=shader_entry(control,stage);if(!entry)die("original shader not in verified Vulkan cache",stage);
    char key[17]={0};memcpy(key,entry,16);
    for(int i=0;i<G.nshader;i++)if(!strcmp(G.shaders[i].key,key))return &G.shaders[i];
    if(G.nshader==LIMIT_OBJECTS)die("shader object limit",G.nshader); VShader *s=&G.shaders[G.nshader++]; strcpy(s->key,key);
    char name[40];snprintf(name,sizeof(name),"%s.vkmeta",key);size_t size=0;uint8_t *bytes=read_file(name,&size);
    if(size<sizeof(RHeader))die("shader metadata header",size);memcpy(&s->h,bytes,sizeof(RHeader));
    if(s->h.magic!=0x4d4b5644 || s->h.version!=1 || s->h.stage!=stage || s->h.nb>LIMIT_BINDINGS || s->h.na>32 || s->h.ncap>32 || s->h.c1n>4096)
       die("shader metadata bounds",stage);
    size_t expected=sizeof(RHeader)+s->h.nb*sizeof(RBinding)+s->h.na*8+s->h.ncap*4+s->h.c1n;
    if(size!=expected)die("shader metadata size",size);uint8_t *p=bytes+sizeof(RHeader);
    memcpy(s->b,p,s->h.nb*sizeof(RBinding));p+=s->h.nb*sizeof(RBinding);memcpy(s->attr,p,s->h.na*8);p+=s->h.na*8;
    memcpy(s->caps,p,s->h.ncap*4);p+=s->h.ncap*4;
    for(uint32_t i=0;i<s->h.ncap;i++) {
       unsigned cap=s->caps[i];VkSubgroupFeatureFlags feature=0;
       if(cap>=61&&cap<=68){const VkSubgroupFeatureFlags features[]={VK_SUBGROUP_FEATURE_BASIC_BIT,VK_SUBGROUP_FEATURE_VOTE_BIT,VK_SUBGROUP_FEATURE_ARITHMETIC_BIT,VK_SUBGROUP_FEATURE_BALLOT_BIT,VK_SUBGROUP_FEATURE_SHUFFLE_BIT,VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT,VK_SUBGROUP_FEATURE_CLUSTERED_BIT,VK_SUBGROUP_FEATURE_QUAD_BIT};feature=features[cap-61];}
       if(feature && (G.subgroup.supportedStages&(stage==5?VK_SHADER_STAGE_COMPUTE_BIT:stage==1?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT)) && (G.subgroup.supportedOperations&feature))continue;
       if(cap==51 && stage==1)continue; /* DerivativeControl is core fragment functionality. */
       if(cap!=1 && cap!=46 && cap!=47 && cap!=50 && cap!=55 && cap!=56){fprintf(stderr,"[nvn-vulkan] shader feature key=%s stage=%u cap=%u\n",key,stage,cap);die("shader feature needs device/semantic adaptation",cap);}
    }
    if(s->h.c1n){s->c1=buffer_make(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);memcpy(s->c1.map,p,s->h.c1n);}free(bytes);
    if(s->h.reserved) {
        /* Metadata records the exact uncompressed size. zlib validates gzip
         * CRC32 and ISIZE; reject trailing/concatenated streams and short data.
         * The IR is exactly the offline-validated original SPIR-V, unchanged.
         */
        uint32_t expected_size=s->h.reserved;
        if(expected_size<20 || expected_size%4 || expected_size>16*1024*1024)die("compressed SPIR-V declared length",expected_size);
        snprintf(name,sizeof(name),"%s.spv.gz",key);uint8_t *compressed=read_file(name,&size);
        if(size<18 || size>UINT32_MAX || compressed[0]!=0x1f || compressed[1]!=0x8b || compressed[2]!=8)die("shader gzip header",size);
        bytes=malloc(expected_size);if(!bytes)die("SPIR-V inflate allocation",expected_size);
        z_stream stream={.next_in=compressed,.avail_in=(uInt)size,.next_out=bytes,.avail_out=expected_size};
        int initialized=inflateInit2(&stream,15+16);if(initialized!=Z_OK)die("shader gzip initialize",initialized);
        int result=inflate(&stream,Z_FINISH);int complete=result==Z_STREAM_END && stream.total_out==expected_size && stream.avail_in==0;
        int ended=inflateEnd(&stream);free(compressed);
        if(!complete || ended!=Z_OK)die("shader gzip CRC/length/trailing-data validation",result);
        size=expected_size;
    } else {snprintf(name,sizeof(name),"%s.spv",key);bytes=read_file(name,&size);}
    if(size%4 || size<20 || ((uint32_t *)bytes)[0]!=0x07230203)die("SPIR-V module",size);
    bytes=buffer_image_module(bytes,&size,s);
    uint32_t *words=(uint32_t *)bytes;int fetch=0,sampling=0;
    for(size_t offset=5;offset<size/4;) {
        unsigned length=words[offset]>>16,op=words[offset]&0xffff;
        if(!length || length>size/4-offset)die("SPIR-V instruction bounds",offset);
        if(op==95 || op==313)fetch=1; /* OpImageFetch / OpImageSparseFetch */
        if((op>=87&&op<=94) || op==96 || op==97 || op==105 || (op>=305&&op<=312) || op==314 || op==315)sampling=1;
        offset+=length;
    }
    (void)fetch;(void)sampling;
    unsigned resource_bound=words[3];if(resource_bound>1000000)die("SPIR-V resource id bound",resource_bound);
    uint64_t *resource=calloc(resource_bound,sizeof(*resource));if(!resource)die("SPIR-V resource audit allocation",resource_bound);
    for(size_t o=5;o<size/4;o+=words[o]>>16)if((words[o]&65535)==71&&words[o]>>16==4&&words[o+2]==33&&words[o+1]<resource_bound)
        for(unsigned b=0;b<s->h.nb;b++)if(s->b[b].binding==words[o+3])resource[words[o+1]]=1ull<<b;
    uint64_t extracted=0,used_sampler=0;
    for(size_t o=5;o<size/4;o+=words[o]>>16){unsigned op=words[o]&65535,n=words[o]>>16;
        if((op==61||op==83||op==100)&&n>=4&&words[o+2]<resource_bound&&words[o+3]<resource_bound){resource[words[o+2]]=resource[words[o+3]];if(op==100)extracted|=resource[words[o+3]];}
        if(op==86&&n>=5&&words[o+2]<resource_bound&&words[o+3]<resource_bound)resource[words[o+2]]=resource[words[o+3]];
        if((op==89||op==90||op==93||op==94||op==97||op==307||op==308||op==311||op==312||op==315)&&n>=4&&words[o+3]<resource_bound)s->dref_bindings|=resource[words[o+3]];
        if(((op>=87&&op<=94)||op==96||op==97||op==105||(op>=305&&op<=312)||op==314||op==315)&&n>=4&&words[o+3]<resource_bound)used_sampler|=resource[words[o+3]];
    }
    s->fetch_bindings=extracted&~used_sampler;free(resource);
    /* DRIVER resources are the original binding tables, not zero constants.
     * Verify every direct access is a statically known table field before
     * constructing them from live NVN bindings. Reject unknown/dynamic reads.
     */
    unsigned bound=words[3];if(bound>1000000)die("SPIR-V id bound",bound);
    uint32_t *constants=calloc(bound,sizeof(*constants));uint8_t *known=calloc(bound,1);
    if(!constants || !known)die("SPIR-V driver audit allocation",bound);
    for(size_t o=5;o<size/4;o+=words[o]>>16)if((words[o]&0xffff)==43 && words[o]>>16==4 && words[o+2]<bound){constants[words[o+2]]=words[o+3];known[words[o+2]]=1;}
    for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==8) {
        RBinding *b=&s->b[i];uint32_t variable=0;
        if(b->slot!=0 && b->slot!=2)die("driver constant bank",b->slot);
        for(size_t o=5;o<size/4;o+=words[o]>>16)if((words[o]&0xffff)==71 && words[o]>>16==4 && words[o+2]==33 && words[o+3]==b->binding)variable=words[o+1];
        if(!variable)die("driver constant variable",b->binding);
        for(size_t o=5;o<size/4;o+=words[o]>>16)if(((words[o]&0xffff)==65 || (words[o]&0xffff)==66) && words[o+3]==variable) {
            if(words[o]>>16!=7)die("driver access shape",b->binding);
            uint32_t a=words[o+4],v=words[o+5],c=words[o+6];
            if(a>=bound||v>=bound||c>=bound||!known[a]||!known[v]||!known[c]||constants[a]||constants[c]>3)die("dynamic driver constant access",b->binding);
            uint64_t offset=(uint64_t)constants[v]*16+constants[c]*4;int valid=0;
            if(b->slot==0){unsigned base=stage==5?0x310:stage==1?0x510:0x110;
                valid=offset>=base && offset<base+256 && (offset-base)%16<12;
                if(stage==5 && offset>=0x210 && offset<0x310 && (offset-0x210)%16<12)valid=1;
            }else valid=(offset>=0x120&&offset<0x168&&(offset-0x120)%8==0)||(offset>=0x168&&offset<0x568&&(offset-0x168)%8==0)||(offset>=0x568&&offset<0x968&&(offset-0x568)%8==0);
            if(!valid)die("driver constant access outside verified binding tables",offset);
        }
    }
    free(constants);free(known);
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=(uint32_t *)bytes};
    CHECK(vkCreateShaderModule(G.device,&mi,NULL,&s->module));free(bytes);
    fprintf(stderr,"[nvn-vulkan] original shader key=%s stage=%u loaded\n",s->key,stage);
    VkDescriptorSetLayoutBinding bindings[LIMIT_BINDINGS];for(uint32_t i=0;i<s->h.nb;i++){
       if(s->b[i].count!=1 || s->b[i].set!=(stage==1?1u:0u))die("shader descriptor set/count",s->b[i].set);
       bindings[i]=(VkDescriptorSetLayoutBinding){s->b[i].binding,(VkDescriptorType)s->b[i].type,1,stage==0?VK_SHADER_STAGE_VERTEX_BIT:stage==1?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_COMPUTE_BIT,NULL}; }
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=s->h.nb,.pBindings=bindings};
    CHECK(vkCreateDescriptorSetLayout(G.device,&li,NULL,&s->layout));return s;
}
void mtl_prog_release(prog_t *p) { (void)p; /* Programs own only immutable shared shader-cache entries; no per-program GPU allocation. */ }
static void aligned_writeback(void) {
    for(int i=0;i<G.naligned;i++)if(G.aligned[i].written){AlignedBinding *b=&G.aligned[i];uint64_t offset=0;
        VPool *p=pool_find(b->range.addr,b->range.size,&offset);if(!p)die("aligned binding pool lifetime",b->range.addr);
        all_barrier();VkBufferCopy copy={0,offset,b->range.size};vkCmdCopyBuffer(G.command,b->buffer->b,p->v.b,1,&copy);
    }
    G.naligned=0;
}
static int depth_read_only(void) {
    const ds_st *d=&G.state.ds;if(d->dw)return 0;
    if(!d->st || (!G.state.stencil_write[0]&&!G.state.stencil_write[1]))return 1;
    return d->fail_f==NVN_STENCIL_OP_KEEP&&d->zfail_f==NVN_STENCIL_OP_KEEP&&d->zpass_f==NVN_STENCIL_OP_KEEP&&d->fail_b==NVN_STENCIL_OP_KEEP&&d->zfail_b==NVN_STENCIL_OP_KEEP&&d->zpass_b==NVN_STENCIL_OP_KEEP;
}
static void collect_image_uses(VShader *s) {
    if(!s)return;
    for(unsigned i=0;i<s->h.nb;i++) {RBinding *b=&s->b[i];if(b->kind!=4&&b->kind!=6)continue;
        if(b->slot<0 || b->slot>=(b->kind==6?32:128))die("image-use shader slot",b->slot);
        uint64_t handle=b->kind==6?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot];unsigned tid=handle&0xfffff;
        if(tid>=G.texture_slots||!G.tslot[tid].live)die("image-use texture descriptor",tid);
        uint64_t serial=G.tslot[tid].texture.serial;unsigned j=0;while(j<G.nimage_uses&&G.image_uses[j].serial!=serial)j++;
        if(j==G.nimage_uses){if(j==2*LIMIT_BINDINGS)die("image-use capacity",j);G.image_uses[G.nimage_uses++]=(ImageUse){serial,0};}
        G.image_uses[j].roles|=b->kind==6?2u:1u;
    }
}
static int storage_image_use(uint64_t serial) {
    for(unsigned i=0;i<G.nimage_uses;i++)if(G.image_uses[i].serial==serial)return (G.image_uses[i].roles&2)!=0;
    return 0;
}
static VkImageLayout sampled_layout(VTexture *t,unsigned stage) {
    if(storage_image_use(t->serial))return VK_IMAGE_LAYOUT_GENERAL;
    if(stage!=5){
        if(G.state.rt.depth && G.state.rt.depth->serial==t->serial){G.depth_sampled=1;
            if(depth_read_only())return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            if(!G.feedback_layout)die("NVN writable depth feedback requires device support",t->serial);
            G.feedback_flags|=VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
            return VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        }
        for(int i=0;i<G.state.rt.n;i++)if(G.state.rt.color[i]&&G.state.rt.color[i]->serial==t->serial){
            if(!G.feedback_layout)die("NVN color feedback requires device support",t->serial);
            G.feedback_flags|=VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
            return VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        }
    }
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
/* NVN permits comparison sampling of normalized color. Vulkan requires a
 * depth image: transfer the identical R16/D16 or R32F/D32F element bits through
 * a GPU buffer, retaining the original color attachment/storage image. */
static void comparison_image(VTexture *t) {
    if(t->depth!=1 || t->source.d.target==NVN_TEXTURE_TARGET_1D || t->source.d.target==NVN_TEXTURE_TARGET_1D_ARRAY)
        die("color comparison image dimensionality",t->source.d.target);
    VkFormat f=t->format==VK_FORMAT_R16_UNORM?VK_FORMAT_D16_UNORM:t->format==VK_FORMAT_R32_SFLOAT?VK_FORMAT_D32_SFLOAT:VK_FORMAT_UNDEFINED;
    if(f==VK_FORMAT_UNDEFINED)die("color comparison image format",t->format);
    if(!t->shadow){
        int cube=t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP||t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
            .imageType=VK_IMAGE_TYPE_2D,.format=f,.extent={t->width,t->height,1},.mipLevels=t->source.d.levels,
            .arrayLayers=t->source.L.layers,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        CHECK(vkCreateImage(G.device,&ci,NULL,&t->shadow));VkMemoryRequirements r;vkGetImageMemoryRequirements(G.device,t->shadow,&r);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(G.device,&ai,NULL,&t->shadow_memory));CHECK(vkBindImageMemory(G.device,t->shadow,t->shadow_memory,0));
        VkImageViewType type=cube?(t->source.L.layers==6?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY):t->source.d.target==NVN_TEXTURE_TARGET_2D_ARRAY?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->shadow,.viewType=type,.format=f,
            .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,t->source.d.levels,0,t->source.L.layers}};
        CHECK(vkCreateImageView(G.device,&vi,NULL,&t->shadow_view));t->shadow_format=f;t->shadow_layout=VK_IMAGE_LAYOUT_UNDEFINED;
        fprintf(stderr,"[nvn-vulkan] COLOR_COMPARISON_ALIAS serial=%llu color=%u depth=%u size=%ux%u levels=%u layers=%u\n",(unsigned long long)t->serial,t->format,f,t->width,t->height,t->source.d.levels,t->source.L.layers);
    }
    if(t->shadow_version==t->version)return;
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=t->shadow_layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_SHADER_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=t->shadow_layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->shadow,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,t->source.d.levels,0,t->source.L.layers}};
    vkCmdPipelineBarrier(G.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    for(unsigned level=0;level<t->source.d.levels;level++){
        unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
        uint64_t bytes=(uint64_t)width*height*t->source.L.layers*(f==VK_FORMAT_D16_UNORM?2:4);
        VBuffer *bridge=temporary((bytes+3)&~3ULL,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,t->source.L.layers},.imageExtent={width,height,1}};
        vkCmdCopyImageToBuffer(G.command,t->image,t->layout,bridge->b,1,&region);
        VkBufferMemoryBarrier bb={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=bridge->b,.offset=0,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(G.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&bb,0,NULL);
        region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(G.command,bridge->b,t->shadow,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    }
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(G.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
    t->shadow_layout=barrier.newLayout;t->shadow_version=t->version;
}
static void prepare_comparison_images(VShader *s) {
    if(!s)return;
    for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==4&&(s->dref_bindings&(1ull<<i))){
        unsigned tid=G.state.tex[s->h.stage][s->b[i].slot]&0xfffff;
        VTexture *t=texture_get(&G.tslot[tid].texture,1);
        if(aspect(t->format)==VK_IMAGE_ASPECT_COLOR_BIT)comparison_image(t);
    }
}
static void trace_texture(VTexture *t,const VShader *s,const RBinding *binding) {
    if(!G.trace_frame||s->h.stage!=1||strcmp(s->key,G.trace_ui?"9a650561e5c40791":"5c1ca4e7c3e4657c")||G.ntrace_textures==24||t->depth!=1)return;
    for(unsigned i=0;i<G.ntrace_textures;i++)if(G.trace_texture_serials[i]==t->serial)return;
    uint64_t bytes=(uint64_t)t->width*t->height*t->source.L.layers*t->bpp;
    if(bytes>32*1024*1024)return;
    G.trace_texture_serials[G.ntrace_textures++]=t->serial;
    all_barrier();VkImageLayout layout=t->layout;texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageAspectFlags flags=aspect(t->format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    unsigned bpp=flags==VK_IMAGE_ASPECT_DEPTH_BIT?(t->format==VK_FORMAT_D16_UNORM?2:4):t->bpp;
    for(unsigned level=0;level<t->source.d.levels;level++){
        unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
        bytes=(uint64_t)width*height*t->source.L.layers*bpp;
        VBuffer *capture=temporary((bytes+3)&~3ULL,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy region={.imageSubresource={flags,level,0,t->source.L.layers},.imageExtent={width,height,1}};
        vkCmdCopyImageToBuffer(G.command,t->image,t->layout,capture->b,1,&region);
        char name[160];snprintf(name,sizeof(name),"texture-%04llu-%llu-mip%u",(unsigned long long)G.present_count+1,(unsigned long long)t->serial,level);
        trace_capture_queue(capture,bytes,name);
        fprintf(stderr,"[nvn-vulkan] GPU_TEXTURE_CAPTURE name=%s draw=%llu serial=%llu slot=%d binding=%u format=%u nvn_format=%u extent=%u,%u layers=%u levels=%u mip=%u bpp=%u cpu_owned=%d\n",name,(unsigned long long)G.draws,(unsigned long long)t->serial,binding->slot,binding->binding,t->format,t->source.d.format,width,height,t->source.L.layers,t->source.d.levels,level,bpp,t->cpu_owned);
        /* Compare streamed CPU backing with the actual GPU image. This is
         * diagnostic data only; never substitute it for GPU-owned pixels. */
        uint32_t decoded_format;int decoded_bpp;
        if(t->source.L.layers==1&&nvn_bc_unpacked(t->source.d.format,&decoded_format,&decoded_bpp)&&t->source.mem){
            int bw,bh;unsigned block=nvn_fmt_bpb(t->source.d.format,&bw,&bh);uint64_t row=(uint64_t)((width+bw-1)/bw)*block,rows=(height+bh-1)/bh;
            uint8_t *linear=malloc(row*rows),*decoded=malloc((size_t)width*height*decoded_bpp);if(!linear||!decoded)die("CPU texture diagnostic allocation",bytes);
            if(nvn_deswizzle(&t->source.d,level,0,t->source.mem,linear,row)||nvn_bc_decode(t->source.d.format,linear,row,width,height,decoded,(size_t)width*decoded_bpp))die("CPU texture diagnostic decode",t->serial);
            char path[1400];snprintf(path,sizeof(path),"%s/%s-cpu.bin",getenv("DM_FRAME_DIR"),name);FILE *out=fopen(path,"wb");if(!out)die("CPU texture diagnostic output",0);
            uint64_t cpu_bytes=(uint64_t)width*height*decoded_bpp;if(fwrite(decoded,1,cpu_bytes,out)!=cpu_bytes)die("CPU texture diagnostic write",cpu_bytes);fclose(out);free(linear);free(decoded);
            fprintf(stderr,"[nvn-vulkan] CPU_TEXTURE_CAPTURE bytes=%llu path=%s\n",(unsigned long long)cpu_bytes,path);
        }
    }
    texture_transition(t,layout);
}
static VkDescriptorSet bind_shader(VShader *s) {
    VkDescriptorSetAllocateInfo a={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=G.descriptors,
      .descriptorSetCount=1,.pSetLayouts=&s->layout};VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(G.device,&a,&set));
    VkWriteDescriptorSet w[LIMIT_BINDINGS];VkDescriptorBufferInfo bi[LIMIT_BINDINGS];VkDescriptorImageInfo ii[LIMIT_BINDINGS];
    memset(w,0,sizeof(w));memset(bi,0,sizeof(bi));memset(ii,0,sizeof(ii));
    for(uint32_t i=0;i<s->h.nb;i++) { RBinding *b=&s->b[i]; w[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstSet=set,.dstBinding=b->binding,.descriptorCount=1,.descriptorType=(VkDescriptorType)b->type};
       if(b->kind==1)bi[i]=(VkDescriptorBufferInfo){G.support.b,0,1392};
       else if(b->kind==7) { if(!s->c1.b)die("missing shader c1",i);bi[i]=(VkDescriptorBufferInfo){s->c1.b,0,65536}; }
       else if(b->kind==10) {
           if(b->slot<0 || b->slot>=128)die("texel buffer slot",b->slot);
           uint32_t tid=(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot])&0xfffff;
           if(tid>=G.texture_slots||!G.tslot[tid].live)die("unbound texel buffer",tid);
           TSlot *slot=&G.tslot[tid];tex_t *t=&slot->texture;
           if(t->d.target!=NVN_TEXTURE_TARGET_BUFFER||t->d.levels!=1||(slot->has_view&&(slot->view.set_layers||slot->view.set_levels)))die("texel buffer target/view",tid);
           int nvn=slot->has_view&&slot->view.set_format?slot->view.format:t->d.format;
           VkFormat f=format(nvn);VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(G.physical,f,&properties);
           VkFormatFeatureFlags required=b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER?VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT:VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
           if(!(properties.bufferFeatures&required))die("texel buffer format feature",nvn);
           uint64_t offset=0;VPool *p=pool_read((uint64_t)(uintptr_t)t->mem,t->L.size,&offset);VkBuffer buffer=p->v.b;
           if(offset%G.properties.limits.minTexelBufferOffsetAlignment){VBuffer *aligned=temporary((t->L.size+3)&~3ULL,VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
               if(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER||offset%4||t->L.size%4)die("writable/word-unaligned texel buffer",offset);
               all_barrier();VkBufferCopy copy={offset,0,t->L.size};vkCmdCopyBuffer(G.command,p->v.b,aligned->b,1,&copy);all_barrier();buffer=aligned->b;offset=0;}
           if(G.nbuffer_view==LIMIT_WRITES)die("texel buffer view count",G.nbuffer_view);
           VkBufferViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=buffer,.format=f,.offset=offset,.range=t->L.size};
           CHECK(vkCreateBufferView(G.device,&vi,NULL,&G.buffer_views[G.nbuffer_view]));w[i].pTexelBufferView=&G.buffer_views[G.nbuffer_view++];
           if(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)mark_write((uint64_t)(uintptr_t)t->mem,t->L.size);
           continue;
       }
       else if(b->kind==8) {
           VBuffer *table=temporary(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
           if(b->slot==0) {
               unsigned base=s->h.stage==5?0x310:s->h.stage==1?0x510:0x110;
               for(unsigned j=0;j<16;j++) {Range r=G.state.storage[s->h.stage][j];if(r.size>UINT32_MAX)die("driver buffer size",r.size);uint32_t n=r.size;memcpy(table->map+base+j*16,&r.addr,8);memcpy(table->map+base+j*16+8,&n,4);
                   if(s->h.stage==5){r=G.state.uniform[5][j];if(r.size>UINT32_MAX)die("driver uniform size",r.size);n=r.size;memcpy(table->map+0x210+j*16,&r.addr,8);memcpy(table->map+0x218+j*16,&n,4);}}
           }else if(b->slot==2) {
               for(unsigned j=0;j<128;j++){uint32_t t=G.state.tex[s->h.stage][j],sp=G.state.samp[s->h.stage][j];memcpy(table->map+0x168+j*8,&t,4);memcpy(table->map+0x568+j*8,&sp,4);}
               for(unsigned j=0;j<9;j++){uint32_t image=G.state.img[s->h.stage][j];memcpy(table->map+0x120+j*8,&image,4);}
           }else die("driver table bank",b->slot);
           bi[i]=(VkDescriptorBufferInfo){table->b,0,65536};
       }
       else if(b->kind==2 || b->kind==3 || b->kind==9) {
           if(b->slot<0 || b->slot>=16)die("shader buffer slot",b->slot);
           Range r=(b->kind==2||b->kind==9)?G.state.uniform[s->h.stage][b->slot]:G.state.storage[s->h.stage][b->slot];
           if(!r.addr || !r.size)die("unbound original shader buffer",b->slot);
           uint64_t offset=0;VPool *p=pool_read(r.addr,r.size,&offset);
           uint64_t alignment=b->kind==2?G.properties.limits.minUniformBufferOffsetAlignment:G.properties.limits.minStorageBufferOffsetAlignment;
           if(offset%alignment){
               AlignedBinding *aligned=NULL;
               for(int j=0;j<G.naligned;j++)if(overlap(r.addr,r.size,G.aligned[j].range.addr,G.aligned[j].range.size)){
                   if(r.addr!=G.aligned[j].range.addr || r.size!=G.aligned[j].range.size)die("overlapping unaligned shader ranges",r.addr);
                   aligned=&G.aligned[j];break;
               }
               if(!aligned){if(G.naligned==2*LIMIT_BINDINGS)die("aligned binding count",G.naligned);
                   VBuffer *temp=temporary((r.size+3)&~3ULL,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                   if(offset%4 || r.size%4)die("unaligned copy requires word granularity",offset);
                   all_barrier();VkBufferCopy copy={offset,0,r.size};vkCmdCopyBuffer(G.command,p->v.b,temp->b,1,&copy);all_barrier();
                   aligned=&G.aligned[G.naligned++];*aligned=(AlignedBinding){r,temp,0};
               }
               if(b->kind==3&&b->flags)aligned->written=1;
               bi[i]=(VkDescriptorBufferInfo){aligned->buffer->b,0,r.size};
           }else bi[i]=(VkDescriptorBufferInfo){p->v.b,offset,r.size};
           if(b->kind==3 && b->flags)mark_write(r.addr,r.size);
       } else if(b->kind==4 || b->kind==6) {
           if(b->slot<0 || b->slot>=(b->kind==6?32:128))die("texture shader slot",b->slot);
           uint64_t handle=b->kind==6?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot];
           unsigned tid=(unsigned)handle&0xfffff;if(tid>=(unsigned)G.texture_pool_n || tid>=G.texture_slots || !G.tslot[tid].live)die("unbound texture descriptor",tid);
           TSlot *slot=&G.tslot[tid];
           VTexture *t=texture_get(&slot->texture,1);
           trace_texture(t,s,b);
           int alias=b->kind==4&&(s->dref_bindings&(1ull<<i))&&aspect(t->format)==VK_IMAGE_ASPECT_COLOR_BIT;
           if(!alias)texture_transition(t,storage_image_use(t->serial)?VK_IMAGE_LAYOUT_GENERAL:sampled_layout(t,s->h.stage));
           if(alias&&!t->shadow_view){fprintf(stderr,"[nvn-vulkan] missing comparison view shader=%s binding=%u slot=%d serial=%llu stage=%u\n",s->key,b->binding,b->slot,(unsigned long long)t->serial,s->h.stage);die("comparison image preparation",t->serial);}
           VkImage selected_image=alias?t->shadow:t->image;VkFormat selected_format=alias?t->shadow_format:t->format;
           ii[i].imageView=alias?t->shadow_view:t->view;ii[i].imageLayout=alias?t->shadow_layout:t->layout;
           if(slot->has_view) {
               view_t *v=&slot->view;
               uint32_t level=v->set_levels?v->base_level:0,levels=v->set_levels?v->nlevels:t->source.d.levels;
               uint32_t layer=v->set_layers?v->min_layer:0,layers=v->set_layers?v->nlayers:t->source.L.layers;
               int target=v->set_target?v->target:t->source.d.target;
               if(level>=t->source.d.levels || !levels || levels>t->source.d.levels-level ||
                  layer>=t->source.L.layers || !layers || layers>t->source.L.layers-layer ||
                  (v->set_format && v->format!=t->source.d.format) ||
                  (target!=NVN_TEXTURE_TARGET_2D && target!=NVN_TEXTURE_TARGET_3D && target!=NVN_TEXTURE_TARGET_2D_ARRAY && target!=NVN_TEXTURE_TARGET_CUBEMAP && target!=NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) ||
                  (target==NVN_TEXTURE_TARGET_2D && layers!=1)) {
                   fprintf(stderr,"[nvn-vulkan] texture view id=%u flags=%u,%u,%u,%u levels=%d,%d layers=%d,%d format=%d/%d target=%d/%d\n",
                       tid,v->set_levels,v->set_layers,v->set_format,v->set_target,v->base_level,v->nlevels,v->min_layer,v->nlayers,
                       v->format,t->source.d.format,v->target,t->source.d.target);
                   die("texture view format/target/layer/range adaptation",tid);
               }
               if(level || levels!=t->source.d.levels || layer || layers!=t->source.L.layers || target!=t->source.d.target) {
                   if(G.nview==LIMIT_WRITES)die("texture view count",G.nview);
                   VkImageViewType view_type=target==NVN_TEXTURE_TARGET_3D?VK_IMAGE_VIEW_TYPE_3D:target==NVN_TEXTURE_TARGET_2D?VK_IMAGE_VIEW_TYPE_2D:target==NVN_TEXTURE_TARGET_2D_ARRAY?VK_IMAGE_VIEW_TYPE_2D_ARRAY:
                       target==NVN_TEXTURE_TARGET_CUBEMAP?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                   VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=selected_image,.viewType=view_type,
                       .format=selected_format,.subresourceRange={aspect(selected_format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,level,levels,layer,layers}};
                   CHECK(vkCreateImageView(G.device,&vi,NULL,&G.temporary_views[G.nview]));ii[i].imageView=G.temporary_views[G.nview++];
               }
           }
           if(b->kind==4 && b->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) { unsigned sid=(unsigned)((b->sampler>=0?G.state.samp[s->h.stage][b->sampler]:handle)>>20)&0xfff;
               if(sid>=LIMIT_OBJECTS || !G.sslot[sid].live){
                   /* Maxwell texel fetch requires no sampler. The translator
                    * still declares a combined descriptor; Vulkan requires a
                    * valid sampler there even though OpImage discards it. */
                   if(!(s->fetch_bindings&(1ull<<i)) || sid){fprintf(stderr,"[nvn-vulkan] unbound sampler shader=%s stage=%u slot=%d sampler=%d handle=%#llx separate=%#llx fetch_bindings=%#llx\n",s->key,s->h.stage,b->slot,b->sampler,(unsigned long long)handle,(unsigned long long)(b->sampler>=0?G.state.samp[s->h.stage][b->sampler]:0),(unsigned long long)s->fetch_bindings);die("unbound sampler descriptor",sid);}
                   if(!G.fetch_sampler){VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=VK_LOD_CLAMP_NONE};CHECK(vkCreateSampler(G.device,&ci,NULL,&G.fetch_sampler));}
                   ii[i].sampler=G.fetch_sampler;
               }else {ii[i].sampler=G.sslot[sid].sampler;
                   if(G.trace_frame&&s->h.stage==1&&!strcmp(s->key,"5c1ca4e7c3e4657c")){
                       samp_par *p=&G.sslot[sid].source;
                       fprintf(stderr,"[nvn-vulkan] TEXTURE_SAMPLER draw=%llu binding=%u slot=%d serial=%llu sampler=%u minf=%u magf=%u lod=%g,%g bias=%g compare=%u,%u view_levels=%d,%d set_levels=%u\n",(unsigned long long)G.draws,b->binding,b->slot,(unsigned long long)t->serial,sid,p->minf,p->magf,p->lod_min,p->lod_max,p->lod_bias,p->cmp_mode,p->cmp_func,slot->view.base_level,slot->view.nlevels,slot->has_view&&slot->view.set_levels);
                   }
               } }
           if(b->kind==6){t->cpu_owned=0;t->version++;}
           w[i].pImageInfo=&ii[i];continue;
       } else die("Vulkan shader resource kind",b->kind);
       w[i].pBufferInfo=&bi[i];
    }
    vkUpdateDescriptorSets(G.device,s->h.nb,w,0,NULL);
    if(G.trace_frame&&G.trace_ui&&s->h.stage==1&&!strcmp(s->key,"62ddcb3faa41a92a")){
        for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==2||s->b[i].kind==9){uint64_t bytes=bi[i].range;
            if(bytes%4)die("UI effective uniform capture alignment",bytes);
            VBuffer *capture=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);all_barrier();VkBufferCopy copy={bi[i].offset,0,bytes};
            vkCmdCopyBuffer(G.command,bi[i].buffer,capture->b,1,&copy);char name[160];snprintf(name,sizeof(name),"ui-effective-%04llu-%llu-ps-ubo%d",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,s->b[i].slot);trace_capture_queue(capture,bytes,name);
        }
    }
    return set;
}
static VShader *program_stage_optional_impl(int stage) {
    const prog_t *p=G.state.program;if(!p)die("draw/dispatch without program",stage);
    for(uint32_t i=0;i<p->nstage;i++) { const void *control=p->sd[i].control;if(!control)die("shader data control",i);
       if(shader_entry(control,stage))return shader_get(control,stage); }
    return NULL;
}
static VShader *program_stage_optional(int stage) {
    uint64_t started=dm_android_nvn_clock_ns();VShader *s=program_stage_optional_impl(stage);
    G.profile_lookup_ns+=dm_android_nvn_clock_ns()-started;return s;
}
static VShader *program_stage(int stage) {
    VShader *s=program_stage_optional(stage);if(!s)die("requested program stage missing",stage);return s;
}

static VkBlendFactor blend_factor(unsigned f) {
    switch(f) {
    case NVN_BLEND_FUNC_ZERO:return VK_BLEND_FACTOR_ZERO;case NVN_BLEND_FUNC_ONE:return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR:return VK_BLEND_FACTOR_SRC_COLOR;case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA:return VK_BLEND_FACTOR_SRC_ALPHA;case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA:return VK_BLEND_FACTOR_DST_ALPHA;case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR:return VK_BLEND_FACTOR_DST_COLOR;case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_COLOR:return VK_BLEND_FACTOR_CONSTANT_COLOR;case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA:return VK_BLEND_FACTOR_CONSTANT_ALPHA;case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:die("blend factor",f); }
}
static VkBlendOp blend_op(unsigned op) {
    switch(op) {case NVN_BLEND_EQUATION_ADD:return VK_BLEND_OP_ADD;case NVN_BLEND_EQUATION_SUB:return VK_BLEND_OP_SUBTRACT;
       case NVN_BLEND_EQUATION_REVERSE_SUB:return VK_BLEND_OP_REVERSE_SUBTRACT;case NVN_BLEND_EQUATION_MIN:return VK_BLEND_OP_MIN;
       case NVN_BLEND_EQUATION_MAX:return VK_BLEND_OP_MAX;default:die("blend equation",op); }
}
static VkPrimitiveTopology topology(int p) {
    switch(p) {case NVN_DRAW_PRIMITIVE_POINTS:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;case NVN_DRAW_PRIMITIVE_LINE_STRIP:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;default:die("draw primitive",p); }
}
static VkImageSubresourceRange attachment_range(const tex_t *source,const view_t *v) {
    unsigned level=v&&v->set_levels?v->base_level:0,layer=v&&v->set_layers?v->min_layer:0;
    if(level>=source->d.levels || layer>=source->L.layers || (v&&v->set_levels&&v->nlevels<1) ||
       (v&&v->set_layers&&v->nlayers!=1) || (v&&v->set_format&&v->format!=source->d.format) || source->d.target==NVN_TEXTURE_TARGET_3D)
        die("attachment view format/layer/mip",level);
    return (VkImageSubresourceRange){aspect(format(source->d.format)),level,1,layer,1};
}
static VkRenderPass render_begin(void) {
    pass_end();p_set_rt *rt=&G.state.rt;if(rt->n<0 || rt->n>8 || (!rt->n&&!rt->depth))die("render targets",rt->n);
    VkAttachmentDescription attachments[9];VkAttachmentReference color[8],depth;VkImageView views[9];uint32_t n=0,width=0,height=0;
    memset(attachments,0,sizeof(attachments));
    for(int i=0;i<rt->n;i++)color[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    for(int i=0;i<rt->n+(rt->depth?1:0);i++) {
        int is_depth=i==rt->n;const tex_t *source=is_depth?rt->depth:rt->color[i];
        /* NVN color slots may be unbound. Pack only real image attachments,
         * while keeping subpass references at their original output locations. */
        if(!source)continue;
        const view_t *v=is_depth?(rt->has_dview?&rt->dview:NULL):(rt->has_cview[i]?&rt->cview[i]:NULL);
        VkImageSubresourceRange range=attachment_range(source,v);
        VTexture *t=texture_get(source,0);uint32_t tw=t->width>>range.baseMipLevel,th=t->height>>range.baseMipLevel;if(!tw)tw=1;if(!th)th=1;
        if(G.present_count<1 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] RT_ATTACHMENT index=%d depth=%d serial=%llu extent=%u,%u base_extent=%u,%u level=%u layer=%u count=%d nvn_format=%u gpu_format=%u\n",i,is_depth,(unsigned long long)source->serial,tw,th,source->d.width,source->d.height,range.baseMipLevel,range.baseArrayLayer,rt->n,source->d.format,t->format);
        /* NVN permits differently sized attachments. Vulkan framebuffer extent
         * must fit every attachment; its render area is their intersection. */
        if(!width){width=tw;height=th;}else {if(tw<width)width=tw;if(th<height)height=th;}
        t->cpu_owned=0;t->version++;
        VkImageLayout attachment_layout=is_depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if(is_depth&&G.depth_sampled&&depth_read_only())attachment_layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        else if(G.feedback_flags&(is_depth?VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT:VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT))attachment_layout=VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        texture_transition(t,attachment_layout);
        VkImageLayout layout=t->layout;
        attachments[n]=(VkAttachmentDescription){.format=t->format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,
           .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
           .initialLayout=layout,.finalLayout=layout};
        if(G.nview==LIMIT_WRITES)die("attachment view count",G.nview);
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=t->format,.subresourceRange=range};
        CHECK(vkCreateImageView(G.device,&view,NULL,&G.temporary_views[G.nview]));views[n]=G.temporary_views[G.nview++];
        if(is_depth)depth=(VkAttachmentReference){n,layout};else color[i]=(VkAttachmentReference){n,layout};n++;
    }
    if(!n)die("render targets have no bound attachment",rt->n);
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=rt->n,.pColorAttachments=color,
        .pDepthStencilAttachment=rt->depth?&depth:NULL};
    VkSubpassDependency deps[3]={
      {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       .dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
      {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
      {.srcSubpass=0,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
       .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
       .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT|VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT}};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=n,.pAttachments=attachments,
       .subpassCount=1,.pSubpasses=&sub,.dependencyCount=G.feedback_flags?3u:2u,.pDependencies=deps};
    if(G.npass==LIMIT_WRITES)die("render pass limit",G.npass);int index=G.npass++;
    CHECK(vkCreateRenderPass(G.device,&ri,NULL,&G.passes[index]));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=G.passes[index],.attachmentCount=n,
       .pAttachments=views,.width=width,.height=height,.layers=1};CHECK(vkCreateFramebuffer(G.device,&fi,NULL,&G.framebuffers[index]));
    VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=G.passes[index],.framebuffer=G.framebuffers[index],
       .renderArea={{0,0},{width,height}}};vkCmdBeginRenderPass(G.command,&begin,VK_SUBPASS_CONTENTS_INLINE);G.in_pass=1;G.pass_count++;
    return G.passes[index];
}
static void trace_mesh_range(const char *name,uint64_t address,uint64_t bytes) {
    const char *root=getenv("DM_FRAME_DIR");if(!root||!bytes)return;
    if(bytes>16*1024*1024||!pool_find(address,bytes,NULL))die("mesh diagnostic range",bytes);
    char path[1400];snprintf(path,sizeof(path),"%s/mesh-%04llu-%llu-%s.bin",root,(unsigned long long)G.present_count+1,(unsigned long long)G.draws,name);
    FILE *out=fopen(path,"wb");if(!out)die("mesh diagnostic output",0);
    if(fwrite((const void *)(uintptr_t)address,1,bytes,out)!=bytes)die("mesh diagnostic write",bytes);fclose(out);
    fprintf(stderr,"[nvn-vulkan] MESH_GUEST_RANGE draw=%llu name=%s bytes=%llu path=%s\n",(unsigned long long)G.draws,name,(unsigned long long)bytes,path);
    uint64_t prefix=address&3ULL,span=(prefix+bytes+3)&~3ULL;
    uint64_t offset=0;VPool *pool=pool_read(address-prefix,span,&offset);VBuffer *capture=temporary(span,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    all_barrier();VkBufferCopy copy={offset,0,span};vkCmdCopyBuffer(G.command,pool->v.b,capture->b,1,&copy);
    char gpu_name[160];snprintf(gpu_name,sizeof(gpu_name),"mesh-%04llu-%llu-%s-gpu",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,name);trace_capture_queue(capture,bytes,gpu_name);
    G.trace_captures[G.ntrace_captures-1].offset=prefix;
}
static void draw(const p_draw *a,const p_draw_idx *idx) {
    if(!a&&!idx)die("draw parameters",0);
    int upper_left=atomic_load(&G.window_origin)==NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
    if(G.state.ms.samples>1 || G.state.ms.a2c || G.state.ms.cov_mod || G.state.ms.sl_en)die("multisample semantics",G.state.ms.samples);
    if(G.state.color.logic_op!=NVN_LOGIC_OP_COPY || G.state.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS)die("logic/alpha test adaptation",G.state.color.logic_op);
    if(G.state.polygon.mode!=NVN_POLYGON_MODE_FILL || G.state.polygon.offset_en)die("polygon mode/offset",G.state.polygon.mode);
    /* A zero-color pass may still run fragment discard, depth export or
     * storage side effects. Only a program with no fragment stage omits it. */
    VShader *vs=program_stage(0),*ps=program_stage_optional(1);
    if(G.trace_frame&&!G.trace_ui&&idx&&idx->count>=100&&G.trace_meshes<4&&!strcmp(vs->key,"54f730a6c0efc118")){
        G.trace_meshes++;for(unsigned i=0;i<16;i++)if(G.state.vbuf[i].size){char name[32];snprintf(name,sizeof(name),"vbuf%u",i);trace_mesh_range(name,G.state.vbuf[i].address,G.state.vbuf[i].size);}
        for(unsigned i=0;i<vs->h.nb;i++)if(vs->b[i].kind==2||vs->b[i].kind==9){int slot=vs->b[i].slot;char name[32];snprintf(name,sizeof(name),"ubo%d",slot);trace_mesh_range(name,G.state.uniform[0][slot].addr,G.state.uniform[0][slot].size);}
        trace_mesh_range("indices",idx->ibuf,(uint64_t)idx->count*(idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?2:4));
        fprintf(stderr,"[nvn-vulkan] MESH_INDEX draw=%llu type=%u base_vertex=%d base_instance=%u count=%d\n",(unsigned long long)G.draws,idx->itype,idx->base_vertex,idx->base_instance,idx->count);
    }
    if(G.trace_frame&&G.trace_ui&&ps&&!strcmp(ps->key,"62ddcb3faa41a92a")){
        for(unsigned stage=0;stage<2;stage++){VShader *s=stage?ps:vs;
            for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==2||s->b[i].kind==9){unsigned slot=s->b[i].slot;char name[48];snprintf(name,sizeof(name),"ui-%s-ubo%u",stage?"ps":"vs",slot);Range r=G.state.uniform[stage][slot];trace_mesh_range(name,r.addr,r.size);}}
        NVNbufferRange v=G.state.vbuf[0];trace_mesh_range("ui-vbuf0",v.address,v.size);
        blend_st b=G.state.blend[0];fprintf(stderr,"[nvn-vulkan] UI_BLEND draw=%llu enable=%u src=%u,%u dst=%u,%u equation=%u,%u channels=%#x\n",(unsigned long long)G.draws,G.state.color.blend_en,b.src_rgb,b.src_a,b.dst_rgb,b.dst_a,b.eq_rgb,b.eq_a,G.state.mask.mask);
    }
    if(G.state.rt.n&&!ps)die("color draw program fragment stage missing",1);
    if(G.draws<9 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_TRACE draw=%llu vs=%s ps=%s count=%d instances=%d rt=%llu viewport=%g,%g,%g,%g scissor=%d,%d,%u,%u cull=%u front=%u depth=%u,%u,%u blend=%u mask=%#x\n",
        (unsigned long long)G.draws,vs->key,ps?ps->key:"none",a?a->count:idx->count,a?1:idx->instances,
        (unsigned long long)(G.state.rt.n&&G.state.rt.color[0]?G.state.rt.color[0]->serial:0),G.state.viewport.x,G.state.viewport.y,G.state.viewport.width,G.state.viewport.height,
        G.state.scissor.offset.x,G.state.scissor.offset.y,G.state.scissor.extent.width,G.state.scissor.extent.height,
        G.state.polygon.cull,G.state.polygon.front,G.state.ds.dt,G.state.ds.dw,G.state.ds.df,G.state.color.blend_en,G.state.mask.mask);
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_DEPTH_STATE draw=%llu range=%g,%g stencil=%u func=%u,%u masks=%#x,%#x write=%#x,%#x ref=%#x,%#x ops=%u,%u,%u/%u,%u,%u\n",
        (unsigned long long)G.draws,G.state.viewport.minDepth,G.state.viewport.maxDepth,G.state.ds.st,G.state.ds.sf_f,G.state.ds.sf_b,
        G.state.stencil_compare[0],G.state.stencil_compare[1],G.state.stencil_write[0],G.state.stencil_write[1],G.state.stencil_ref[0],G.state.stencil_ref[1],
        G.state.ds.fail_f,G.state.ds.zfail_f,G.state.ds.zpass_f,G.state.ds.fail_b,G.state.ds.zfail_b,G.state.ds.zpass_b);
    G.feedback_flags=0;G.depth_sampled=0;G.nimage_uses=0;collect_image_uses(vs);collect_image_uses(ps);
    uint64_t bind_started=dm_android_nvn_clock_ns();
    prepare_comparison_images(vs);prepare_comparison_images(ps);
    VkDescriptorSet descriptor_sets[2]={bind_shader(vs),ps?bind_shader(ps):VK_NULL_HANDLE};
    G.profile_bind_ns+=dm_android_nvn_clock_ns()-bind_started;
    VkDescriptorSetLayout set_layouts[2]={vs->layout,ps?ps->layout:VK_NULL_HANDLE};
    uint64_t render_started=dm_android_nvn_clock_ns();VkRenderPass renderpass=render_begin();G.profile_render_ns+=dm_android_nvn_clock_ns()-render_started;
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=ps?2u:1u,.pSetLayouts=set_layouts};
    if(G.npipeline==LIMIT_WRITES)die("pipeline count limit",G.npipeline);int pi=G.npipeline++;
    CHECK(vkCreatePipelineLayout(G.device,&li,NULL,&G.layouts[pi]));
    VkVertexInputAttributeDescription attributes[32];VkVertexInputBindingDescription bindings[16];uint32_t nbindings=0;uint8_t used[16]={0};
    for(uint32_t i=0;i<vs->h.na;i++) { unsigned location=vs->attr[i][0];if(location>=16)die("vertex attribute count",location);
        va_st v=G.state.attributes[location];if(v.stream>=16)die("vertex stream",v.stream);
        if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] VERTEX_INPUT draw=%llu location=%u shader_type=%u stream=%u format=%u offset=%u stride=%u divisor=%u address=%#llx size=%llu\n",(unsigned long long)G.draws,location,vs->attr[i][1],v.stream,v.fmt,v.offset,G.state.streams[v.stream].stride,G.state.streams[v.stream].divisor,(unsigned long long)G.state.vbuf[v.stream].address,(unsigned long long)G.state.vbuf[v.stream].size);
        attributes[i]=(VkVertexInputAttributeDescription){location,v.stream,format(v.fmt),v.offset};used[v.stream]=1; }
    for(unsigned i=0;i<16;i++)if(used[i]) { vs_st v=G.state.streams[i];if(v.divisor>1)die("vertex divisor",v.divisor);
        bindings[nbindings++]=(VkVertexInputBindingDescription){i,v.stride,v.divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};
        NVNbufferRange r=G.state.vbuf[i];if(!r.address || !r.size)die("unbound vertex stream",i);uint64_t offset=0;VPool *p=pool_read(r.address,r.size,&offset);
        vkCmdBindVertexBuffers(G.command,i,1,&p->v.b,&offset); }
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=nbindings,
        .pVertexBindingDescriptions=bindings,.vertexAttributeDescriptionCount=vs->h.na,.pVertexAttributeDescriptions=attributes};
    VkPipelineInputAssemblyStateCreateInfo ai={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=topology(a?a->prim:idx->prim)};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,
       .cullMode=G.state.polygon.cull==NVN_FACE_FRONT?VK_CULL_MODE_FRONT_BIT:G.state.polygon.cull==NVN_FACE_BACK?VK_CULL_MODE_BACK_BIT:
          G.state.polygon.cull==NVN_FACE_FRONT_AND_BACK?VK_CULL_MODE_FRONT_AND_BACK:VK_CULL_MODE_NONE,
       // An upper-left NVN window retains NDC y upwards; a negative Vulkan
       // viewport restores that mapping. Facing follows the same inversion.
       .frontFace=(G.state.polygon.front==NVN_FRONT_FACE_CCW)==upper_left?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=G.state.ds.dt,
        .depthWriteEnable=G.state.ds.dw,.depthCompareOp=(VkCompareOp)(G.state.ds.df-1),.stencilTestEnable=G.state.ds.st};
    ds.front=(VkStencilOpState){.failOp=(VkStencilOp)(G.state.ds.fail_f-1),.passOp=(VkStencilOp)(G.state.ds.zpass_f-1),
       .depthFailOp=(VkStencilOp)(G.state.ds.zfail_f-1),.compareOp=(VkCompareOp)(G.state.ds.sf_f-1),
       .compareMask=G.state.stencil_compare[0],.writeMask=G.state.stencil_write[0],.reference=G.state.stencil_ref[0]};
    ds.back=(VkStencilOpState){.failOp=(VkStencilOp)(G.state.ds.fail_b-1),.passOp=(VkStencilOp)(G.state.ds.zpass_b-1),
       .depthFailOp=(VkStencilOp)(G.state.ds.zfail_b-1),.compareOp=(VkCompareOp)(G.state.ds.sf_b-1),
       .compareMask=G.state.stencil_compare[1],.writeMask=G.state.stencil_write[1],.reference=G.state.stencil_ref[1]};
    VkPipelineColorBlendAttachmentState blends[8];memset(blends,0,sizeof(blends));
    for(int i=0;i<G.state.rt.n;i++) {blend_st b=G.state.blend[i];
        if(!G.state.rt.color[i])continue;
        blends[i].colorWriteMask=(G.state.mask.mask>>(4*i))&15;
        if(G.state.color.blend_en&(1u<<i)) {blends[i].blendEnable=1;blends[i].srcColorBlendFactor=blend_factor(b.src_rgb);
           blends[i].dstColorBlendFactor=blend_factor(b.dst_rgb);blends[i].srcAlphaBlendFactor=blend_factor(b.src_a);
           blends[i].dstAlphaBlendFactor=blend_factor(b.dst_a);blends[i].colorBlendOp=blend_op(b.eq_rgb);blends[i].alphaBlendOp=blend_op(b.eq_a);} }
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=G.state.rt.n,.pAttachments=blends};
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=3,.pDynamicStates=dynamic};
    VkPipelineShaderStageCreateInfo stages[2]={
       {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs->module,.pName="main"},
       {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=ps?ps->module:VK_NULL_HANDLE,.pName="main"}};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.flags=G.feedback_flags,.stageCount=ps?2u:1u,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ai,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,
        .pDepthStencilState=&ds,.pColorBlendState=&cb,.pDynamicState=&dy,.layout=G.layouts[pi],.renderPass=renderpass};
    uint64_t pipeline_start=dm_android_nvn_clock_ns();CHECK(vkCreateGraphicsPipelines(G.device,G.pipeline_cache,1,&gp,NULL,&G.pipelines[pi]));G.profile_pipeline_ns+=dm_android_nvn_clock_ns()-pipeline_start;
    vkCmdBindPipeline(G.command,VK_PIPELINE_BIND_POINT_GRAPHICS,G.pipelines[pi]);
    vkCmdBindDescriptorSets(G.command,VK_PIPELINE_BIND_POINT_GRAPHICS,G.layouts[pi],0,ps?2:1,descriptor_sets,0,NULL);
    if(G.state.viewport.width<=0 || G.state.viewport.height<=0)die("viewport bounds",0);
    VkViewport viewport=G.state.viewport;if(upper_left){viewport.y+=viewport.height;viewport.height=-viewport.height;}
    vkCmdSetViewport(G.command,0,1,&viewport);vkCmdSetScissor(G.command,0,1,&G.state.scissor);
    vkCmdSetBlendConstants(G.command,G.state.blend_color);
    if(!G.precise_occlusion || G.nocc==LIMIT_WRITES)die("precise occlusion query support/count",G.nocc);
    uint32_t query=G.nocc++;vkCmdBeginQuery(G.command,G.occlusion,query,VK_QUERY_CONTROL_PRECISE_BIT);
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_QUERY draw=%llu query=%u\n",(unsigned long long)G.draws,query);
    if(a) {if(a->count<0 || a->first<0)die("draw arrays bounds",a->count);vkCmdDraw(G.command,a->count,1,a->first,0);}
    else { if(idx->count<0 || idx->instances<0 || (idx->itype!=NVN_INDEX_TYPE_UNSIGNED_SHORT && idx->itype!=NVN_INDEX_TYPE_UNSIGNED_INT))die("index type/bounds",idx->itype);
        uint64_t offset=0,size=(uint64_t)idx->count*(idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?2:4);VPool *p=pool_read(idx->ibuf,size,&offset);
        vkCmdBindIndexBuffer(G.command,p->v.b,offset,idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(G.command,idx->count,idx->instances,0,idx->base_vertex,idx->base_instance); }
    vkCmdEndQuery(G.command,G.occlusion,query);counter_op(1,query,0);
    if(G.trace_frame&&G.trace_ui&&ps&&!strcmp(ps->key,"62ddcb3faa41a92a")&&G.state.rt.n==1){
        VTexture *t=texture_get(G.state.rt.color[0],0);if(t->bpp!=4||aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)die("UI result capture format",t->format);
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);uint64_t bytes=(uint64_t)t->width*t->height*4;
        VBuffer *capture=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
        vkCmdCopyImageToBuffer(G.command,t->image,t->layout,capture->b,1,&copy);char name[160];snprintf(name,sizeof(name),"ui-result-%04llu-%llu-%ux%u",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,t->width,t->height);trace_capture_queue(capture,bytes,name);
    }
    G.draws++;aligned_writeback();
}
static void dispatch(const p_dispatch *p) {
    pass_end();if(p->x<0 || p->y<0 || p->z<0)die("dispatch dimensions",p->x);VShader *s=program_stage(5);
    G.nimage_uses=0;collect_image_uses(s);prepare_comparison_images(s);
    VkDescriptorSet set=bind_shader(s);VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&s->layout};if(G.npipeline==LIMIT_WRITES)die("compute pipeline limit",0);int i=G.npipeline++;
    CHECK(vkCreatePipelineLayout(G.device,&li,NULL,&G.layouts[i]));
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=s->module,.pName="main"},.layout=G.layouts[i]};
    uint64_t pipeline_start=dm_android_nvn_clock_ns();CHECK(vkCreateComputePipelines(G.device,G.pipeline_cache,1,&ci,NULL,&G.pipelines[i]));G.profile_pipeline_ns+=dm_android_nvn_clock_ns()-pipeline_start;
    vkCmdBindPipeline(G.command,VK_PIPELINE_BIND_POINT_COMPUTE,G.pipelines[i]);
    vkCmdBindDescriptorSets(G.command,VK_PIPELINE_BIND_POINT_COMPUTE,G.layouts[i],0,1,&set,0,NULL);vkCmdDispatch(G.command,p->x,p->y,p->z);
    aligned_writeback();
}

static void copy_b2t(const p_copy_b2t *p) {
    if(p->flags || p->r.zoffset<0 || p->r.depth<1 || p->r.xoffset<0 || p->r.yoffset<0 || p->r.width<0 || p->r.height<0)
       die("buffer-to-texture flags/view/region",p->flags);
    unsigned level=p->has_view&&p->view.set_levels?(unsigned)p->view.base_level:0;
    int volume=p->tex->d.target==NVN_TEXTURE_TARGET_3D;
    unsigned layer=(p->has_view&&p->view.set_layers?(unsigned)p->view.min_layer:0)+(unsigned)p->r.zoffset;
    unsigned available=volume?(p->tex->d.depth>>level):p->tex->L.layers;if(!available)available=1;
    if(layer>=available || (unsigned)p->r.depth>available-layer) {
        fprintf(stderr,"[nvn-vulkan] b2t target=%d extent=%u,%u,%u layers=%u region=%d,%d,%d,%d,%d,%d view_flags=%u,%u levels=%d,%d layers=%d,%d\n",
            p->tex->d.target,p->tex->d.width,p->tex->d.height,p->tex->d.depth,p->tex->L.layers,
            p->r.xoffset,p->r.yoffset,p->r.zoffset,p->r.width,p->r.height,p->r.depth,p->view.set_levels,p->view.set_layers,
            p->view.base_level,p->view.nlevels,p->view.min_layer,p->view.nlayers);
        die("buffer-to-texture layer range",layer);
    }
    if(level>=p->tex->d.levels || (p->has_view &&
       ((p->view.set_format&&p->view.format!=p->tex->d.format) ||
        (p->view.set_target&&p->view.target!=p->tex->d.target))))die("buffer-to-texture view range/format/target",level);
    uint32_t pf;int bb;int bc=nvn_bc_unpacked(p->tex->d.format,&pf,&bb);
    /* A compressed texture can already contain streamed CPU mip data when a
     * transfer updates only its tail. Preserve the other levels from its NVN
     * backing on first materialization, just as copy_t2t does. */
    VTexture *t=texture_get(p->tex,bc);uint64_t row=G.state.copy_stride?G.state.copy_stride:(uint64_t)p->r.width*t->bpp;
    unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
    if(bc) {
        int bw,bh;int block=nvn_fmt_bpb(p->tex->d.format,&bw,&bh);
        row=G.state.copy_stride?G.state.copy_stride:(uint64_t)((p->r.width+bw-1)/bw)*block;
        uint64_t rows=(p->r.height+bh-1)/bh,size=row*rows*p->r.depth;
        if(!block || row<(uint64_t)((p->r.width+bw-1)/bw)*block ||
           (uint64_t)p->r.xoffset+p->r.width>width || (uint64_t)p->r.yoffset+p->r.height>height)
            die("compressed buffer-to-texture range/stride",row);
        for(int i=0;i<G.nwrites;++i)if(overlap(p->src,size,G.writes[i].addr,G.writes[i].size))
            die("compressed decode requires completed GPU source write",p->src);
        if(!pool_find(p->src,size,NULL))die("compressed decode source range",p->src);
        /* No queued GPU write overlaps this range (checked above), so the
         * compressed source bytes are authoritative in guest CPU memory. */
        const uint8_t *compressed=(const void *)(uintptr_t)p->src;
        uint64_t image_bytes=(uint64_t)p->r.width*p->r.height*t->bpp;
        VBuffer *staging=temporary(image_bytes*p->r.depth,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for(int z=0;z<p->r.depth;++z)if(nvn_bc_decode(p->tex->d.format,compressed+z*row*rows,row,p->r.width,p->r.height,staging->map+z*image_bytes,(size_t)p->r.width*t->bpp))
            die("buffer-to-texture BC decode",p->tex->d.format);
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c={.imageSubresource={aspect(t->format),level,volume?0:layer,volume?1:p->r.depth},
            .imageOffset={p->r.xoffset,p->r.yoffset,volume?p->r.zoffset:0},.imageExtent={p->r.width,p->r.height,volume?p->r.depth:1}};
        vkCmdCopyBufferToImage(G.command,staging->b,t->image,t->layout,1,&c);t->cpu_owned=0;t->version++;return;
    }
    if(row%t->bpp || row<(uint64_t)p->r.width*t->bpp || (uint64_t)p->r.xoffset+p->r.width>width || (uint64_t)p->r.yoffset+p->r.height>height)
       die("buffer-to-texture extent/stride",row);
    uint64_t offset=0;VPool *src=pool_read(p->src,row*p->r.height*p->r.depth,&offset);all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c={.bufferOffset=offset,.bufferRowLength=(uint32_t)(row/t->bpp),.imageSubresource={aspect(t->format),level,volume?0:layer,volume?1:p->r.depth},
       .imageOffset={p->r.xoffset,p->r.yoffset,volume?p->r.zoffset:0},.imageExtent={p->r.width,p->r.height,volume?p->r.depth:1}};
    vkCmdCopyBufferToImage(G.command,src->v.b,t->image,t->layout,1,&c);t->cpu_owned=0;t->version++;
}
static void clear_color(const p_clear_color *p) {
    if(p->mask!=15 || p->index<0 || p->index>=G.state.rt.n)die("clear-color target/mask/view",p->mask);
    VTexture *t=texture_get(G.state.rt.color[p->index],0);VkRect2D s=G.state.scissor;
    VkImageSubresourceRange range=attachment_range(&t->source,G.state.rt.has_cview[p->index]?&G.state.rt.cview[p->index]:NULL);
    unsigned w=t->width>>range.baseMipLevel,h=t->height>>range.baseMipLevel;if(!w)w=1;if(!h)h=1;
    if(s.offset.x || s.offset.y || (s.extent.width && s.extent.width!=w) || (s.extent.height && s.extent.height!=h)) {
        if(s.offset.x<0 || s.offset.y<0)die("clear scissor negative",p->index);
        if((unsigned)s.offset.x>=w || (unsigned)s.offset.y>=h)return;
        if(s.extent.width>w-s.offset.x)s.extent.width=w-s.offset.x;if(s.extent.height>h-s.offset.y)s.extent.height=h-s.offset.y;
        render_begin();VkClearAttachment a={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=p->index};memcpy(a.clearValue.color.float32,p->rgba,16);
        VkClearRect r={s,0,1};vkCmdClearAttachments(G.command,1,&a,1,&r);t->cpu_owned=0;t->version++;return;
    }
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearColorValue color;memcpy(color.float32,p->rgba,sizeof(p->rgba));
    vkCmdClearColorImage(G.command,t->image,t->layout,&color,1,&range);t->cpu_owned=0;t->version++;
}
static void clear_ds(const p_clear_ds *p) {
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] CLEAR_DS depth=%g depth_mask=%#x stencil=%#x stencil_mask=%#x serial=%llu\n",p->depth,p->depth_mask,p->stencil,p->stencil_mask,
        (unsigned long long)(G.state.rt.depth?G.state.rt.depth->serial:0));
    if(!G.state.rt.depth || (p->stencil_mask && p->stencil_mask!=255 && p->stencil_mask!=-1))die("depth/stencil clear target/mask",p->stencil_mask);
    VTexture *t=texture_get(G.state.rt.depth,0);VkImageAspectFlags flags=0;
    if(p->depth_mask)flags|=VK_IMAGE_ASPECT_DEPTH_BIT;if(p->stencil_mask)flags|=VK_IMAGE_ASPECT_STENCIL_BIT;
    flags&=aspect(t->format);if(!flags)return;
    VkImageSubresourceRange range=attachment_range(&t->source,G.state.rt.has_dview?&G.state.rt.dview:NULL);range.aspectMask=flags;
    unsigned w=t->width>>range.baseMipLevel,h=t->height>>range.baseMipLevel;if(!w)w=1;if(!h)h=1;
    VkRect2D s=G.state.scissor;
    if(s.offset.x || s.offset.y || (s.extent.width && s.extent.width!=w) || (s.extent.height && s.extent.height!=h)){
        if(s.offset.x<0 || s.offset.y<0)die("depth clear negative scissor",0);
        if((unsigned)s.offset.x>=w || (unsigned)s.offset.y>=h)return;
        if(s.extent.width>w-s.offset.x)s.extent.width=w-s.offset.x;if(s.extent.height>h-s.offset.y)s.extent.height=h-s.offset.y;
        render_begin();VkClearAttachment a={.aspectMask=flags,.clearValue.depthStencil={p->depth,(uint32_t)p->stencil}};
        VkClearRect r={s,0,1};vkCmdClearAttachments(G.command,1,&a,1,&r);t->cpu_owned=0;t->version++;return;
    }
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearDepthStencilValue value={p->depth,(uint32_t)p->stencil};
    vkCmdClearDepthStencilImage(G.command,t->image,t->layout,&value,1,&range);t->cpu_owned=0;t->version++;
}
static void clear_tex(const p_clear_tex *p) {
    const view_t *v=p->has_view?&p->view:NULL;
    unsigned level=v&&v->set_levels?v->base_level:0,layer=v&&v->set_layers?v->min_layer:0;
    int nvn=v&&v->set_format?v->format:p->tex->d.format;
    VTexture *t=texture_get(p->tex,0);
    fprintf(stderr,"[nvn-vulkan] CLEAR_TEX serial=%llu basefmt=%d viewfmt=%d region=%d,%d,%d,%d,%d,%d uint=%u\n",(unsigned long long)p->tex->serial,p->tex->d.format,nvn,p->r.xoffset,p->r.yoffset,p->r.zoffset,p->r.width,p->r.height,p->r.depth,p->is_uint);
    unsigned w=t->width>>level,h=t->height>>level;if(!w)w=1;if(!h)h=1;
    int volume=p->tex->d.target==NVN_TEXTURE_TARGET_3D;
    unsigned z=volume?(t->depth>>level):p->tex->L.layers;if(!z)z=1;
    int bw,bh,bpp=nvn_fmt_bpb(nvn,&bw,&bh);
    if(p->mask!=15 || level>=p->tex->d.levels || p->r.xoffset<0 || p->r.yoffset<0 || p->r.zoffset<0 ||
       p->r.width<0 || p->r.height<0 || p->r.depth<1 || bw!=1 || bh!=1 || bpp>16 || bpp!=(int)t->bpp ||
       (v&&v->set_target&&v->target!=p->tex->d.target) || (uint64_t)p->r.xoffset+p->r.width>w ||
       (uint64_t)p->r.yoffset+p->r.height>h || (uint64_t)(volume?0:layer)+p->r.zoffset+p->r.depth>z)
        die("texture clear view/region/mask",nvn);
    if(!p->r.width || !p->r.height)return;
    uint8_t pixel[16]={0};int channels=0,unorm=0,integer=0,floating=0;
    switch(nvn) {
        case NVN_FORMAT_R8:channels=1;unorm=1;break;case NVN_FORMAT_RG8:channels=2;unorm=1;break;case NVN_FORMAT_RGBA8:case NVN_FORMAT_RGBA8_SRGB:channels=4;unorm=1;break;
        case NVN_FORMAT_R8UI:channels=1;integer=1;break;case NVN_FORMAT_RG8UI:channels=2;integer=1;break;case NVN_FORMAT_RGBA8UI:channels=4;integer=1;break;
        case NVN_FORMAT_R32UI:channels=1;integer=4;break;case NVN_FORMAT_RG32UI:channels=2;integer=4;break;case NVN_FORMAT_RGBA32UI:channels=4;integer=4;break;
        case NVN_FORMAT_R32F:channels=1;floating=1;break;case NVN_FORMAT_RG32F:channels=2;floating=1;break;case NVN_FORMAT_RGBA32F:channels=4;floating=1;break;
        default:die("texture clear value format",nvn);
    }
    for(int i=0;i<channels;i++) {
        double value=p->is_uint?(double)p->c.u[i]:(double)p->c.f[i];
        if(unorm) {
            value=fmax(0,fmin(1,value));
            if(nvn==NVN_FORMAT_RGBA8_SRGB && i<3)value=value<=0.0031308?12.92*value:1.055*pow(value,1.0/2.4)-0.055;
            pixel[i]=(uint8_t)floor(value*255+0.5);
        }else if(integer==1)pixel[i]=(uint8_t)fmax(0,fmin(255,value));
        else if(integer==4) {uint32_t u=p->is_uint?p->c.u[i]:(uint32_t)fmax(0,fmin(4294967295.0,value));memcpy(pixel+i*4,&u,4);}
        else if(floating) {float f=(float)value;memcpy(pixel+i*4,&f,4);}
    }
    uint64_t bytes=(uint64_t)p->r.width*p->r.height*p->r.depth*bpp;
    VBuffer *staging=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for(uint64_t i=0;i<bytes;i+=bpp)memcpy(staging->map+i,pixel,bpp);
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c={.imageSubresource={aspect(t->format),level,volume?0:layer+p->r.zoffset,volume?1:p->r.depth},
        .imageOffset={p->r.xoffset,p->r.yoffset,volume?p->r.zoffset:0},.imageExtent={p->r.width,p->r.height,volume?p->r.depth:1}};
    vkCmdCopyBufferToImage(G.command,staging->b,t->image,t->layout,1,&c);
    t->cpu_owned=0;t->version++; /* GPU now owns the image; sampled access must preserve it. */
}
static VkImageSubresourceLayers copy_layers(const tex_t *t,VkFormat gpu_format,const view_t *v,const NVNcopyRegion *r) {
    unsigned level=v&&v->set_levels?v->base_level:0,base=v&&v->set_layers?v->min_layer:0;
    int volume=t->d.target==NVN_TEXTURE_TARGET_3D;
    if(level>=t->d.levels || (v&&v->set_levels&&v->nlevels<1) || (v&&v->set_format&&v->format!=t->d.format) ||
       (v&&v->set_target&&v->target!=t->d.target) || r->xoffset<0 || r->yoffset<0 || r->zoffset<0 || r->width<1 || r->height<1 || r->depth<1)
        die("texture copy view/region",t->serial);
    unsigned w=t->d.width>>level,h=(t->d.target==NVN_TEXTURE_TARGET_1D || t->d.target==NVN_TEXTURE_TARGET_1D_ARRAY)?1:t->d.height>>level;
    unsigned z=volume?t->d.depth>>level:t->L.layers;if(!w)w=1;if(!h)h=1;if(!z)z=1;
    if((uint64_t)r->xoffset+r->width>w || (uint64_t)r->yoffset+r->height>h || (uint64_t)(volume?0:base)+r->zoffset+r->depth>z)
        die("texture copy extent",t->serial);
    /* BC images have already been expanded into the resident GPU format.
     * Their guest format is not a Vulkan image format in this backend. */
    return (VkImageSubresourceLayers){aspect(gpu_format),level,volume?0:base+r->zoffset,volume?1:r->depth};
}
static void copy_t2t(const p_copy_t2t *p) {
    if(p->flags&~(NVN_COPY_FLAGS_ENGINE_2D_BIT|NVN_COPY_FLAGS_LINEAR_FILTER_BIT))die("texture copy flags",p->flags);
    uint32_t decoded_format;int decoded_bpp;
    int bc_destination=nvn_bc_unpacked(p->dst->d.format,&decoded_format,&decoded_bpp);
    /* A partial BC update must retain the other compressed blocks already
     * present in guest storage, even when the destination is first used here. */
    VTexture *s=texture_get(p->src,1),*d=texture_get(p->dst,bc_destination);
    int sv=p->src->d.target==NVN_TEXTURE_TARGET_3D,dv=p->dst->d.target==NVN_TEXTURE_TARGET_3D;
    if(s==d || sv!=dv || s->format!=d->format || p->src->d.format!=p->dst->d.format)die("texture copy alias/dimension/format",p->dst->serial);
    VkImageSubresourceLayers sl=copy_layers(p->src,s->format,p->has_sv?&p->sv:NULL,&p->sr),dl=copy_layers(p->dst,d->format,p->has_dv?&p->dv:NULL,&p->dr);
    fprintf(stderr,"[nvn-vulkan] COPY_T2T src=%llu dst=%llu region=%d,%d,%d -> %d,%d,%d flags=%#x src_level=%u dst_level=%u src_layer=%u dst_layer=%u\n",(unsigned long long)p->src->serial,(unsigned long long)p->dst->serial,p->sr.width,p->sr.height,p->sr.depth,p->dr.width,p->dr.height,p->dr.depth,p->flags,sl.mipLevel,dl.mipLevel,sl.baseArrayLayer,dl.baseArrayLayer);
    texture_transition(s,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);texture_transition(d,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);all_barrier();
    VkOffset3D so={p->sr.xoffset,p->sr.yoffset,sv?p->sr.zoffset:0},doff={p->dr.xoffset,p->dr.yoffset,dv?p->dr.zoffset:0};
    if(p->sr.width==p->dr.width&&p->sr.height==p->dr.height&&p->sr.depth==p->dr.depth) {
        VkImageCopy c={.srcSubresource=sl,.srcOffset=so,.dstSubresource=dl,.dstOffset=doff,.extent={p->sr.width,p->sr.height,sv?p->sr.depth:1}};
        vkCmdCopyImage(G.command,s->image,s->layout,d->image,d->layout,1,&c);
    }else {
        VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(G.physical,s->format,&fp);
        if(aspect(s->format)!=VK_IMAGE_ASPECT_COLOR_BIT || !(p->flags&NVN_COPY_FLAGS_ENGINE_2D_BIT) || (!sv&&p->sr.depth!=p->dr.depth) ||
           !(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))die("texture scaled copy feature/flags",p->flags);
        VkFilter filter=p->flags&NVN_COPY_FLAGS_LINEAR_FILTER_BIT?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        if(filter==VK_FILTER_LINEAR&&!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))die("texture blit linear feature",s->format);
        VkImageBlit c={.srcSubresource=sl,.srcOffsets={so,{so.x+p->sr.width,so.y+p->sr.height,so.z+(sv?p->sr.depth:1)}},
            .dstSubresource=dl,.dstOffsets={doff,{doff.x+p->dr.width,doff.y+p->dr.height,doff.z+(dv?p->dr.depth:1)}}};
        vkCmdBlitImage(G.command,s->image,s->layout,d->image,d->layout,1,&c,filter);
    }
    d->cpu_owned=0;d->version++;all_barrier();
}
static void payload_check(const op_hdr *h,size_t bytes) { if(h->size<8+bytes)die("command payload truncated",h->op); }
static void execute(const uint8_t *p,unsigned depth) {
    if(depth>32)die("nested command recursion",depth);
    for(unsigned guard=0;guard<1000000;guard++) {
        if(!pool_find((uint64_t)(uintptr_t)p,8,NULL))die("command memory outside pools",(long long)(uintptr_t)p);
        op_hdr h;memcpy(&h,p,sizeof(h));if(h.size<8 || h.size%8 || !pool_find((uint64_t)(uintptr_t)p,h.size,NULL))die("command record size",h.size);
        const uint8_t *a=p+8;
        switch(h.op) {
        case OP_END:return;
        case OP_BEGIN:break; /* Structural record; GPU binding state persists across command handles. */
        case OP_JUMP:payload_check(&h,8);memcpy(&p,a,8);continue;
        case OP_CALL:{const uint8_t *next;payload_check(&h,8);memcpy(&next,a,8);execute(next,depth+1);break;}
        case OP_SIGNAL_EVENT:{Event e;payload_check(&h,16);memcpy(&e.addr,a,8);memcpy(&e.mode,a+8,4);memcpy(&e.value,a+12,4);mtl_sub_event(e.addr,e.mode,e.value);break;}
        case OP_REPORT_COUNTER:{uint64_t type,address;payload_check(&h,16);memcpy(&type,a,8);memcpy(&address,a+8,8);mtl_sub_counter(address,type);break;}
        case OP_RESET_COUNTER:{uint64_t type;payload_check(&h,8);memcpy(&type,a,8);
            if(type!=NVN_COUNTER_TYPE_SAMPLES_PASSED)die("GPU counter reset type implementation pending",type);
            counter_op(0,0,0);break;}
        case OP_COPY_B2B:{uint64_t src,dst,len;payload_check(&h,24);memcpy(&src,a,8);memcpy(&dst,a+8,8);memcpy(&len,a+16,8);mtl_sub_copy(src,dst,len);break;}
        case OP_UPDATE_UB:{uint64_t dst,len;payload_check(&h,16);memcpy(&dst,a,8);memcpy(&len,a+8,8);if(len>h.size-24)die("uniform update payload",len);
            all_barrier();uint64_t off=0;VPool *pool=pool_find(dst,len,&off);if(!pool || off%4 || len%4 || len>65536)die("uniform update bounds/alignment",len);
            pool_materialize(pool);vkCmdUpdateBuffer(G.command,pool->v.b,off,len,a+16);mark_write(dst,len);break;}
        case OP_SET_RT:payload_check(&h,sizeof(p_set_rt));pass_end();memcpy(&G.state.rt,a,sizeof(p_set_rt));break;
        case OP_CLEAR_COLOR:payload_check(&h,sizeof(p_clear_color));clear_color((const p_clear_color *)a);break;
        case OP_CLEAR_DS:payload_check(&h,sizeof(p_clear_ds));clear_ds((const p_clear_ds *)a);break;
        case OP_VIEWPORTS:{payload_check(&h,sizeof(p_viewports));const p_viewports *v=(const void *)a;if(v->first || v->n!=1)die("multiple viewports",v->n);
            /* NVN SetViewports records only x/y/w/h; depth range has its own
             * command and survives viewport changes. */
            G.state.viewport.x=v->v[0][0];G.state.viewport.y=v->v[0][1];
            G.state.viewport.width=v->v[0][2];G.state.viewport.height=v->v[0][3];break;}
        case OP_SCISSOR:{payload_check(&h,sizeof(p_scissor));const p_scissor *s=(const void *)a;if(s->x<0||s->y<0||s->w<0||s->h<0)die("scissor bounds",s->x);
            G.state.scissor=(VkRect2D){{s->x,s->y},{s->w,s->h}};break;}
        case OP_DEPTH_RANGE:{payload_check(&h,sizeof(p_depth_range));const p_depth_range *d=(const void *)a;G.state.viewport.minDepth=d->n;G.state.viewport.maxDepth=d->f;break;}
        case OP_BIND_PROGRAM:payload_check(&h,sizeof(p_bind_program));G.state.program=((const p_bind_program *)a)->prog;break;
        case OP_BIND_STATE:{payload_check(&h,sizeof(p_bind_state));const p_bind_state *b=(const void *)a;switch(b->kind){
            case 0:{blend_st s;memcpy(&s,b->bytes,sizeof(s));if(s.target>=8)die("blend target",s.target);G.state.blend[s.target]=s;break;}
            case 1:memcpy(&G.state.color,b->bytes,sizeof(color_st));break;case 2:memcpy(&G.state.mask,b->bytes,sizeof(chmask_st));break;
            case 3:memcpy(&G.state.ms,b->bytes,sizeof(ms_st));break;case 4:memcpy(&G.state.polygon,b->bytes,sizeof(poly_st));break;
            case 5:memcpy(&G.state.ds,b->bytes,sizeof(ds_st));break;default:die("state kind",b->kind);}break;}
        case OP_BIND_VATTR:{payload_check(&h,offsetof(p_bind_vattr,st));const p_bind_vattr *b=(const void *)a;if(b->first<0||b->n<0||b->first>16||b->n>16-b->first)die("vertex attribute range",b->n);
            payload_check(&h,offsetof(p_bind_vattr,st)+(size_t)b->n*sizeof(va_st));
            memcpy(G.state.attributes+b->first,b->st,b->n*sizeof(va_st));break;}
        case OP_BIND_VSTREAM:{payload_check(&h,offsetof(p_bind_vstream,st));const p_bind_vstream *b=(const void *)a;if(b->first<0||b->n<0||b->first>16||b->n>16-b->first)die("vertex stream range",b->n);
            payload_check(&h,offsetof(p_bind_vstream,st)+(size_t)b->n*sizeof(vs_st));
            memcpy(G.state.streams+b->first,b->st,b->n*sizeof(vs_st));break;}
        case OP_BIND_VBUF:{payload_check(&h,offsetof(p_bind_vbuf,r));const p_bind_vbuf *b=(const void *)a;if(b->first<0||b->n<0||b->first>16||b->n>16-b->first)die("vertex buffer range",b->n);
            payload_check(&h,offsetof(p_bind_vbuf,r)+(size_t)b->n*sizeof(NVNbufferRange));
            memcpy(G.state.vbuf+b->first,b->r,b->n*sizeof(NVNbufferRange));break;}
        case OP_BIND_UBO:case OP_BIND_SSBO:{payload_check(&h,sizeof(p_bind_buf));const p_bind_buf *b=(const void *)a;if(b->stage<0||b->stage>=6||b->index<0||b->index>=16)die("buffer stage/index",b->index);
            Range *dest=h.op==OP_BIND_UBO?G.state.uniform[b->stage]:G.state.storage[b->stage];dest[b->index]=(Range){b->addr,b->size};break;}
        case OP_STENCIL:{payload_check(&h,sizeof(p_stencil));const p_stencil *s=(const void *)a;if(s->face<0||s->face>3||s->which<0||s->which>2)die("stencil dynamic parameters",s->face);
            for(int i=0;i<2;i++) {
                if(s->face&(1<<i)){if(s->which==0)G.state.stencil_compare[i]=s->value_mask;
                    else if(s->which==1)G.state.stencil_write[i]=s->mask;else G.state.stencil_ref[i]=s->ref;}
            }
            break;}
        case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:{payload_check(&h,offsetof(p_bind_handles,h));const p_bind_handles *b=(const void *)a;
            int limit=h.op==OP_BIND_IMG?32:128;if(b->stage<0||b->stage>=6||b->first<0||b->n<0||b->n>32||b->first>limit||b->n>limit-b->first)die("texture/sampler binding range",b->n);
            payload_check(&h,offsetof(p_bind_handles,h)+(size_t)b->n*sizeof(uint64_t));
            uint64_t *dest=h.op==OP_BIND_TEX?G.state.tex[b->stage]:h.op==OP_BIND_SAMP?G.state.samp[b->stage]:G.state.img[b->stage];
            memcpy(dest+b->first,b->h,b->n*8);break;}
        case OP_DRAW:payload_check(&h,sizeof(p_draw));draw((const p_draw *)a,NULL);break;
        case OP_DRAW_IDX:payload_check(&h,sizeof(p_draw_idx));draw(NULL,(const p_draw_idx *)a);break;
        case OP_DISPATCH:payload_check(&h,sizeof(p_dispatch));dispatch((const p_dispatch *)a);break;
        case OP_BARRIER:all_barrier();break; /* Full barrier is stronger than every supported NVN barrier bit. */
        case OP_POLY_OFFSET:{payload_check(&h,sizeof(p_poly_offset));const p_poly_offset *v=(const void *)a;
            if(v->factor || v->units || v->clamp)die("nonzero polygon offset requires Vulkan pipeline bias",0);
            break;}
        case OP_WAIT_EVENT:{uint64_t address;uint32_t mode,value;payload_check(&h,16);
            memcpy(&address,a,8);memcpy(&mode,a+8,4);memcpy(&value,a+12,4);
            if(!pool_find(address,4,NULL) || address%4 || mode>1)die("event wait storage/mode",address);
            /* Submit preceding work and publish only its actual GPU results.
             * This also handles a signal earlier in the same command handle. */
            mtl_sub_commit();
            uint64_t start=dm_android_nvn_clock_ns();
            for(;;){uint32_t observed=atomic_load((_Atomic uint32_t *)(uintptr_t)address);
                if(mode==0 ? observed==value : (int32_t)(observed-value)>=0)break;
                if(dm_android_nvn_clock_ns()-start>30000000000ULL)die("event wait exceeded diagnostic deadline",address);
                struct timespec pause={0,100000};nanosleep(&pause,NULL);
            }
            mtl_sub_begin();break;}
        case OP_WAIT_SYNC:case OP_FENCE_SYNC:{payload_check(&h,sizeof(p_sync));const p_sync *v=(const void *)a;
            if(!v->sync || v->sync->magic!=MAGIC('S','Y','N','C'))die("sync lifetime",0);
            if(h.op==OP_WAIT_SYNC){
                if(!v->sync->fenced || v->sync->fence>G.fence+1)die("wait on unsubmitted sync",v->sync->fence);
                /* Earlier submits finish synchronously. A fence recorded in this
                 * submit is ordered by a full GPU memory/execution barrier. */
                all_barrier();
            }else{
                if(v->condition<0 || v->condition>1 || v->flags&~1)die("fence condition/flags",v->flags);
                all_barrier();v->sync->fence=G.fence+1;v->sync->fenced=1;
                /* CPU completion remains false until vkWaitForFences finishes. */
            }break;}
        case OP_COPY_B2T:payload_check(&h,sizeof(p_copy_b2t));copy_b2t((const p_copy_b2t *)a);break;
        case OP_CLEAR_TEX:payload_check(&h,sizeof(p_clear_tex));clear_tex((const p_clear_tex *)a);break;
        case OP_COPY_T2T:payload_check(&h,sizeof(p_copy_t2t));copy_t2t((const p_copy_t2t *)a);break;
        case OP_COPY_STRIDE:payload_check(&h,sizeof(p_copy_stride));G.state.copy_stride=((const p_copy_stride *)a)->stride;break;
        case OP_BLEND_COLOR:payload_check(&h,sizeof(p_blend_color));memcpy(G.state.blend_color,((const p_blend_color *)a)->rgba,16);break;
        case OP_SHADER_SCRATCH:payload_check(&h,sizeof(p_bind_buf));
            {const p_bind_buf *r=(const p_bind_buf *)a;
             if(!pool_find(r->addr,r->size,NULL))die("shader scratch pool lifetime/range",r->addr);
             /* Maxwell LDL/STL are translated to SPIR-V Private variables.
              * Vulkan's compiler owns the physical per-invocation allocation;
              * the NVN scratch range has no application-visible payload. */
             G.state.scratch=(Range){r->addr,r->size};}break;
        case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:payload_check(&h,sizeof(p_set_pool));
            if(!((const p_set_pool *)a)->pool)die("null descriptor pool",h.op);
            {const dpool_t **current=h.op==OP_SET_TEX_POOL?&G.state.texture_pool:&G.state.sampler_pool;
             if(*current && *current!=((const p_set_pool *)a)->pool)die("multiple descriptor pools need qualified registration",h.op);
             *current=((const p_set_pool *)a)->pool;}break;
        default:die("unimplemented NVN command opcode",h.op);
        }
        p+=h.size;
    }
    die("command iteration limit",depth);
}
void mtl_exec(const uint8_t *handle) { if(!G.active)die("execute without submission",0);execute(handle,0); }
void mtl_exec_rest(const uint64_t *handles,int n) { (void)handles;(void)n; } /* Optimization-only lookahead; replay order is unchanged. */
void mtl_sub_index(const uint64_t *handles,int n) { (void)handles;(void)n; } /* No attachment store-elision optimizer in this backend. */

void dm_vk_set_window(void *native) {
    mtl_init();pthread_mutex_lock(&G.lock);CHECK(vkQueueWaitIdle(G.queue));
    /* Focus/resume notifications may publish the same BufferQueue again.
     * Retain its Vulkan surface instead of disconnecting and reconnecting the
     * producer. Invalidate only the swapchain if this window changed size. */
    if(native && G.window==native){
        if(G.swapchain && ((uint32_t)ANativeWindow_getWidth(G.window)!=G.swap_extent.width ||
                          (uint32_t)ANativeWindow_getHeight(G.window)!=G.swap_extent.height)){
            vkDestroySwapchainKHR(G.device,G.swapchain,NULL);G.swapchain=VK_NULL_HANDLE;G.nswap=0;
        }
        pthread_mutex_unlock(&G.lock);return;
    }
    if(G.swapchain)vkDestroySwapchainKHR(G.device,G.swapchain,NULL);if(G.surface)vkDestroySurfaceKHR(G.instance,G.surface,NULL);
    if(G.window)ANativeWindow_release(G.window);G.window=native;G.swapchain=VK_NULL_HANDLE;G.surface=VK_NULL_HANDLE;G.nswap=0;
    if(G.window){ANativeWindow_acquire(G.window);VkAndroidSurfaceCreateInfoKHR i={.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,.window=G.window};
        PFN_vkCreateAndroidSurfaceKHR create=(PFN_vkCreateAndroidSurfaceKHR)dm_vk_get(G.instance,"vkCreateAndroidSurfaceKHR");
        if(!create)die("Android Vulkan surface entry",0);CHECK(create(G.instance,&i,NULL,&G.surface)); }
    pthread_mutex_unlock(&G.lock);
}
static void swapchain_make(void) {
    if(G.swapchain)return;if(!G.surface)die("present without Android Surface",0);
    VkBool32 supported=0;CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(G.physical,G.family,G.surface,&supported));if(!supported)die("queue surface support",0);
    VkSurfaceCapabilitiesKHR caps;CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(G.physical,G.surface,&caps));
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))die("swapchain transfer usage",caps.supportedUsageFlags);
    uint32_t n=0;CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(G.physical,G.surface,&n,NULL));if(!n)die("surface formats",0);
    VkSurfaceFormatKHR *formats=calloc(n,sizeof(*formats));if(!formats)die("surface format allocation",n);
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(G.physical,G.surface,&n,formats));VkSurfaceFormatKHR chosen=formats[0];
    for(uint32_t i=0;i<n;i++)if(formats[i].format==VK_FORMAT_R8G8B8A8_UNORM || formats[i].format==VK_FORMAT_B8G8R8A8_UNORM){chosen=formats[i];break;}free(formats);
    G.swap_format=chosen.format;G.swap_extent=caps.currentExtent;
    if(G.swap_extent.width==UINT32_MAX)G.swap_extent=(VkExtent2D){(uint32_t)ANativeWindow_getWidth(G.window),(uint32_t)ANativeWindow_getHeight(G.window)};
    uint32_t count=caps.minImageCount+1;if(caps.maxImageCount && count>caps.maxImageCount)count=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;if(!(caps.supportedCompositeAlpha&alpha))alpha=VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if(!(caps.supportedCompositeAlpha&alpha))die("surface composite alpha",caps.supportedCompositeAlpha);
    VkSwapchainCreateInfoKHR i={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=G.surface,.minImageCount=count,
      .imageFormat=chosen.format,.imageColorSpace=chosen.colorSpace,.imageExtent=G.swap_extent,.imageArrayLayers=1,
      .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR?VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:caps.currentTransform,
      .compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};CHECK(vkCreateSwapchainKHR(G.device,&i,NULL,&G.swapchain));
    CHECK(vkGetSwapchainImagesKHR(G.device,G.swapchain,&G.nswap,NULL));if(G.nswap>8)die("swapchain image limit",G.nswap);
    CHECK(vkGetSwapchainImagesKHR(G.device,G.swapchain,&G.nswap,G.swap_images));memset(G.swap_layouts,0,sizeof(G.swap_layouts));
}
uint64_t mtl_present(const tex_t *source,double period,const int crop[4]) {
    uint64_t present_started=dm_android_nvn_clock_ns();
    (void)period;if(!G.window)die("original game present requires Surface",0);mtl_sub_begin();swapchain_make();
    VTexture *t=texture_get(source,0);if(aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)die("present depth texture",t->format);
    if(G.present_count<2 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] PRESENT_FORMAT frame=%llu source_format=%u swap_format=%u source_extent=%u,%u swap_extent=%u,%u\n",(unsigned long long)G.present_count+1,t->format,G.swap_format,t->width,t->height,G.swap_extent.width,G.swap_extent.height);
    const char *capture_root=getenv("DM_FRAME_DIR");VBuffer capture={0};
    int capture_frame=capture_root && (G.trace_frame || G.present_count<4 || (G.present_count%120==0 && G.present_count<1000));
    if(capture_frame && t->bpp==4){capture=buffer_make((uint64_t)t->width*t->height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);VkBufferImageCopy c={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
        vkCmdCopyImageToBuffer(G.command,t->image,t->layout,capture.b,1,&c);}
    VkSemaphoreCreateInfo sem_info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore available,finished;
    CHECK(vkCreateSemaphore(G.device,&sem_info,NULL,&available));CHECK(vkCreateSemaphore(G.device,&sem_info,NULL,&finished));
    uint32_t index=0;VkResult acquired=vkAcquireNextImageKHR(G.device,G.swapchain,UINT64_MAX,available,VK_NULL_HANDLE,&index);
    if(acquired!=VK_SUCCESS && acquired!=VK_SUBOPTIMAL_KHR)die("swapchain acquisition",acquired);
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=G.swap_layouts[index],
       .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
       .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=G.swap_images[index],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(G.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    int x=0,y=0,w=t->width,h=t->height;if(crop && crop[2]>0 && crop[3]>0){x=crop[0];y=crop[1];w=crop[2];h=crop[3];}
    if(x<0||y<0||w<=0||h<=0||(uint64_t)x+w>t->width||(uint64_t)y+h>t->height)die("present crop bounds",w);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{x,y,0},{x+w,y+h,1}},
      .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int)G.swap_extent.width,(int)G.swap_extent.height,1}}};
    VkFormatProperties src_format,dst_format;vkGetPhysicalDeviceFormatProperties(G.physical,t->format,&src_format);
    vkGetPhysicalDeviceFormatProperties(G.physical,G.swap_format,&dst_format);
    if(!(src_format.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(dst_format.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))die("present blit formats",t->format);
    vkCmdBlitImage(G.command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,G.swap_images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,
       src_format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
    vkCmdPipelineBarrier(G.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
    G.swap_layouts[index]=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;CHECK(vkEndCommandBuffer(G.command));
    VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,
      .pWaitSemaphores=&available,.pWaitDstStageMask=&wait_stage,.commandBufferCount=1,.pCommandBuffers=&G.command,
      .signalSemaphoreCount=1,.pSignalSemaphores=&finished};
    VkFenceCreateInfo f_info={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;CHECK(vkCreateFence(G.device,&f_info,NULL,&fence));
    CHECK(vkQueueSubmit(G.queue,1,&si,fence));VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,
      .pWaitSemaphores=&finished,.swapchainCount=1,.pSwapchains=&G.swapchain,.pImageIndices=&index};
    VkResult presented=vkQueuePresentKHR(G.queue,&pi);if(presented!=VK_SUCCESS && presented!=VK_SUBOPTIMAL_KHR)die("queue present",presented);
    CHECK(vkWaitForFences(G.device,1,&fence,VK_TRUE,UINT64_MAX));CHECK(vkQueueWaitIdle(G.queue));
    trace_capture_complete();
    if(capture.b){char file[1200];snprintf(file,sizeof file,"%s/frame-%04llu.ppm",capture_root,(unsigned long long)G.present_count+1);
        FILE *out=fopen(file,"wb");if(!out)die("frame readback output",0);fprintf(out,"P6\n%u %u\n255\n",t->width,t->height);
        size_t row_bytes=(size_t)t->width*3;uint8_t *rgb_row=malloc(row_bytes);if(!rgb_row)die("frame readback row allocation",row_bytes);
        unsigned nonblack=0;unsigned red=t->format==VK_FORMAT_B8G8R8A8_UNORM?2:0,blue=2-red;
        for(unsigned y=0;y<t->height;y++){
            const uint8_t *pixels=capture.map+(uint64_t)y*t->width*4;
            for(unsigned x=0;x<t->width;x++){const uint8_t *pixel=pixels+(size_t)x*4;uint8_t *rgb=rgb_row+(size_t)x*3;
                rgb[0]=pixel[red];rgb[1]=pixel[1];rgb[2]=pixel[blue];if(rgb[0]||rgb[1]||rgb[2])nonblack++;}
            if(fwrite(rgb_row,1,row_bytes,out)!=row_bytes)die("frame readback write",row_bytes);
        }free(rgb_row);
        fclose(out);fprintf(stderr,"[nvn-vulkan] PRESENT_READBACK frame=%llu extent=%u,%u nonblack=%u draws=%llu passes=%llu\n",(unsigned long long)G.present_count+1,t->width,t->height,nonblack,(unsigned long long)G.draws,(unsigned long long)G.pass_count);buffer_free(&capture);
    }
    vkDestroyFence(G.device,fence,NULL);vkDestroySemaphore(G.device,available,NULL);vkDestroySemaphore(G.device,finished,NULL);
    vkDestroyQueryPool(G.device,G.occlusion,NULL);vkDestroyQueryPool(G.device,G.timestamps,NULL);
    G.occlusion=G.timestamps=VK_NULL_HANDLE;
    for(int i=0;i<G.nview;++i)vkDestroyImageView(G.device,G.temporary_views[i],NULL);
    vkFreeCommandBuffers(G.device,G.command_pool,1,&G.command);vkDestroyDescriptorPool(G.device,G.descriptors,NULL);
    for(int i=0;i<G.ntemporary;i++){buffer_free(G.temporaries[i]);free(G.temporaries[i]);}G.command=VK_NULL_HANDLE;G.active=0;
    G.profile_frames++;
    G.profile_present_ns+=dm_android_nvn_clock_ns()-present_started;
    if(G.trace_frame || G.profile_frames>=60 || G.present_count<2){uint64_t now=dm_android_nvn_clock_ns();
        fprintf(stderr,"[nvn-vulkan] RENDER_PROFILE frame=%llu frames=%llu elapsed_ms=%.3f pipeline_ms=%.3f gpu_wait_ms=%.3f upload_decode_ms=%.3f submits=%llu draws=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,(now-G.profile_start_ns)/1e6,G.profile_pipeline_ns/1e6,G.profile_wait_ns/1e6,G.profile_decode_ns/1e6,(unsigned long long)G.profile_submits,(unsigned long long)(G.draws-G.profile_draws));
        fprintf(stderr,"[nvn-vulkan] RENDER_CPU_PROFILE frame=%llu lookup_ms=%.3f pool_ms=%.3f bind_ms=%.3f renderpass_ms=%.3f present_ms=%.3f\n",(unsigned long long)G.present_count+1,G.profile_lookup_ns/1e6,G.profile_pool_ns/1e6,G.profile_bind_ns/1e6,G.profile_render_ns/1e6,G.profile_present_ns/1e6);
        G.profile_lookup_ns=G.profile_pool_ns=G.profile_bind_ns=G.profile_render_ns=G.profile_present_ns=0;
        G.profile_start_ns=now;G.profile_pipeline_ns=G.profile_wait_ns=G.profile_decode_ns=G.profile_submits=G.profile_frames=0;G.profile_draws=G.draws;}
    G.trace_frame=G.trace_ui=0;
    if(capture_root){char request[1200];snprintf(request,sizeof request,"%s/trace-ui-request",capture_root);
        if(!access(request,F_OK)){if(unlink(request))die("UI trace request consume",0);G.trace_frame=G.trace_ui=1;G.trace_meshes=G.ntrace_textures=0;
            fprintf(stderr,"[nvn-vulkan] TRACE_NEXT_FRAME frame=%llu ui=1\n",(unsigned long long)G.present_count+2);}
        snprintf(request,sizeof request,"%s/trace-request",capture_root);
        if(!access(request,F_OK)){if(unlink(request))die("trace request consume",0);G.trace_frame=1;G.trace_meshes=G.ntrace_textures=0;
            fprintf(stderr,"[nvn-vulkan] TRACE_NEXT_FRAME frame=%llu\n",(unsigned long long)G.present_count+2);}}
    G.completed=++G.fence;uint64_t ticket=++G.present_count;pthread_mutex_unlock(&G.lock);return ticket;
}
void mtl_present_wait(uint64_t ticket) { if(ticket>G.present_count)die("unknown present ticket",ticket); }
void mtl_present_flush(void) { mtl_finish(); }
int mtl_has_window(void) { return G.window!=NULL; }
void dm_vk_set_display_hz(int hz) { if(hz<0 || hz>1000)die("Android display refresh rate",hz);G.display_hz=hz; }
int mtl_display_hz(void) { return G.window?G.display_hz:0; } /* 0 means the front end has not supplied a measured display mode. */
double mtl_presented_time(uint64_t ticket) { (void)ticket;return 0; } /* Actual scanout timing is not available yet. */
uint64_t mtl_present_count(void) { return G.present_count; }
void mtl_read_texels(const tex_t *source,unsigned level,unsigned layer,const NVNcopyRegion *region,uint8_t *dst,size_t row,size_t img) {
    int volume=source->d.target==NVN_TEXTURE_TARGET_3D;
    unsigned depth=volume?source->d.depth>>level:1;if(!depth)depth=1;
    if(G.active || !region || region->zoffset<0 || (unsigned)region->zoffset>=depth || region->depth!=1 || level>=source->d.levels || layer>=source->L.layers)die("CPU texture read mip/layer/inflight",level);
    uint32_t decoded_format;int decoded_bpp;if(nvn_bc_unpacked(source->d.format,&decoded_format,&decoded_bpp))die("BC readback needs original-format encoding",source->d.format);
    mtl_sub_begin();VTexture *t=texture_get(source,1);unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
    if(region->xoffset<0||region->yoffset<0||region->width<0||region->height<0||
      (uint64_t)region->xoffset+region->width>width||(uint64_t)region->yoffset+region->height>height)die("CPU texture read bounds",region->width);
    size_t pitch=(size_t)region->width*t->bpp;if(row<pitch || (img && img<row*(size_t)region->height))die("CPU texture read stride",row);
    VBuffer readback=buffer_make(pitch*region->height,VK_BUFFER_USAGE_TRANSFER_DST_BIT);texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy={.imageSubresource={aspect(t->format),level,layer,1},.imageOffset={region->xoffset,region->yoffset,region->zoffset},.imageExtent={region->width,region->height,1}};
    vkCmdCopyImageToBuffer(G.command,t->image,t->layout,readback.b,1,&copy);mtl_sub_commit();
    for(int y=0;y<region->height;y++)memcpy(dst+y*row,readback.map+y*pitch,pitch);buffer_free(&readback);
}
#ifdef DM_VK_COPY_TEST
void dm_vk_copy_test_pool_bytes(uint64_t *logical,uint64_t *resident) {
    *logical=*resident=0;for(int i=0;i<G.npool;i++){*logical+=G.pools[i].size;*resident+=G.pools[i].v.size;}
}
/* Independent device probe only: inspect expanded BC pixels without promising
 * compressed guest-format CPU readback or introducing a lossy BC encoder. */
void dm_vk_copy_test_read(const tex_t *source,unsigned level,unsigned layer,uint8_t *dst) {
    if(G.active || level>=source->d.levels || layer>=source->L.layers)die("copy probe subresource",level);
    mtl_sub_begin();VTexture *t=texture_get(source,1);
    unsigned w=t->width>>level,h=t->height>>level;if(!w)w=1;if(!h)h=1;
    VBuffer readback=buffer_make((uint64_t)w*h*t->bpp,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c={.imageSubresource={aspect(t->format),level,layer,1},.imageExtent={w,h,1}};
    vkCmdCopyImageToBuffer(G.command,t->image,t->layout,readback.b,1,&c);mtl_sub_commit();
    memcpy(dst,readback.map,(size_t)w*h*t->bpp);buffer_free(&readback);
}
#endif
void mtl_stats(uint64_t *passes,uint64_t *draws) { *passes=G.pass_count;*draws=G.draws; }
void mtl_pkg_stats(uint64_t *uploads,uint64_t *reuploads) { *uploads=G.uploads;*reuploads=0; }
void mtl_set_frame(uint64_t frame) { G.frame=frame; }
double mtl_gpu_ms(uint64_t frame) { (void)frame;return -1; }
int mtl_gpu_span(uint64_t frame,uint64_t *a,uint64_t *b) { (void)frame;(void)a;(void)b;return 0; }
int mtl_warm_wait(int timeout) { (void)timeout;return 0; } /* No asynchronous pipeline warm-up job exists. */
int mtl_link_active(void) { return 0; } /* No Android frame-link service has been installed. */
int mtl_link_wait_tick(uint64_t timeout) { (void)timeout;return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; } /* No cross-frame store-elision ledger exists. */
void mtl_ub_present(double ms) { (void)ms; } /* Disabled diagnostic experiment. */
uint64_t mtl_upload_bytes(void) { uint64_t n=0;for(int i=0;i<G.ntexture;i++)if(G.textures[i].cpu_owned)n+=(uint64_t)G.textures[i].width*G.textures[i].height*G.textures[i].bpp;return n; }
