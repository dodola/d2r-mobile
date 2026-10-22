/* First Android Vulkan replay implementation for the original NVN recorder.
 * This file preserves the recorder's mtl_* C interface to avoid guest changes.
 * Unsupported semantics abort with a named boundary; no draw/dispatch is dropped.
 * The initial queue is deliberately synchronous. Guest-visible writes/events
 * are published only after a real fence has completed.
 */
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include "nvn_obj.h"
#include "nvn_vk_attachment_plan.h"
#include "nvn_vk_coverage.h"
#include "nvn_vk_pipeline_store.h"
#include "nvn_vk_upload.h"
#include "nvn_translation_sync.h"
#include "../../android/nvn_platform_clock.h"
#include "nvn_mtl.h"
#include "nvn_dirty.h"
#include "nvn_dirty_summary.h"
#include "nvn_copy_pitch.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_bcdec.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include <math.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
#include "../../android/nvn_platform_backend.h"

void dm_guest_write_prepare(void *address,size_t length);
static int g_bc_passthrough,g_texture_upload_batch;

#define LIMIT_POOLS 512
#define LIMIT_OBJECTS 4096
#define LIMIT_BINDINGS 64
#define LIMIT_WRITES 4096
#define LIMIT_PSOS 2048
#define PSO_TABLE_SIZE 4096
#define DRIVER_TABLE_BYTES 0x968
#define DRIVER_TABLE_SLOTS 8192
#ifndef TEMP_POOL_COUNT
#define TEMP_POOL_COUNT 512
#endif
#ifndef TEMP_POOL_BYTES
#define TEMP_POOL_BYTES (128ULL*1024*1024)
#endif
#define CHECK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS) die(#x,(long long)r_); } while(0)
static VkResult cpu_work_wait_fences(VkDevice device,uint32_t count,const VkFence *fences,VkBool32 all,uint64_t timeout) {
    uint64_t token=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_FENCE);
    VkResult result=vkWaitForFences(device,count,fences,all,timeout);dm_android_nvn_work_wait_end(token);return result;
}
static VkResult cpu_work_queue_idle(VkQueue queue) {
    uint64_t token=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_QUEUE_IDLE);
    VkResult result=vkQueueWaitIdle(queue);dm_android_nvn_work_wait_end(token);return result;
}
static _Noreturn void die(const char *where,long long value) {
    fprintf(stderr,"[nvn-vulkan] STOP %s (%lld)\n",where,value); fflush(stderr); abort();
}
#include "nvn_vk_dispatch.h"
/* Any command that can write memory or change image state marks the command buffer dirty;
 * all_barrier() omits a full barrier only when nothing was recorded since the previous one. */
enum { HZ_DRAW=1,HZ_ATTACH,HZ_TRANSFER,HZ_COMPUTE };
static void hz_note(int kind);
#undef vkCmdCopyImage
#define vkCmdCopyImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&1)?(void)0:dm_vkCmdCopyImage(__VA_ARGS__))
#undef vkCmdCopyImageToBuffer
#define vkCmdCopyImageToBuffer(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&16)?(void)0:dm_vkCmdCopyImageToBuffer(__VA_ARGS__))
#undef vkCmdCopyBufferToImage
#define vkCmdCopyBufferToImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&8)?(void)0:dm_vkCmdCopyBufferToImage(__VA_ARGS__))
#undef vkCmdCopyBuffer
#define vkCmdCopyBuffer(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&4)?(void)0:dm_vkCmdCopyBuffer(__VA_ARGS__))
#undef vkCmdClearAttachments
#define vkCmdClearAttachments(...) (S.command_dirty=1,hz_note(HZ_ATTACH),dm_vkCmdClearAttachments(__VA_ARGS__))
#undef vkCmdBlitImage
#define vkCmdBlitImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&32)?(void)0:dm_vkCmdBlitImage(__VA_ARGS__))
#undef vkCmdUpdateBuffer
#define vkCmdUpdateBuffer(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&64)?(void)0:dm_vkCmdUpdateBuffer(__VA_ARGS__))
#undef vkCmdBeginRenderPass
#define vkCmdBeginRenderPass(...) (S.command_dirty=1,dm_vkCmdBeginRenderPass(__VA_ARGS__))
#undef vkCmdEndRenderPass
#define vkCmdEndRenderPass(...) (S.command_dirty=1,dm_vkCmdEndRenderPass(__VA_ARGS__))
#undef vkCmdDraw
#define vkCmdDraw(...) (S.command_dirty=1,hz_note(HZ_DRAW),dm_vkCmdDraw(__VA_ARGS__))
#undef vkCmdDrawIndexed
#define vkCmdDrawIndexed(...) (S.command_dirty=1,hz_note(HZ_DRAW),dm_vkCmdDrawIndexed(__VA_ARGS__))
#undef vkCmdDispatch
#define vkCmdDispatch(...) (S.command_dirty=1,hz_note(HZ_COMPUTE),dm_vkCmdDispatch(__VA_ARGS__))
#undef vkCmdClearColorImage
#define vkCmdClearColorImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&2)?(void)0:dm_vkCmdClearColorImage(__VA_ARGS__))
#undef vkCmdClearDepthStencilImage
#define vkCmdClearDepthStencilImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&2)?(void)0:dm_vkCmdClearDepthStencilImage(__VA_ARGS__))
#undef vkCmdPipelineBarrier
#define vkCmdPipelineBarrier(...) (S.command_dirty=1,dm_vkCmdPipelineBarrier(__VA_ARGS__))
#include "nvn_vk_storage_bias.h"
#include "nvn_vk_ballot.h"
#include "nvn_vk_skin.h"
#include "nvn_vk_profile.h"
#include "nvn_vk_pipeline_cache_key.h"
#include "nvn_vk_sampler_policy.h"
#include "nvn_vk_depth_bias.h"
#include "nvn_vk_vertex.h"
#include "nvn_vk_vertex_spv.h"
#include "nvn_vk_null_shader.h"
#include "nvn_vk_draw_parameters.h"
#include "nvn_vk_distance_capabilities.h"
#include "nvn_vk_redundant_clip.h"
#include "nvn_vk_unused_distance.h"
#include "nvn_vk_image_type.h"
#include "nvn_vk_depth_input.h"
#include "nvn_vk_ui_ghost.h"
#include "nvn_vk_memory.h"
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *map; uint64_t size,allocation_size;VkBufferUsageFlags usage;int cached;uint64_t dirty;unsigned memory_class; } VBuffer;
enum { TEMP_OTHER,TEMP_DRIVER,TEMP_ALIGNED,TEMP_COMPARISON,TEMP_ROLES };
/* Binding-plan failure buckets identify the first invalidation cause. */
enum {
    PLAN_MISS_NO_PLAN,PLAN_MISS_DISABLED,PLAN_MISS_TRACE,PLAN_MISS_INELIGIBLE,
    PLAN_MISS_UNRESOLVED,PLAN_MISS_EPOCH,PLAN_MISS_STATE_REVISION,PLAN_MISS_REGISTRY_REVISION,
    PLAN_MISS_POOL_REVISION,PLAN_MISS_BUFFER_RANGE,PLAN_MISS_BUFFER_ALIGNMENT,
    PLAN_MISS_BUFFER_POOL,PLAN_MISS_BUFFER_OFFSET,PLAN_MISS_IMAGE_INVALID,
    PLAN_MISS_IMAGE_IDENTITY,PLAN_MISS_IMAGE_VIEW,PLAN_MISS_IMAGE_DISJOINT,
    PLAN_MISS_IMAGE_LAYOUT,PLAN_MISS_SAMPLER,PLAN_MISS_COUNT
};
typedef struct { const uint8_t *guest; uint64_t size,serial; VBuffer v; uint32_t *use_rd[4],*use_wr[4]; } VPool;
typedef struct { uint64_t address,size,pool_serial,stamp,age; } PoolUpload;
typedef struct { uint64_t epoch,ssbo,tex;VBuffer *table; } DriverCacheEntry;
#define UPLOAD_BUCKETS 4096
#define UPLOAD_WAYS 4
typedef struct { uint32_t set,binding,type,kind; int32_t slot,sampler; uint32_t count,flags; } RBinding;
typedef struct { uint32_t magic,version,stage,nb,na,c1n,ncap,local[3],fout,reserved; } RHeader;
#include "nvn_vk_binding_plan.h"
typedef struct VertexModule { NvnVkVertexAdapt plan; uint64_t null_bindings;VkShaderModule module; struct VertexModule *next; } VertexModule;
typedef struct NvnNullModule {uint64_t mask;VkShaderModule module;struct NvnNullModule *next;} NvnNullModule;
typedef struct VShader VShader;
struct VShader {
    char key[17]; RHeader h; RBinding b[LIMIT_BINDINGS]; uint32_t attr[32][2],caps[32];
    VBuffer c1; VkShaderModule module; VkDescriptorSetLayout layout; uint64_t fetch_bindings,dref_bindings,large_bindings_seen,texture_layout_logged;
    uint64_t dyn_bindings;uint32_t dyn_count;
    SBInfo storage_bias;
    BindingPlan *plan;
    uint8_t *vertex_spv;size_t vertex_bytes;NvnVkVertexInput vertex_inputs[16];VertexModule *vertex_modules;
    uint32_t *spirv;size_t spirv_words;NvnNullModule *null_modules;unsigned null_module_count;
    int vertex_layout_logged,requires_native_depth_clip;
    uint32_t draw_parameter_mask;
    NvnVkImageType image_types[LIMIT_BINDINGS];uint64_t image_type_mask,typed_view_logged;
    uint32_t identity;int depth_input;VShader *depth_input_variant;
};
typedef struct { uint64_t serial,incarnation; tex_t source; VkImage image; VkDeviceMemory memory;
    VkImageView view; VkFormat format; VkImageLayout layout; VkImageUsageFlags usage; uint32_t width,height,depth,bpp; int cpu_owned; uint64_t upload_stamp,version;
    /* At most one lazy STORAGE promotion. Prior commands and slot caches keep
     * the old allocation alive until texture finalization drains both slots. */
    VkImage before_storage;VkDeviceMemory before_storage_memory;VkImageView before_storage_view;
    uint8_t *cpu_snapshot;
    uint8_t bc_block; /* 0 = decoded texels; 8/16 = native BC block bytes. */
    uint64_t allocation_size,before_storage_allocation_size,sample_allocation_size,shadow_allocation_size,cpu_snapshot_size;
    VkImage sample_copy;VkDeviceMemory sample_memory;VkImageView sample_view;VkImageLayout sample_layout;
    VkImage shadow;VkDeviceMemory shadow_memory;VkImageView shadow_view;VkFormat shadow_format;VkImageLayout shadow_layout;uint64_t shadow_version;uint64_t other_version,sample_other;uint32_t dirty_levels,sc_valid,sc_level,sc_levels,sc_layer,sc_layers;
    VkImageLayout lvl[16];int mixed; /* per-mip layouts, valid only while mixed (then layout is stale) */ } VTexture;
static void tex_dirty(VTexture *t){t->version++;t->other_version++;}
static void tex_attach_dirty(VTexture *t,unsigned level){t->version++;t->dirty_levels|=1u<<(level&31);}
typedef struct { int live; tex_t texture; view_t view; uint8_t has_view; uint64_t revision;TextureRef resident; } TSlot;
typedef struct { int live; samp_par source; VkSampler sampler,nearest,integer_nearest;VkSamplerCreateInfo info; } SSlot;
typedef struct { volatile uint32_t *addr; uint32_t mode,value; } Event;
typedef struct { uint32_t kind,query; uint64_t address; } CounterOp;
typedef struct { Range range; VBuffer *buffer; int written; } AlignedBinding;
typedef struct { Range range;VBuffer *buffer;int write_count; } ReadSnapshot;
typedef struct { uint32_t hash;VBuffer *buffer; } DriverTable;
typedef struct { uint64_t serial; unsigned roles; } ImageUse;
typedef struct { const uint8_t *entry;uint64_t hash; } ShaderEntry;
/* Per-draw shader identity memo (full-code identity, index v2). The key is the guest object, not its content: a hit
 * still re-verifies the control block and the code bytes against the indexed entry, so a rewritten shader misses. */
/* stamp: dirty-tracker generation taken (with the code pages write-protected) just before the code bytes were last
 * compared with the indexed copy; 0 = never verified under protection. pool_serial keeps a recreated pool at the same
 * address from inheriting the stamp. */
typedef struct { const void *control;uint64_t address;const uint8_t *entry;uint32_t stage,code_size;uint64_t pool_serial,stamp; } ShaderLookup;
#define SHADER_LOOKUP 2048
typedef struct { uint64_t hash;VShader *shader;VkDescriptorSet set;uint32_t nb,off; } DescEntry;
/* Vulkan returns selected statistics in ascending flag-bit order: input
 * vertices, input primitives, vertex invocations, fragment invocations. */
#define PASS_STATS_FLAGS (VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT|VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT|VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT|VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT)
typedef struct { uint64_t key,draws,stats[4];uint32_t w,h,n,passes;double ms; } PassAcc;
typedef struct { const void *control;uint32_t stage,valid;VShader *shader;uint8_t bytes[0x880]; } ControlCache;
#define DESC_ENTRIES 8192
#define DESC_TABLE 16384
#define DESC_ARENA (8u<<20)
#define CONTROL_CACHE 4096
typedef struct { VBuffer *buffer;uint64_t bytes,offset;char path[1400]; } TraceCapture;
/* Only one subpass and single-sample attachments are supported here. Keep
 * the complete render-pass description: reuse never depends on image handles,
 * framebuffer size or Vulkan render-pass handle allocation. */
typedef struct {
    uint32_t count,color_count,has_depth,dependency_count;
    uint32_t input_count,subpass_flags,input_aspect;
    VkAttachmentDescription attachments[9];
    VkAttachmentReference color[8],depth,input;
    VkSubpassDependency dependencies[3];
} PassKey;

/* Submission-owned resources are immutable and remain live until its fence.
 * Include the complete view and framebuffer description in reuse keys. */
typedef struct {
    VkImage image;VkImageViewCreateFlags flags;VkImageViewType type;VkFormat format;
    VkComponentMapping components;VkImageSubresourceRange range;
} ImageViewKey;
typedef struct { PassKey pass;VkImageView views[9];uint32_t width,height; } FramebufferKey;
#define VIEW_TABLE_SIZE 8192
#define RETAINED_PASS_LIMIT 128
#define RETAINED_VIEW_LIMIT 256
typedef struct {
    uint32_t compute,shader[2],flags,topology,cull,front;
    uint32_t attribute_count,binding_count,color_count;
    uint32_t vertex_split,vertex_unsigned,vertex_signed;
    VkVertexInputAttributeDescription attributes[32];
    VkVertexInputBindingDescription bindings[16];
    uint32_t depth_test,depth_write,depth_compare,stencil_test,depth_flags,depth_clamp;
    NvnVkDepthBias depth_bias;
    uint32_t shader_variant[2];
    VkStencilOpState stencil[2];
    VkPipelineColorBlendAttachmentState blend[8];
    uint32_t storage_bias[2][LIMIT_BINDINGS];
    uint64_t null_bindings[2];
    int32_t draw_parameters[3];
    PassKey pass;
} PsoKey;
typedef struct { VkPipeline pipeline; VkPipelineLayout layout; } Pso;
typedef struct { PsoKey key;Pso pso;uint64_t hash; } CachedPso;
typedef struct {
    const prog_t *programs[6]; p_set_rt rt; va_st attributes[16]; vs_st streams[16]; NVNbufferRange vbuf[16];
    Range uniform[6][16],storage[6][16]; uint64_t tex[6][128],samp[6][128],img[6][32];
    color_st color; chmask_st mask; blend_st blend[8]; poly_st polygon; p_poly_offset polygon_offset; ds_st ds; ms_st ms;
    VkViewport viewport; VkRect2D scissor; int scissor_set; float blend_color[4]; int64_t copy_stride,copy_image_stride;
    const dpool_t *texture_pool,*sampler_pool; Range scratch; uint32_t stencil_compare[2],stencil_write[2],stencil_ref[2];
    uint32_t dispatch[3],depth_clamp;
} ReplayState;
#include "nvn_vk_writeback_plan.h"
#include "nvn_vk_command_state.h"
#include "nvn_vk_submission.h"
#include "nvn_vk_command_stream.h"
#include "nvn_vk_shader_index.h"
#include "nvn_vk_zcull.h"
static struct {
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family; VkSampler fetch_sampler;
    VkPhysicalDeviceMemoryProperties memory; VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures enabled_features;
    VkFormatProperties format_properties[256];uint8_t format_known[256];
    int depth_order,depth_input_experiment,depth_input_profile,depth_input_calibrated_ties,fragment_stores;
    VkPipelineCache pipeline_cache;
    VkSubmission submission_slot,submission_slot2;VkSubmission extra_render_slots[3];int render_slots_n,render_ring_next;uint64_t slot_conflicts,dump_frame;int filter_cubic_ext,present_cubic;VkSubmission *current_submission;VkSemaphore swap_finished[16];int async_enabled,completing,async_drain_after_submit,slot_publication;_Atomic int recording_active;uint64_t profile_overlap_publications;uint64_t recording_epoch;
    uint64_t profile_slot_wait_ns,profile_drain_wait_ns,profile_completion_wait_ns,profile_completion_waits;
    VBuffer support,zero,default_vertex; VPool pools[LIMIT_POOLS]; int npool;
    PoolUpload pool_uploads[UPLOAD_BUCKETS][UPLOAD_WAYS];uint64_t upload_age;
    int disable_dirty_uploads,enable_writeback_prepare,enable_writeback_coalesce,event_trace;
    uint64_t profile_pool_uploaded_bytes,profile_pool_reused_ranges,profile_pool_reused_bytes;
    VShader shaders[LIMIT_OBJECTS]; int nshader; VTexture textures[LIMIT_OBJECTS]; int ntexture;
    uint32_t texture_used,texture_free_count,texture_free[LIMIT_OBJECTS],texture_table[2*LIMIT_OBJECTS];
    uint64_t texture_incarnation,registry_revision,pool_revision,binding_revision[6],ssbo_revision[6],texbind_revision[6],driver_revision[6][2];DriverCacheEntry driver_cache[6][2];
    int disable_binding_plans,plan_precise_state,enable_command_cache;
    int force_vertex_split;
    int depth_bias_clamp,depth_clamp_available;
    uint64_t profile_cmd_issued[NVN_VK_CMD_STATE_COUNT],profile_cmd_skipped[NVN_VK_CMD_STATE_COUNT];
    NvnDirtyQueryStats profile_dirty_base;
    uint64_t profile_plan_hits,profile_plan_misses,profile_plan_fallbacks,profile_plan_buffers,profile_plan_images,profile_driver_revision_hits;
    uint64_t profile_plan_miss_reason[PLAN_MISS_COUNT];
    TSlot *tslot; size_t texture_slots; SSlot sslot[LIMIT_OBJECTS]; int texture_pool_n;

    VBuffer *retired_buffers[TEMP_POOL_COUNT];uint64_t retired_age[TEMP_POOL_COUNT],retired_bytes,retire_clock;
    unsigned nretired;int disable_temp_pool;

    int disable_driver_table_reuse;

    uint32_t timestamp_bits;int precise_occlusion,disable_query_batch,enable_query_reset_range;
    uint64_t profile_query_reset_calls,profile_query_reset_slots;
    uint64_t profile_occlusion_skipped_ui,profile_occlusion_skipped_shadow;
    unsigned query_skip_mask;
    uint64_t samples_passed;

    CachedPso psos[LIMIT_PSOS];uint32_t pso_table[PSO_TABLE_SIZE],npso;
    uint32_t last_pso_slot;int enable_pso_last;
    int disable_pso_reuse;

    int disable_resource_cache,retain_resources,image_usage_by_flags;
    uint64_t image_usage_omitted,image_usage_storage,image_promotions,image_retired_live;
    uint64_t resource_flushes,resource_kept,resource_views,resource_passes,resource_hits;
    uint64_t profile_view_hits,profile_view_misses,profile_framebuffer_hits,profile_framebuffer_misses;
    int active,feedback_layout,shader_draw_parameters,shader_image_gather_extended,mirror_clamp,null_descriptors,portable_draw_parameters;
    int shader_clip_distance,shader_cull_distance;
    ReplayState state; VkPhysicalDeviceSubgroupProperties subgroup;
    NvnVkGpuProfile profile; NvnVkQuirkRegistry quirks;
    NvnVkPipelineCacheKey pipeline_cache_key;
    uint8_t *shader_index; size_t index_bytes;uint32_t shader_index_version;int shader_code_identity; char shader_root[1024]; ANativeWindow *window;int vi_crop[4];
    ShaderEntry *shader_entries;size_t shader_entry_capacity;ShaderLookup shader_lookup[SHADER_LOOKUP];
    VkSurfaceKHR surface; VkSwapchainKHR swapchain; VkImage *swap_images; VkImageLayout *swap_layouts;
    uint32_t nswap; VkFormat swap_format; VkExtent2D swap_extent; int display_hz;
    VTexture present_encoding_bridge;
    _Atomic uint64_t fence,completed,present_count,frame; uint64_t draws,pass_count,uploads,pool_adds;
    int trace_frame,trace_ui,capture_feedback;_Atomic int window_origin,depth_mode;
    uint64_t profile_pipeline_ns,profile_wait_ns,profile_decode_ns,profile_submits,profile_start_ns,profile_draws,profile_frames;
    /* 卡顿归因：相邻两次出图间隔超阈值时按计数器差值打一行 | Hitch attribution: when the interval between two presents exceeds the threshold, one line is printed from counter deltas */
    uint64_t profile_shader_ns,hitch_last_ns,hitch_pipe,hitch_wait,hitch_decode,hitch_shader,hitch_pso_sync,hitch_pso_wait,hitch_count,hitch_max_ns;double hitch_ema_ms;
    uint64_t profile_t2t_copies,profile_t2t_scaled,profile_t2t_texels;
    uint64_t profile_submit_candidates,profile_candidate_wait_ns,profile_writeback_bytes,profile_events,profile_reports;
    uint64_t profile_writeback_raw_bytes,profile_writeback_raw_ranges,profile_writeback_copy_ranges,profile_writeback_plan_cpu_ns;
    uint64_t profile_write_submits,profile_event_submits,profile_report_submits,profile_trace_submits,profile_cpu_readback_submits,profile_event_wait_submits;

    uint64_t profile_desc_alloc_ns,profile_desc_update_ns,profile_prep_ns,profile_lookup_ns,profile_pool_ns,profile_bind_ns,profile_render_ns,profile_present_ns;unsigned trace_meshes;
    int pool_range_stats,pool_owned_bypass;
    uint64_t profile_owned_reads,profile_owned_bytes;
    struct {uint64_t reads,hits,protects,read_bytes,protect_bytes,uniforms;} pool_ranges[9];
    int disable_shader_stamp,disable_set_recycle,desc_diag;uint64_t profile_shader_stamp_hits;int diag_barrier,diag_loadstore,disable_barrier_elision,disable_sample_copy_cache,barrier_stats,pass_timing,pass_stats,draw_timing,wsi_diag;
    uint64_t pass_stats_total[4],pass_stats_passes,pass_stats_omitted,pass_stats_window_omitted;
    int attachment_defaults,attachment_flags;
    NvnAttachmentStats attachment_stats;
    /* Immutable render passes contain no image/view references. Keep across
     * resource-cache invalidation, following upstream b5a9beb. G.lock owns it. */
    struct {PassKey key;VkRenderPass pass;} render_pass_cache[1024];
    uint64_t render_pass_hits,render_pass_creates;
    int disable_global_pass_cache;
    const uint8_t **attachment_ops;size_t attachment_count,attachment_capacity,attachment_pos;
    int attachment_index_valid;
    uint32_t npacc;

    PassAcc pacc[128];char skip_ps[17];struct{char key[17];uint32_t draws;}hot[64];uint32_t nhot;int disable_desc_cache,disable_control_cache,diag_no_occlusion,diag_img_usage,barrier_pass_elide,pass_side_effects,diag_no_pass_deps,precise_sync,draw_store,xition_vertex;uint64_t profile_wait_elided,profile_hz_barriers,hz_count[8];int hz_diag,chunk_draws,chunk_log,profile_detail,seg_index,diag_skip_draws,diag_skip_xfer,diag_skip_class,diag_shape,enable_narrow_access,diag_fixed_desc,disable_wait_elision,want_cached,wb_stage_enabled,wb_stage_type,disable_setrt_skip,disable_subres,disable_pass_reuse,disable_cpu_uniform_snapshot;uint64_t setrt_total,setrt_same;char pass_desc[96],last_ps[20],last_uses[100];int last_count,last_barrier_bits;uint64_t pass_draws0;uint64_t gts_busy[4],gts_span[4],gts_gap[4],gts_chunks[4],pres_acq_ns,pres_fence_ns,pres_idle_ns,pres_submit_ns,seg_begin_ns,seg_record_ns[4],seg_wait_ns[4],seg_tail_ns[4],seg_draws[4],seg_chunks[4];uint64_t profile_chunks;uint32_t barrier_bits[256],barrier_kept[256],barrier_nopass,barrier_sidefx;uint32_t barrier_sites[4096],pass_end_sites[4096];
    ControlCache *control_cache;
    uint64_t profile_desc_hits,profile_desc_misses,profile_control_hits,profile_control_misses,profile_control_evict,profile_control_changed,profile_barriers_skipped,profile_sample_copy_hits,profile_sample_copy_misses;
    uint64_t profile_passes,profile_barriers,profile_uniform_updates,profile_cpu_snapshots,profile_gpu_copies,profile_snapshot_reuses;char trace_shader[17];
    uint64_t profile_pso_hits,profile_pso_misses;
    uint64_t profile_pso_lookups,profile_pso_hash_calls,profile_pso_hash_ns,profile_pso_lookup_ns;
    uint64_t profile_pso_last_hits,profile_pso_last_misses,profile_pso_table_hits,profile_pso_probes,profile_pso_max_probe;
    uint64_t profile_replay_cpu_ns,profile_begin_cpu_ns,profile_commit_prepare_cpu_ns,profile_commit_wait_cpu_ns,profile_retire_cpu_ns,profile_writeback_cpu_ns,profile_present_cpu_ns;
    uint64_t profile_replay_calls,profile_begin_calls,profile_commit_calls,profile_writeback_calls,profile_present_calls;
    uint64_t profile_submission_creates,profile_submission_reuses,profile_submission_publications;
    uint64_t profile_command_records,profile_command_bytes,profile_command_lease_hits,profile_command_resolves;
    uint64_t profile_writeback_global_faults,profile_writeback_global_preps,profile_writeback_global_mp_calls,profile_writeback_global_mp_pages;
    uint64_t profile_driver_tables,profile_driver_table_reuses,profile_buffer_allocs,profile_buffer_bytes;
    uint64_t profile_temp_reuses,profile_temp_clear_ns,profile_buffer_allocate_ns,profile_temp_bytes[TEMP_ROLES];
    uint64_t profile_storage_bias_bindings,profile_storage_bias_bytes,profile_occlusion_queries;
    unsigned ntrace_textures;uint64_t trace_texture_serials[24];
    pthread_mutex_t lock;
    void *transfer_memory; size_t transfer_size; int graphics_live;
} G={.lock=PTHREAD_MUTEX_INITIALIZER};
static void pipeline_manifest_flush(void);
#include "nvn_vk_pipeline_cache_runtime.inc"
/* Immutable per-device capabilities. Binding the same format should not call
 * into a vendor driver again on every draw. Extension formats remain queried. */
static VkFormatProperties format_properties(VkFormat format) {
    if((uint32_t)format<256&&G.format_known[format])return G.format_properties[format];
    VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(G.physical,format,&properties);
    if((uint32_t)format<256){G.format_properties[format]=properties;G.format_known[format]=1;}
    return properties;
}
/* Recording helpers access the selected slot; completion receives its owner explicitly. */
#define S (*G.current_submission)
#include "nvn_vk_publication_queue.h"
static NvnVkPublicationQueue g_publication=NVN_VK_PUBLICATION_QUEUE_INIT;
static void publication_stage_record(void);
static void publication_enqueue(VkSubmission *sub,int kind);
static void publication_retire_ready_locked(void);
static VkRenderPass render_pass_cached(const PassKey *key,const VkRenderPassCreateInfo *info,uint8_t *owned) {
    uint64_t hash=1469598103934665603ull;const uint8_t *bytes=(const uint8_t *)key;
    for(size_t i=0;i<sizeof *key;i++){hash^=bytes[i];hash*=1099511628211ull;}
    if(!G.disable_global_pass_cache)for(unsigned probe=0;probe<1024;probe++){
        unsigned at=(unsigned)(hash+probe)&1023;
        if(G.render_pass_cache[at].pass){
            if(memcmp(&G.render_pass_cache[at].key,key,sizeof *key))continue;
            *owned=0;G.render_pass_hits++;return G.render_pass_cache[at].pass;
        }
        VkRenderPass pass;CHECK(vkCreateRenderPass(G.device,info,NULL,&pass));
        G.render_pass_cache[at].key=*key;G.render_pass_cache[at].pass=pass;*owned=0;G.render_pass_creates++;return pass;
    }
    VkRenderPass pass;CHECK(vkCreateRenderPass(G.device,info,NULL,&pass));*owned=1;G.render_pass_creates++;return pass;
}
#include "nvn_vk_resource_runtime.inc"
typedef struct {int on,dim;float origin[2],dest[2],grid[4];uint64_t serial,token;const _Atomic uint64_t *live;} GhostRequest;
typedef struct {Range range;VPool pool;} GhostBuffer;
typedef struct {unsigned id,sid;uint64_t revision,serial,incarnation,version;VkSampler sampler;} GhostTexture;
typedef struct {ReplayState state;p_draw draw;uint64_t tex;float rect[4];unsigned nb,nt;uint64_t bytes;GhostBuffer buffer[34];GhostTexture texture[16];} GhostCandidate;
static pthread_mutex_t ghost_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic int ghost_on;static GhostRequest ghost_request;static uint64_t ghost_serial,ghost_found_serial;
static float ghost_found[4];static int ghost_found_valid;
static struct {uint64_t pass,epoch,serial;unsigned n,nt;int busy;float area;GhostCandidate *c[4],*replaying;uint64_t texture[128];unsigned count[128];} ghost;
static VPool *ghost_pool_override(uint64_t,uint64_t,uint64_t *);
static void ghost_flush(void);
static void ghost_note(const p_draw *,const p_draw_idx *,int,int,VShader *,VShader *);
static void command_state_profile(const char *tag,uint64_t frame,uint64_t frames) {
    uint64_t issued=0,skipped=0;
    for(unsigned i=0;i<NVN_VK_CMD_STATE_COUNT;i++){issued+=G.profile_cmd_issued[i];skipped+=G.profile_cmd_skipped[i];}
    fprintf(stderr,"[nvn-vulkan] %s frame=%llu frames=%llu enabled=%d trace_bypass=%d issued=%llu skipped=%llu pipeline_issued=%llu pipeline_skipped=%llu descriptors_issued=%llu descriptors_skipped=%llu viewport_issued=%llu viewport_skipped=%llu scissor_issued=%llu scissor_skipped=%llu blend_issued=%llu blend_skipped=%llu vertex_issued=%llu vertex_skipped=%llu index_issued=%llu index_skipped=%llu\n",
        tag,(unsigned long long)frame,(unsigned long long)frames,G.enable_command_cache,G.trace_frame,
        (unsigned long long)issued,(unsigned long long)skipped,
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_PIPELINE],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_PIPELINE],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_DESCRIPTORS],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_DESCRIPTORS],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_VIEWPORT],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_VIEWPORT],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_SCISSOR],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_SCISSOR],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_BLEND],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_BLEND],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_VERTEX],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_VERTEX],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_INDEX],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_INDEX]);
}
static void dirty_query_profile(const char *tag,uint64_t frame,uint64_t frames,int window) {
    NvnDirtyQueryStats stats,current;int enabled,recording;
    nvn_dirty_query_stats(&current,&enabled,&recording);stats=current;
    if(window){
        stats.calls-=G.profile_dirty_base.calls;stats.page_reads-=G.profile_dirty_base.page_reads;
        stats.block_reads-=G.profile_dirty_base.block_reads;stats.skipped_pages-=G.profile_dirty_base.skipped_pages;
        stats.range_fast_paths-=G.profile_dirty_base.range_fast_paths;G.profile_dirty_base=current;
        if(frames<60)return;
    }
    fprintf(stderr,"[nvn-vulkan] %s frame=%llu frames=%llu enabled=%d recording=%d scope=process calls=%llu page_reads=%llu block_reads=%llu skipped_pages=%llu range_fast_paths=%llu block_pages=%u\n",
        tag,(unsigned long long)frame,(unsigned long long)frames,enabled,recording,(unsigned long long)stats.calls,
        (unsigned long long)stats.page_reads,(unsigned long long)stats.block_reads,(unsigned long long)stats.skipped_pages,
        (unsigned long long)stats.range_fast_paths,NVN_DIRTY_BLOCK_PAGES);
}
static void binding_plan_profile(uint64_t frame) {
    const uint64_t *r=G.profile_plan_miss_reason;
    fprintf(stderr,"[nvn-vulkan] RENDER_BINDING_PLAN_REASON frame=%llu no_plan=%llu disabled=%llu trace=%llu ineligible=%llu unresolved=%llu epoch=%llu state_revision=%llu registry_revision=%llu pool_revision=%llu buffer_range=%llu buffer_alignment=%llu buffer_pool=%llu buffer_offset=%llu image_invalid=%llu image_identity=%llu image_view=%llu image_disjoint=%llu image_layout=%llu sampler=%llu\n",
        (unsigned long long)frame,
        (unsigned long long)r[PLAN_MISS_NO_PLAN],(unsigned long long)r[PLAN_MISS_DISABLED],
        (unsigned long long)r[PLAN_MISS_TRACE],(unsigned long long)r[PLAN_MISS_INELIGIBLE],
        (unsigned long long)r[PLAN_MISS_UNRESOLVED],(unsigned long long)r[PLAN_MISS_EPOCH],
        (unsigned long long)r[PLAN_MISS_STATE_REVISION],(unsigned long long)r[PLAN_MISS_REGISTRY_REVISION],
        (unsigned long long)r[PLAN_MISS_POOL_REVISION],(unsigned long long)r[PLAN_MISS_BUFFER_RANGE],
        (unsigned long long)r[PLAN_MISS_BUFFER_ALIGNMENT],(unsigned long long)r[PLAN_MISS_BUFFER_POOL],
        (unsigned long long)r[PLAN_MISS_BUFFER_OFFSET],(unsigned long long)r[PLAN_MISS_IMAGE_INVALID],
        (unsigned long long)r[PLAN_MISS_IMAGE_IDENTITY],(unsigned long long)r[PLAN_MISS_IMAGE_VIEW],
        (unsigned long long)r[PLAN_MISS_IMAGE_DISJOINT],(unsigned long long)r[PLAN_MISS_IMAGE_LAYOUT],
        (unsigned long long)r[PLAN_MISS_SAMPLER]);
}
static void binding_plan_mode_poll(uint64_t frame) {
    if(frame%60)return;
    const char *root=getenv("DM_FRAME_DIR");char path[1400];
    if(!root||snprintf(path,sizeof path,"%s/planstate.txt",root)>=(int)sizeof path)return;
    FILE *f=fopen(path,"r");if(!f)return;
    int mode=fgetc(f),tail=fgetc(f);fclose(f);
    if((mode=='0'||mode=='1')&&(tail==EOF||tail=='\n')&&
       G.plan_precise_state!=(mode=='1')){
        G.plan_precise_state=mode=='1';
        fprintf(stderr,"[nvn-vulkan] PLAN_STATE mode=%d (%s)\n",G.plan_precise_state,
            G.plan_precise_state?"per-binding validation":"coarse state revision");
    }
}
static void query_skip_mode_poll(uint64_t frame) {
    if(frame%60)return;
    const char *root=getenv("DM_FRAME_DIR");char path[1400];
    if(!root||snprintf(path,sizeof path,"%s/query-mode.txt",root)>=(int)sizeof path)return;
    FILE *f=fopen(path,"r");if(!f)return;
    int mode=fgetc(f),tail=fgetc(f);fclose(f);
    if(mode>='0'&&mode<='3'&&(tail==EOF||tail=='\n')&&
       G.query_skip_mask!=(unsigned)(mode-'0')){
        G.query_skip_mask=(unsigned)(mode-'0');
        fprintf(stderr,"[nvn-vulkan] QUERY_SKIP mask=%u ui=%u shadow=%u\n",G.query_skip_mask,
            !!(G.query_skip_mask&1),!!(G.query_skip_mask&2));
    }
}
static pthread_once_t init_once=PTHREAD_ONCE_INIT;
/* Thread CPU clocks must stay on their originating thread. Replay is inclusive
 * of event-wait commit/begin cuts; present includes its own sub_begin, and
 * writeback is a retire subset. These phase totals must not be summed blindly. */
static _Thread_local unsigned replay_cpu_depth,present_cpu_depth;
static _Thread_local uint64_t replay_cpu_started,present_cpu_started;
static VkDebugUtilsMessengerEXT validation_messenger;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;
    static struct {int32_t id;unsigned count;} seen[128];static unsigned count;
    unsigned i=0;while(i<count && seen[i].id!=data->messageIdNumber)i++;
    if(i==count){if(count==128)return VK_FALSE;seen[count++].id=data->messageIdNumber;}
    if(seen[i].count++<10)fprintf(stderr,"[nvn-vulkan] VALIDATION severity=%#x draw=%llu id=%s message=%s\n",severity,(unsigned long long)G.draws,data->pMessageIdName?data->pMessageIdName:"unknown",data->pMessage);
    return VK_FALSE;
}

/* 计数器每 60 帧清零，差值遇到回绕时取当前值 | The counters are zeroed every 60 frames; on wrap-around the delta is the current value */
#define HITCH_DELTA(cur,saved) ((cur)>=(saved)?(cur)-(saved):(cur))
static inline uint64_t prof_now(void) { return G.profile_detail?dm_android_nvn_clock_ns():0; }
static uint32_t memory_type(uint32_t mask,VkMemoryPropertyFlags properties) {
    for(uint32_t i=0;i<G.memory.memoryTypeCount;i++) if((mask&(1u<<i)) &&
        (G.memory.memoryTypes[i].propertyFlags&properties)==properties) return i;
    die("memory type",properties);
}
/* A separate short-held ledger mutex also covers startup allocations, before
 * the recorder starts owning G.lock. No driver call runs under this mutex. */
static pthread_mutex_t memory_mu=PTHREAD_MUTEX_INITIALIZER;
static DmVkMemoryStats memory_stats={.abi_version=1,.class_count=DM_VK_MEMORY_CLASS_COUNT};
static VkDeviceMemory memory_allocate(const VkMemoryAllocateInfo *info,unsigned kind) {
    if(kind>=DM_VK_MEMORY_CLASS_COUNT)die("allocation class",kind);
    VkDeviceMemory memory;CHECK(vkAllocateMemory(G.device,info,NULL,&memory));
    pthread_mutex_lock(&memory_mu);
    uint64_t size=info->allocationSize;
    if(size>UINT64_MAX-memory_stats.total_bytes)die("allocation ledger overflow",size);
    memory_stats.bytes[kind]+=size;memory_stats.allocations[kind]++;memory_stats.total_bytes+=size;
    if(memory_stats.bytes[kind]>memory_stats.peak_bytes[kind])memory_stats.peak_bytes[kind]=memory_stats.bytes[kind];
    if(memory_stats.total_bytes>memory_stats.peak_total_bytes)memory_stats.peak_total_bytes=memory_stats.total_bytes;
    pthread_mutex_unlock(&memory_mu);return memory;
}
static void memory_release(VkDeviceMemory memory,uint64_t size,unsigned kind) {
    if(!memory)return;
    if(kind>=DM_VK_MEMORY_CLASS_COUNT)die("release class",kind);
    vkFreeMemory(G.device,memory,NULL);
    pthread_mutex_lock(&memory_mu);
    if(!memory_stats.allocations[kind]||size>memory_stats.bytes[kind]||size>memory_stats.total_bytes)die("allocation ledger underflow",size);
    memory_stats.bytes[kind]-=size;memory_stats.allocations[kind]--;memory_stats.total_bytes-=size;
    pthread_mutex_unlock(&memory_mu);
}
static void texture_snapshot_set(VTexture *t,uint8_t *snapshot,uint64_t size) {
    pthread_mutex_lock(&memory_mu);
    if(t->cpu_snapshot_size>memory_stats.cpu_baseline_bytes)die("CPU baseline ledger underflow",t->cpu_snapshot_size);
    memory_stats.cpu_baseline_bytes-=t->cpu_snapshot_size;memory_stats.cpu_baseline_bytes+=size;
    if(memory_stats.cpu_baseline_bytes>memory_stats.cpu_baseline_peak_bytes)memory_stats.cpu_baseline_peak_bytes=memory_stats.cpu_baseline_bytes;
    pthread_mutex_unlock(&memory_mu);
    free(t->cpu_snapshot);t->cpu_snapshot=snapshot;t->cpu_snapshot_size=size;
}
static VBuffer buffer_create_kind(uint64_t size,VkBufferUsageFlags usage,int zero,unsigned kind) {
    uint64_t started=prof_now();VBuffer b={.size=size,.usage=usage,.memory_class=kind}; if(!size || size>SIZE_MAX) die("buffer size",size);
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE}; CHECK(vkCreateBuffer(G.device,&info,NULL,&b.b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(G.device,b.b,&r);
    b.allocation_size=r.size;
    VkMemoryAllocateInfo a={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,
        .memoryTypeIndex=memory_type(r.memoryTypeBits,G.want_cached?(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT):(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))};
    b.cached=G.want_cached&&!(G.memory.memoryTypes[a.memoryTypeIndex].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    b.m=memory_allocate(&a,kind); CHECK(vkBindBufferMemory(G.device,b.b,b.m,0));
    void *mapped=NULL; CHECK(vkMapMemory(G.device,b.m,0,size,0,&mapped)); b.map=mapped;if(zero)memset(b.map,0,size);
    G.profile_buffer_allocs++;G.profile_buffer_bytes+=size;G.profile_buffer_allocate_ns+=prof_now()-started;return b;
}
static VBuffer buffer_create(uint64_t size,VkBufferUsageFlags usage,int zero) {return buffer_create_kind(size,usage,zero,DM_VK_MEMORY_SUPPORT);}
static VBuffer buffer_make(uint64_t size,VkBufferUsageFlags usage) { return buffer_create(size,usage,1); }
static void buffer_free(VBuffer *b) {
    if(b->map) vkUnmapMemory(G.device,b->m); if(b->b) vkDestroyBuffer(G.device,b->b,NULL);
    memory_release(b->m,b->allocation_size,b->memory_class); memset(b,0,sizeof(*b));
}
static VBuffer *retired_take(unsigned index) {
    VBuffer *b=G.retired_buffers[index];G.retired_bytes-=b->allocation_size;
    unsigned last=--G.nretired;G.retired_buffers[index]=G.retired_buffers[last];G.retired_age[index]=G.retired_age[last];return b;
}
static void temporaries_retire(VkSubmission *sub) {
    /* Called only after fence completion and CPU writeback/capture. Entries
     * taken for a new submission are removed from this pool, so no live
     * command or another binding can receive the same mutable allocation. */
    for(int i=0;i<sub->ntemporary;i++){
        VBuffer *b=sub->temporaries[i];
        if(G.disable_temp_pool||b->allocation_size>TEMP_POOL_BYTES){buffer_free(b);free(b);continue;}
        while(G.nretired&&(G.nretired==TEMP_POOL_COUNT||G.retired_bytes>TEMP_POOL_BYTES-b->allocation_size)){
            unsigned oldest=0;for(unsigned j=1;j<G.nretired;j++)if(G.retired_age[j]<G.retired_age[oldest])oldest=j;
            VBuffer *old=retired_take(oldest);buffer_free(old);free(old);
        }
        unsigned slot=G.nretired++;G.retired_buffers[slot]=b;G.retired_age[slot]=++G.retire_clock;G.retired_bytes+=b->allocation_size;
    }
    sub->ntemporary=0;
}
static VBuffer *temporary_named(uint64_t size,VkBufferUsageFlags usage,unsigned role) {
    if(role>=TEMP_ROLES)die("temporary buffer role",role);G.profile_temp_bytes[role]+=size;
    if((size_t)S.ntemporary==S.temporary_capacity){size_t count=S.temporary_capacity?S.temporary_capacity*2:256;
        VBuffer **entries=realloc(S.temporaries,count*sizeof(*entries));if(!entries)die("temporary buffer registry allocation",count);S.temporaries=entries;S.temporary_capacity=count;}
    VBuffer *b=NULL;
    if(!G.disable_temp_pool)for(unsigned i=0;i<G.nretired;i++)if(G.retired_buffers[i]->size==size&&G.retired_buffers[i]->usage==usage){
        b=retired_take(i);G.profile_temp_reuses++;uint64_t started=prof_now();
        /* Preserve the original zero-initialized semantics, including bytes
         * outside a partial upload and the tails of driver constant banks. */
        /* Bytes at or beyond b->dirty are still zero from the previous use. */
        memset(b->map,0,b->dirty<size?b->dirty:size);G.profile_temp_clear_ns+=prof_now()-started;break;
    }
    if(!b){b=malloc(sizeof(*b));if(!b)die("temporary buffer allocation",size);*b=buffer_create_kind(size,usage,1,DM_VK_MEMORY_TEMP);}
    b->dirty=role==TEMP_DRIVER?DRIVER_TABLE_BYTES:size;
    S.temporaries[S.ntemporary++]=b;return b;
}
static VBuffer *temporary(uint64_t size,VkBufferUsageFlags usage) {return temporary_named(size,usage,TEMP_OTHER);}
static void trace_capture_queue(VBuffer *buffer,uint64_t bytes,const char *name) {
    const char *root=getenv("DM_FRAME_DIR");if(!root)return;
    if(S.ntrace_captures==512)die("GPU trace capture capacity",512);
    TraceCapture *c=&S.trace_captures[S.ntrace_captures++];c->buffer=buffer;c->bytes=bytes;c->offset=0;
    snprintf(c->path,sizeof(c->path),"%s/%s.bin",root,name);
}
static void trace_capture_complete(VkSubmission *sub) {
    for(unsigned i=0;i<sub->ntrace_captures;i++){TraceCapture *c=&sub->trace_captures[i];FILE *out=fopen(c->path,"wb");if(!out)die("GPU trace output",0);
        if(fwrite(c->buffer->map+c->offset,1,c->bytes,out)!=c->bytes)die("GPU trace write",c->bytes);fclose(out);
        fprintf(stderr,"[nvn-vulkan] GPU_TRACE_CAPTURE bytes=%llu path=%s\n",(unsigned long long)c->bytes,c->path);
    }
    sub->ntrace_captures=0;
}
static int g_cov_table,g_cov_probe,g_cov_selftest,g_cov_store_skip,g_cov_traffic_on;
static uint64_t g_cov_budget=16u*1024u*1024u,g_cov_frame=UINT64_MAX,g_cov_frame_bytes;
static void cov_load(void);
static void cov_probe_end(void);
static void cov_probe_complete(VkSubmission *sub);
static void cov_frame_report(uint64_t frame);
void dm_vk_set_shader_root(const char *path) {
    if(!path || strlen(path)>=sizeof(G.shader_root) || G.nshader) die("shader root change",0);
    strcpy(G.shader_root,path);
}
void mtl_set_window_origin_mode(int mode) {
    if(mode!=NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT&&mode!=NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT)die("NVN window origin mode",mode);
    atomic_store(&G.window_origin,mode);
    fprintf(stderr,"[nvn-vulkan] WINDOW_ORIGIN mode=%d negative_viewport=%d\n",mode,mode==NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
}
static void pipeline_cache_init(void);
static void pipeline_worker_start(void);
static void pipeline_frame(uint64_t frame);
void dm_vk_set_depth_mode(int mode) {
    if(mode!=NVN_DEPTH_MODE_NEAR_IS_MINUS_W&&mode!=NVN_DEPTH_MODE_NEAR_IS_ZERO)die("NVN depth mode",mode);
    atomic_store(&G.depth_mode,mode);
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
    PFN_vkEnumerateInstanceVersion instance_version=(PFN_vkEnumerateInstanceVersion)dm_vk_get(NULL,"vkEnumerateInstanceVersion");
    uint32_t loader_api=VK_API_VERSION_1_0;
    if(instance_version)CHECK(instance_version(&loader_api));
    if(loader_api<VK_API_VERSION_1_1)die("Vulkan 1.1 loader required",loader_api);
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
    if(G.properties.apiVersion<VK_API_VERSION_1_1)die("Vulkan 1.1 device required",G.properties.apiVersion);
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions=(PFN_vkEnumerateDeviceExtensionProperties)dm_vk_get(G.instance,"vkEnumerateDeviceExtensionProperties");
    uint32_t extension_count=0;CHECK(enumerate_extensions(G.physical,NULL,&extension_count,NULL));
    VkExtensionProperties *extension_properties=calloc(extension_count?extension_count:1,sizeof(*extension_properties));
    if(!extension_properties)die("device extension allocation",extension_count);
    CHECK(enumerate_extensions(G.physical,NULL,&extension_count,extension_properties));
    int has_driver_properties=G.properties.apiVersion>=VK_API_VERSION_1_2;
    int has_feedback=0,has_performance_query=0,has_swapchain=0,has_robustness2=0,has_draw_parameters=0,has_depth_order=0;
    for(uint32_t i=0;i<extension_count;i++){
        const char *name=extension_properties[i].extensionName;
        has_driver_properties|=!strcmp(name,VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
        has_feedback|=!strcmp(name,VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
        has_performance_query|=!strcmp(name,"VK_KHR_performance_query");
        has_swapchain|=!strcmp(name,VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        G.mirror_clamp|=!strcmp(name,VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
        has_robustness2|=!strcmp(name,VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        has_draw_parameters|=!strcmp(name,VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME);
        has_depth_order|=!strcmp(name,VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);
        G.filter_cubic_ext|=!strcmp(name,VK_EXT_FILTER_CUBIC_EXTENSION_NAME);
    }
    {char line[6144];int n=0;int host=0;for(uint32_t i=0;i<extension_count&&n<(int)sizeof line-80;i++){const char *name=extension_properties[i].extensionName;host|=!strcmp(name,"VK_EXT_external_memory_host");n+=snprintf(line+n,sizeof line-n,"%s ",name);}
     fprintf(stderr,"[nvn-vulkan] DEVICE_EXTENSIONS count=%u external_memory_host=%d %s\n",extension_count,host,line);}
    free(extension_properties);
    if(!has_swapchain)die("Vulkan swapchain extension required",0);
    for(uint32_t i=0;i<G.memory.memoryTypeCount;i++)fprintf(stderr,"[nvn-vulkan] MEMORY_TYPE %u flags=%#x heap=%u size=%llu\n",i,G.memory.memoryTypes[i].propertyFlags,G.memory.memoryTypes[i].heapIndex,(unsigned long long)G.memory.memoryHeaps[G.memory.memoryTypes[i].heapIndex].size);
    G.subgroup=(VkPhysicalDeviceSubgroupProperties){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver_properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,.pNext=&G.subgroup};
    VkPhysicalDeviceProperties2 properties2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext=has_driver_properties?(void *)&driver_properties:(void *)&G.subgroup};
    vkGetPhysicalDeviceProperties2(G.physical,&properties2);
    G.profile=nvn_vk_gpu_profile_make(&G.properties,&driver_properties,&G.subgroup);
    G.quirks=nvn_vk_quirk_registry_resolve(&G.profile);
    G.pipeline_cache_key=nvn_vk_pipeline_cache_key_make(&G.properties,&G.profile,&G.quirks);
    if(private_driver && !strcmp(private_driver,"turnip") && driver_properties.driverID!=VK_DRIVER_ID_MESA_TURNIP)die("requested private driver was not loaded",driver_properties.driverID);
    fprintf(stderr,"[nvn-vulkan] driver id=%u name=%s info=%s\n",driver_properties.driverID,driver_properties.driverName,driver_properties.driverInfo);
    G.depth_input_experiment=getenv("DM_DEPTH_INPUT_EXPERIMENT")!=NULL;
    G.depth_input_profile=driver_properties.driverID==VK_DRIVER_ID_ARM_PROPRIETARY&&
        G.properties.vendorID==5045u&&G.properties.deviceID==2825322496u&&G.properties.driverVersion==159387648u&&
        !strcmp(G.properties.deviceName,"Mali-G610 MC6")&&
        !strcmp(driver_properties.driverInfo,"v1.r38p1-01eac0.91ad9bb3ee6ee6b8afda80c5afcd3137")&&
        G.properties.limits.subTexelPrecisionBits==8&&G.properties.limits.subPixelPrecisionBits==8;
    /* Separate opt-in for finite exact-boundary calibration on this driver.
     * Neither marker can authorize another device or a wider sample interval. */
    G.depth_input_calibrated_ties=G.depth_input_experiment&&G.depth_input_profile&&getenv("DM_DEPTH_INPUT_CALIBRATED_TIES")!=NULL;
    fprintf(stderr,"[nvn-vulkan] subgroup size=%u stages=%#x operations=%#x\n",G.subgroup.subgroupSize,G.subgroup.supportedStages,G.subgroup.supportedOperations);
    fprintf(stderr,"[nvn-vulkan] render tier=%u quirks=%#x\n",(unsigned)G.quirks.render_tier,G.quirks.flags);
    fprintf(stderr,"[nvn-vulkan] pipeline cache identity version=%u uuid=%02x%02x%02x%02x quirks=%#x\n",
        G.pipeline_cache_key.version,G.pipeline_cache_key.pipeline_cache_uuid[0],
        G.pipeline_cache_key.pipeline_cache_uuid[1],G.pipeline_cache_key.pipeline_cache_uuid[2],
        G.pipeline_cache_key.pipeline_cache_uuid[3],G.pipeline_cache_key.quirk_flags);
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
    if(getenv("DM_VK_AB_CONTROL"))fprintf(stderr,"[nvn-vulkan] ATTACHMENT_AB_CAPS performance_query=%d timing=cpu_present_only bandwidth=not_measured\n",has_performance_query);
    const char *de[6]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};uint32_t enabled_extensions=1;
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesARM depth_order={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_ARM};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,.pNext=has_depth_order?&depth_order:NULL};
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,.pNext=has_feedback?(void *)&feedback:has_depth_order?(void *)&depth_order:NULL};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,.pNext=&draw_parameters};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=has_robustness2?(void *)&robustness2:(void *)&draw_parameters};
    PFN_vkGetPhysicalDeviceFeatures2 get_features=(PFN_vkGetPhysicalDeviceFeatures2)dm_vk_get(G.instance,"vkGetPhysicalDeviceFeatures2");get_features(G.physical,&features);
    G.feedback_layout=feedback.attachmentFeedbackLoopLayout;
    if(G.feedback_layout)de[enabled_extensions++]=VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
    if(G.mirror_clamp)de[enabled_extensions++]=VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME;
    {const char *pf=getenv("DM_PRESENT_FILTER");G.present_cubic=G.filter_cubic_ext&&!(pf&&!strcmp(pf,"linear")); /* cubic by default; "linear" opts out */
     if(G.present_cubic)de[enabled_extensions++]=VK_EXT_FILTER_CUBIC_EXTENSION_NAME;
     fprintf(stderr,"[nvn-vulkan] PRESENT_FILTER filter=%s cubic_extension=%d enabled=%d\n",pf?pf:"default",G.filter_cubic_ext,G.present_cubic);}
    if(has_draw_parameters)de[enabled_extensions++]=VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME;
    G.null_descriptors=robustness2.nullDescriptor;
    robustness2.robustBufferAccess2=robustness2.robustImageAccess2=VK_FALSE;
    if(G.null_descriptors)de[enabled_extensions++]=VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    G.shader_draw_parameters=draw_parameters.shaderDrawParameters;
    G.portable_draw_parameters=getenv("DM_PORTABLE_DRAW_PARAMETERS")!=NULL;
    G.depth_order=has_depth_order&&depth_order.rasterizationOrderDepthAttachmentAccess&&G.depth_input_experiment&&G.depth_input_profile;
    depth_order.rasterizationOrderColorAttachmentAccess=depth_order.rasterizationOrderStencilAttachmentAccess=VK_FALSE;
    depth_order.rasterizationOrderDepthAttachmentAccess=G.depth_order;
    if(G.depth_order)de[enabled_extensions++]=VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
    feedback.pNext=G.depth_order?&depth_order:NULL;
    draw_parameters.pNext=G.feedback_layout?(void *)&feedback:G.depth_order?(void *)&depth_order:NULL;
    VkPhysicalDeviceFeatures available,enabled={0}; vkGetPhysicalDeviceFeatures(G.physical,&available);
    enabled.shaderStorageImageReadWithoutFormat=available.shaderStorageImageReadWithoutFormat;
    enabled.shaderStorageImageWriteWithoutFormat=available.shaderStorageImageWriteWithoutFormat;
    enabled.shaderImageGatherExtended=available.shaderImageGatherExtended;
    G.shader_image_gather_extended=enabled.shaderImageGatherExtended;
    G.shader_code_identity=getenv("DM_SHADER_IDENTITY_CODE")!=NULL;
    {VkFormatProperties bc1,bc3,bc7,astc,etc2;vkGetPhysicalDeviceFormatProperties(G.physical,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,&bc1);vkGetPhysicalDeviceFormatProperties(G.physical,VK_FORMAT_BC3_UNORM_BLOCK,&bc3);
     vkGetPhysicalDeviceFormatProperties(G.physical,VK_FORMAT_BC7_UNORM_BLOCK,&bc7);vkGetPhysicalDeviceFormatProperties(G.physical,VK_FORMAT_ASTC_4x4_UNORM_BLOCK,&astc);vkGetPhysicalDeviceFormatProperties(G.physical,VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,&etc2);
     fprintf(stderr,"[nvn-vulkan] TEXTURE_COMPRESSION feature_bc=%u feature_astc_ldr=%u feature_etc2=%u bc1=%#x bc3=%#x bc7=%#x astc4x4=%#x etc2=%#x\n",available.textureCompressionBC,available.textureCompressionASTC_LDR,available.textureCompressionETC2,
         (unsigned)bc1.optimalTilingFeatures,(unsigned)bc3.optimalTilingFeatures,(unsigned)bc7.optimalTilingFeatures,(unsigned)astc.optimalTilingFeatures,(unsigned)etc2.optimalTilingFeatures);}
    enabled.samplerAnisotropy=available.samplerAnisotropy;
    enabled.depthBiasClamp=available.depthBiasClamp;G.depth_bias_clamp=enabled.depthBiasClamp;
    enabled.depthClamp=available.depthClamp;G.depth_clamp_available=enabled.depthClamp;
    enabled.shaderClipDistance=available.shaderClipDistance;G.shader_clip_distance=enabled.shaderClipDistance;
    enabled.shaderCullDistance=available.shaderCullDistance;G.shader_cull_distance=enabled.shaderCullDistance;
    enabled.imageCubeArray=available.imageCubeArray;
    enabled.occlusionQueryPrecise=available.occlusionQueryPrecise;
    enabled.depthBiasClamp=available.depthBiasClamp;
    fprintf(stderr,"[nvn-vulkan] DEPTH_BIAS_CAPS clamp_supported=%u clamp_enabled=%u\n",available.depthBiasClamp,enabled.depthBiasClamp);
    if(!available.independentBlend)die("NVN independent attachment blending unavailable",0);
    /* Experimental until device validation. Never enable a feature solely on
     * the GPU name: use the feature bit and per-format/image queries. */
    /* DM_BC_PASSTHROUGH: 1 = force on, 0 = force off, unset = automatic: on for Adreno 8xx (validated on Adreno 830, removes the CPU BC decode),
     * off elsewhere until each GPU class has been checked in game | 1 强制开，0 强制关，未设置时自动：Adreno 8xx 开（Adreno 830 已验证，省掉 CPU 解码），其余未验证的 GPU 保持关 */
    const char *native_bc=getenv("DM_BC_PASSTHROUGH");
    int bc_auto=0;{int adreno=0;const char *a=strstr(G.properties.deviceName,"Adreno (TM) ");if(a&&sscanf(a+12,"%d",&adreno)==1&&adreno>=800)bc_auto=1;}
    int bc_want=native_bc&&*native_bc?!strcmp(native_bc,"1"):bc_auto;
    enabled.textureCompressionBC=available.textureCompressionBC&&bc_want;
    g_bc_passthrough=enabled.textureCompressionBC;
    const char *batch=getenv("DM_TEXTURE_UPLOAD_BATCH");
    g_texture_upload_batch=!batch||strcmp(batch,"0");
    fprintf(stderr,"[nvn-vulkan] TEXTURE_UPLOAD batch=%d bc_requested=%d bc_enabled=%d\n",
        g_texture_upload_batch,bc_want,g_bc_passthrough);
    enabled.independentBlend=VK_TRUE;
    enabled.fragmentStoresAndAtomics=available.fragmentStoresAndAtomics;
    G.fragment_stores=enabled.fragmentStoresAndAtomics;
    enabled.vertexPipelineStoresAndAtomics=available.vertexPipelineStoresAndAtomics;
    enabled.shaderClipDistance=available.shaderClipDistance;
    enabled.shaderCullDistance=available.shaderCullDistance;
    /* GPU-authored NVN indices can use all 32 bits, even when a negative base
     * vertex brings the effective vertex back into a small bound stream. */
    enabled.fullDrawIndexUint32=available.fullDrawIndexUint32;
    fprintf(stderr,"[nvn-vulkan] INDEX_CAPS full_uint32_supported=%u full_uint32_enabled=%u max_device_index=%u\n",
        available.fullDrawIndexUint32,enabled.fullDrawIndexUint32,
        G.properties.limits.maxDrawIndexedIndexValue);
    G.pass_stats=getenv("DM_PASS_STATS") && !strcmp(getenv("DM_PASS_STATS"),"1") && available.pipelineStatisticsQuery;
    enabled.pipelineStatisticsQuery=G.pass_stats;
    fprintf(stderr,"[nvn-vulkan] PASS_STATS_CAPS requested=%d supported=%u enabled=%d\n",
        getenv("DM_PASS_STATS") && !strcmp(getenv("DM_PASS_STATS"),"1"),available.pipelineStatisticsQuery,G.pass_stats);
    G.enabled_features=enabled;
    const VkBool32 cache_features[]={enabled.shaderStorageImageReadWithoutFormat,enabled.shaderStorageImageWriteWithoutFormat,
        enabled.shaderImageGatherExtended,enabled.samplerAnisotropy,enabled.imageCubeArray,enabled.occlusionQueryPrecise,
        enabled.independentBlend,enabled.fragmentStoresAndAtomics,enabled.vertexPipelineStoresAndAtomics,
        enabled.shaderClipDistance,enabled.shaderCullDistance,draw_parameters.shaderDrawParameters,G.feedback_layout,
        enabled.pipelineStatisticsQuery,enabled.fullDrawIndexUint32,enabled.depthBiasClamp,enabled.depthClamp,
        G.null_descriptors,G.portable_draw_parameters,G.depth_order,G.depth_input_calibrated_ties,enabled.textureCompressionBC};
    for(unsigned i=0;i<sizeof cache_features/sizeof cache_features[0];i++)
        if(cache_features[i])G.pipeline_cache_key.enabled_features|=1u<<i;
    G.precise_occlusion=enabled.occlusionQueryPrecise;
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,NULL);
    families=calloc(n,sizeof(*families));if(!families)die("timestamp queue properties",n);
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,families);G.timestamp_bits=families[G.family].timestampValidBits;free(families);
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
       .enabledExtensionCount=enabled_extensions,.ppEnabledExtensionNames=de,.pEnabledFeatures=&enabled,.pNext=G.null_descriptors?(void *)&robustness2:(void *)&draw_parameters}; CHECK(vkCreateDevice(G.physical,&di,NULL,&G.device));
    fprintf(stderr,"[nvn-vulkan] null_descriptors=%d\n",G.null_descriptors);
    fprintf(stderr,"[nvn-vulkan] shader_clip_distance=%d shader_cull_distance=%d\n",G.shader_clip_distance,G.shader_cull_distance);
    fprintf(stderr,"[nvn-vulkan] shader_draw_parameters=%d\n",G.shader_draw_parameters);
    fprintf(stderr,"[nvn-vulkan] attachment_feedback_layout=%d independent_blend=%u fragment_stores=%u\n",G.feedback_layout,enabled.independentBlend,enabled.fragmentStoresAndAtomics);
    fprintf(stderr,"[nvn-vulkan] DEPTH_INPUT_EXPERIMENT requested=%d profile=%d depth_order=%d subtexel=%u subpixel=%u calibrated_ties=%d\n",G.depth_input_experiment,G.depth_input_profile,G.depth_order,G.properties.limits.subTexelPrecisionBits,G.properties.limits.subPixelPrecisionBits,G.depth_input_calibrated_ties);
    vkGetDeviceQueue(G.device,G.family,0,&G.queue);
    pipeline_cache_init();
    fprintf(stderr,"[nvn-vulkan] driver_pipeline_cache=%d\n",G.pipeline_cache!=VK_NULL_HANDLE);
    G.disable_pso_reuse=getenv("DM_DISABLE_PSO_REUSE")!=NULL;
    {const char *last=getenv("DM_PSO_LAST_CACHE");G.enable_pso_last=last&&atoi(last)!=0;G.last_pso_slot=UINT32_MAX;}
    G.disable_driver_table_reuse=getenv("DM_DISABLE_DRIVER_TABLE_REUSE")!=NULL;
    G.disable_temp_pool=getenv("DM_DISABLE_TEMP_POOL")!=NULL;
    {const char *force=getenv("DM_VK_VERTEX_SPLIT");G.force_vertex_split=force&&!strcmp(force,"1");}
    G.disable_query_batch=getenv("DM_DISABLE_QUERY_BATCH")!=NULL;G.diag_no_occlusion=getenv("DM_DIAG_NO_OCCLUSION")!=NULL;G.barrier_pass_elide=getenv("DM_BARRIER_PASS_ELIDE")!=NULL;G.diag_no_pass_deps=getenv("DM_DIAG_NO_PASS_DEPS")!=NULL;G.precise_sync=getenv("DM_DISABLE_PRECISE_SYNC")==NULL;{const char *hd=getenv("DM_HZ_DIAG");G.hz_diag=hd?atoi(hd):0;}{const char *cd=getenv("DM_CHUNK_DRAWS");G.chunk_draws=cd?atoi(cd):48;G.chunk_log=getenv("DM_RESOURCE_STATS")!=NULL;G.profile_detail=getenv("DM_PROFILE_DETAIL")!=NULL;G.diag_skip_draws=getenv("DM_DIAG_SKIP_DRAWS")!=NULL;{const char *sh=getenv("DM_DIAG_SHAPE");G.diag_shape=sh?atoi(sh):0;}G.disable_setrt_skip=getenv("DM_DISABLE_SETRT_SKIP")!=NULL;G.disable_subres=getenv("DM_DISABLE_SUBRES")!=NULL;G.enable_narrow_access=getenv("DM_NARROW_ACCESS")!=NULL;G.diag_fixed_desc=getenv("DM_DIAG_FIXED_DESC")!=NULL;G.disable_wait_elision=getenv("DM_DISABLE_WAIT_ELISION")!=NULL;G.async_enabled=getenv("DM_DISABLE_ASYNC_COMMIT")==NULL;G.async_drain_after_submit=getenv("DM_ASYNC_DRAIN")!=NULL;{const char *sc=getenv("DM_DIAG_SKIP_CLASS");G.diag_skip_class=sc?atoi(sc):0;}G.wb_stage_enabled=getenv("DM_DISABLE_WB_STAGE")==NULL;G.disable_pass_reuse=getenv("DM_DISABLE_PASS_REUSE")!=NULL;G.disable_cpu_uniform_snapshot=getenv("DM_DISABLE_CPU_UNIFORM_SNAPSHOT")!=NULL;{const char *sx=getenv("DM_DIAG_SKIP_XFER");G.diag_skip_xfer=sx?atoi(sx):0;}}fprintf(stderr,"[nvn-vulkan] PRECISE_SYNC=%d hz_diag=%d\n",G.precise_sync,G.hz_diag);{const char *iu=getenv("DM_DIAG_IMG_USAGE");G.diag_img_usage=iu?atoi(iu):0;}
    {const char *range=getenv("DM_QUERY_RESET_RANGE");G.enable_query_reset_range=range&&atoi(range)!=0;}
    G.disable_desc_cache=getenv("DM_DISABLE_DESC_CACHE")!=NULL;G.disable_control_cache=getenv("DM_DISABLE_CONTROL_CACHE")!=NULL;G.disable_barrier_elision=getenv("DM_DISABLE_BARRIER_ELISION")!=NULL;G.disable_sample_copy_cache=getenv("DM_DISABLE_SAMPLE_COPY_CACHE")!=NULL;G.pass_timing=getenv("DM_PASS_TIMING")!=NULL||G.pass_stats;G.draw_timing=getenv("DM_DRAW_TIMING")!=NULL;G.disable_shader_stamp=getenv("DM_DISABLE_SHADER_STAMP")!=NULL;G.disable_set_recycle=getenv("DM_DISABLE_SET_RECYCLE")!=NULL;G.desc_diag=getenv("DM_DESC_DIAG")!=NULL;G.wsi_diag=getenv("DM_WSI_DIAG")!=NULL;{const char *sk=getenv("DM_SKIP_PS");if(sk){strncpy(G.skip_ps,sk,16);G.skip_ps[16]=0;fprintf(stderr,"[nvn-vulkan] DIAG_SKIP_PS key=%s (timing diagnostic only)\n",G.skip_ps);}}
    G.disable_resource_cache=getenv("DM_DISABLE_RESOURCE_CACHE")!=NULL;
    G.disable_global_pass_cache=getenv("DM_DISABLE_GLOBAL_PASS_CACHE")!=NULL;
    {const char *v=getenv("DM_ATT_LOAD_TABLE");g_cov_table=v&&atoi(v)!=0;
     v=getenv("DM_PASS_SENTINEL");g_cov_probe=v&&atoi(v)!=0;
     v=getenv("DM_PASS_SENTINEL_SELFTEST");g_cov_selftest=v&&atoi(v)!=0;
     v=getenv("DM_ATT_STORE_SKIP");g_cov_store_skip=v?atoi(v)&3:0;
     v=getenv("DM_RENDER_TRAFFIC");g_cov_traffic_on=v&&atoi(v)!=0;}
    cov_load();
    {const char *retain=getenv("DM_VK_RETAIN_RESOURCES");G.retain_resources=!retain||atoi(retain)!=0;}
    {const char *usage=getenv("DM_VK_IMAGE_USAGE_FLAGS");G.image_usage_by_flags=!usage||atoi(usage)!=0;}
    G.pool_range_stats=getenv("DM_POOL_RANGE_STATS")!=NULL;
    G.pool_owned_bypass=getenv("DM_POOL_OWNED_READ_BYPASS")!=NULL;
    G.disable_binding_plans=getenv("DM_DISABLE_BINDING_PLANS")!=NULL;
    G.plan_precise_state=getenv("DM_PLAN_PRECISE_STATE")!=NULL;
    {const char *cache=getenv("DM_COMMAND_STATE_CACHE");G.enable_command_cache=cache&&atoi(cache)!=0;}
    {const char *q=getenv("DM_QUERY_SKIP_MASK");G.query_skip_mask=q&&*q?(unsigned)strtoul(q,NULL,10)&3u:0;}
    G.disable_dirty_uploads=getenv("DM_DISABLE_DIRTY_UPLOADS")!=NULL;
    {const char *prepare=getenv("DM_WRITEBACK_PREPARE");G.enable_writeback_prepare=prepare&&atoi(prepare)!=0;}
    {const char *coalesce=getenv("DM_WRITEBACK_COALESCE");G.enable_writeback_coalesce=coalesce&&atoi(coalesce)!=0;}
    G.event_trace=getenv("DM_EVENT_TRACE")!=NULL;
    G.current_submission=&G.submission_slot;
    G.slot_publication=getenv("DM_SLOT_PUBLICATION")!=NULL;
    fprintf(stderr,"[nvn-vulkan] SLOT_PUBLICATION enabled=%d\n",G.slot_publication);
    {const char *rs=getenv("DM_RENDER_SLOTS");int n=rs?atoi(rs):1;G.render_slots_n=n<1?1:n>4?4:n;G.render_ring_next=0;
     {const char *df=getenv("DM_DIAG_DUMP_FRAME");G.dump_frame=df?(uint64_t)strtoull(df,NULL,10):0;}
     G.submission_slot.slot_index=0;for(int i=1;i<4;i++)G.extra_render_slots[i-1].slot_index=i;G.submission_slot2.slot_index=-1;
     fprintf(stderr,"[nvn-vulkan] RENDER_SLOTS n=%d\n",G.render_slots_n);}
    S.desc_entries=calloc(DESC_ENTRIES,sizeof(*S.desc_entries));S.desc_arena=malloc(DESC_ARENA);G.control_cache=calloc(CONTROL_CACHE,sizeof(*G.control_cache));
    if(!S.desc_entries||!S.desc_arena||!G.control_cache)die("descriptor/control cache allocation",0);
    {const char *d=getenv("DM_DIAG_BARRIER");G.diag_barrier=d?atoi(d):0;G.barrier_stats=getenv("DM_BARRIER_STATS")!=NULL;
     const char *ls=getenv("DM_DIAG_LOADSTORE");G.diag_loadstore=ls?atoi(ls):0;
     const char *cm=getenv("DM_VK_CLEAR_MERGE"),*se=getenv("DM_VK_STORE_ELIDE");
     G.attachment_defaults=((!cm||strcmp(cm,"0"))?1:0)|((!se||strcmp(se,"0"))?2:0);
     if(G.diag_loadstore)fprintf(stderr,"[nvn-vulkan] DIAG_LOADSTORE mode=%d (timing diagnostic only; render output is intentionally incorrect)\n",G.diag_loadstore);
     if(G.diag_barrier)fprintf(stderr,"[nvn-vulkan] DIAG_BARRIER mode=%d (timing diagnostic only; GPU synchronization is intentionally incorrect)\n",G.diag_barrier);}
    VkCommandPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=G.family,
       .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT}; CHECK(vkCreateCommandPool(G.device,&ci,NULL,&S.command_pool));CHECK(vkCreateCommandPool(G.device,&ci,NULL,&G.submission_slot2.command_pool));
    for(int i=1;i<G.render_slots_n;i++){VkSubmission *x=&G.extra_render_slots[i-1];CHECK(vkCreateCommandPool(G.device,&ci,NULL,&x->command_pool));
        x->desc_entries=calloc(DESC_ENTRIES,sizeof(*x->desc_entries));x->desc_arena=malloc(DESC_ARENA);if(!x->desc_entries||!x->desc_arena)die("render slot descriptor allocation",i);}
    G.support=buffer_make(1392,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT); G.zero=buffer_make(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    for(int i=0;i<73;i++) { float one=1; memcpy(G.support.map+192+16*i,&one,4); }
    G.state.mask.mask=UINT32_MAX; G.state.polygon.mode=NVN_POLYGON_MODE_FILL;
    G.state.color.logic_op=NVN_LOGIC_OP_COPY;G.state.color.alpha_func=NVN_ALPHA_FUNC_ALWAYS;
    G.state.ds.df=NVN_DEPTH_FUNC_LESS; G.state.viewport=(VkViewport){0,0,1,1,0,1};
    G.state.ds.sf_f=G.state.ds.sf_b=NVN_STENCIL_FUNC_ALWAYS;
    G.state.ds.fail_f=G.state.ds.fail_b=G.state.ds.zfail_f=G.state.ds.zfail_b=G.state.ds.zpass_f=G.state.ds.zpass_b=NVN_STENCIL_OP_KEEP;
    G.state.stencil_compare[0]=G.state.stencil_compare[1]=G.state.stencil_write[0]=G.state.stencil_write[1]=UINT32_MAX;
    fprintf(stderr,"[nvn-vulkan] initialized %s\n",G.properties.deviceName);
    G.profile_start_ns=dm_android_nvn_clock_ns();
    pipeline_worker_start();
}
void mtl_init(void) { if(pthread_once(&init_once,backend_init))die("Vulkan initialize once",0); }
#ifdef DM_VK_INDEX_TEST
int dm_vk_index_full_uint32(void) { mtl_init();return G.enabled_features.fullDrawIndexUint32; }
#endif
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
static void async_drain_locked(void);
void dm_vk_graphics_end(void) {
    mtl_init();pthread_mutex_lock(&G.lock);async_drain_locked();
    if(!G.graphics_live || G.active || G.npool)die("nv::FinalizeGraphics live session/pools/submission",G.npool);
    CHECK(vkDeviceWaitIdle(G.device));resource_cache_clear(&G.submission_slot);resource_cache_clear(&G.submission_slot2);for(int i=1;i<G.render_slots_n;i++)resource_cache_clear(&G.extra_render_slots[i-1]);
    G.graphics_live=0;G.transfer_memory=NULL;G.transfer_size=0;
    fprintf(stderr,"[nvn-vulkan] graphics session ended after Vulkan device idle; host device retained\n");pthread_mutex_unlock(&G.lock);
}
void mtl_pool_add(const void *mem,uint64_t size) {
    mtl_init(); pthread_mutex_lock(&G.lock);
    if(!mem || G.npool==LIMIT_POOLS || size>UINTPTR_MAX-(uintptr_t)mem) die("memory pool bounds",size);
    for(int i=0;i<G.npool;i++) if(G.pools[i].guest==mem) die("duplicate memory pool",(long long)(uintptr_t)mem);
    VPool *pool=&G.pools[G.npool++]; pool->guest=mem;
    *pool=(VPool){.guest=mem,.size=size,.serial=++G.pool_adds};G.pool_revision++; pthread_mutex_unlock(&G.lock);
}
static void pool_materialize(VPool *pool) {
    if(pool->v.b)return;
    /* pool_read supplies every CPU-owned range before a GPU read, and queued
     * writes supply their own bytes. Touching the entire backing with memset
     * would commit hundreds of MiB that the game has not used. */
    pool->v=buffer_create_kind(pool->size,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,DM_VK_MEMORY_POOL);
    uint64_t logical=0,resident=0;for(int i=0;i<G.npool;i++){logical+=G.pools[i].size;resident+=G.pools[i].v.size;}
    fprintf(stderr,"[nvn-vulkan] POOL_MATERIALIZE bytes=%llu resident=%llu logical=%llu pools=%d\n",(unsigned long long)pool->size,(unsigned long long)resident,(unsigned long long)logical,G.npool);
}
/* Render-slot overlap guard. With several render slots in flight, CPU writes to a pool mirror (guest uploads, CPU snapshots)
 * must not land on pages an earlier in-flight slot still reads or writes. Each slot keeps 4 KiB page bitmaps per pool; a
 * real conflict waits for that slot's GPU fence and is counted, so a clean run shows slot_conflicts=0. */
#define SLOT_PAGE_SHIFT 8
static VkSubmission *render_slot_ptr(int i){return i?&G.extra_render_slots[i-1]:&G.submission_slot;}
static void pool_use_alloc(VPool *p){
    if(G.render_slots_n<2||p->use_rd[0])return;
    size_t words=(size_t)(((p->size>>SLOT_PAGE_SHIFT)+1+31)/32);
    for(int i=0;i<G.render_slots_n;i++){p->use_rd[i]=calloc(words,4);p->use_wr[i]=calloc(words,4);if(!p->use_rd[i]||!p->use_wr[i])die("slot use bitmap allocation",i);}
}
static void pool_use_mark(VPool *p,uint64_t off,uint64_t len,int write){
    if(!p->use_rd[0]||!len||S.slot_index<0)return;
    uint32_t *bm=write?p->use_wr[S.slot_index]:p->use_rd[S.slot_index];
    for(uint64_t pg=off>>SLOT_PAGE_SHIFT,e=(off+len-1)>>SLOT_PAGE_SHIFT;pg<=e;pg++)bm[pg>>5]|=1u<<(pg&31);
}
static void pool_use_clear(int idx){
    for(int i=0;i<G.npool;i++){VPool *p=&G.pools[i];if(!p->use_rd[0])continue;
        size_t words=(size_t)(((p->size>>SLOT_PAGE_SHIFT)+1+31)/32);memset(p->use_rd[idx],0,words*4);memset(p->use_wr[idx],0,words*4);}
}
static int ctx_busy(const VkSubmission *c);
/* 冲突台账：按（池，类别）汇总，找出冲突集中在哪些范围、有多少是页粒度造成的假冲突 | Conflict ledger: aggregated per (pool, class) to see where conflicts concentrate and how many are page-granularity false sharing */
typedef struct {int used,pool,kind;uint64_t n,range_bytes,hit_pages,range_pages,wait_ns,first_page,last_page;} ConflictLedger;
static ConflictLedger g_conflicts[48];
static void conflict_note(int pool,int kind,uint64_t range_bytes,uint64_t hit_pages,uint64_t range_pages,uint64_t wait_ns,uint64_t first,uint64_t last){
    ConflictLedger *e=NULL;
    for(unsigned i=0;i<48;i++){if(g_conflicts[i].used&&g_conflicts[i].pool==pool&&g_conflicts[i].kind==kind){e=&g_conflicts[i];break;}if(!g_conflicts[i].used&&!e)e=&g_conflicts[i];}
    if(!e)return;if(!e->used){e->used=1;e->pool=pool;e->kind=kind;e->first_page=first;e->last_page=last;}
    e->n++;e->range_bytes+=range_bytes;e->hit_pages+=hit_pages;e->range_pages+=range_pages;e->wait_ns+=wait_ns;
    if(first<e->first_page)e->first_page=first;if(last>e->last_page)e->last_page=last;
}
static void conflict_ledger_dump(void){
    for(unsigned i=0;i<48;i++){ConflictLedger *e=&g_conflicts[i];if(!e->used)continue;
        fprintf(stderr,"[nvn-vulkan] SLOT_CONFLICT pool=%d kind=%s n=%llu avg_range_bytes=%llu hit_pages_of_range=%.2f wait_ms=%.2f page_span=%llu-%llu\n",
            e->pool,e->kind==0?"cpu_upload_vs_slot_write":e->kind==1?"cpu_upload_vs_slot_read":e->kind==2?"snapshot_vs_slot_write":"snapshot_vs_slot_read",
            (unsigned long long)e->n,(unsigned long long)(e->n?e->range_bytes/e->n:0),e->range_pages?(double)e->hit_pages/e->range_pages:0.0,e->wait_ns/1e6,
            (unsigned long long)e->first_page,(unsigned long long)e->last_page);}
    memset(g_conflicts,0,sizeof g_conflicts);
}
static void pool_conflict_wait(VPool *p,uint64_t off,uint64_t len,int cpu_writes){
    if(!p->use_rd[0]||!len||S.slot_index<0)return;
    uint64_t first=off>>SLOT_PAGE_SHIFT,last=(off+len-1)>>SLOT_PAGE_SHIFT;
    for(int j=0;j<G.render_slots_n;j++){
        VkSubmission *c=render_slot_ptr(j);SubmissionPhase phase=atomic_load(&c->phase);
        if(j==S.slot_index||(phase!=SUBMISSION_INFLIGHT&&!(G.slot_publication&&phase==SUBMISSION_GPU_DONE)))continue;
        int hit=0,write_hit=0;uint64_t hit_pages=0;
        for(uint64_t pg=first;pg<=last;pg++){uint32_t m=1u<<(pg&31);
            int w=(p->use_wr[j][pg>>5]&m)!=0,r=cpu_writes&&(p->use_rd[j][pg>>5]&m);
            if(w||r){hit=1;hit_pages++;if(w)write_hit=1;}}
        if(!hit)continue;
        uint64_t started=dm_android_nvn_clock_ns();
        G.slot_conflicts++;
        if(G.slot_publication)nvn_vk_publication_wait(&g_publication,c->ticket);
        else CHECK(cpu_work_wait_fences(G.device,1,&c->gpu_fence,VK_TRUE,UINT64_MAX));
        conflict_note((int)(p-G.pools),(cpu_writes?0:2)+(write_hit?0:1),len,hit_pages,last-first+1,dm_android_nvn_clock_ns()-started,first,last);
    }
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
    pthread_mutex_lock(&G.lock);async_drain_locked(); for(int i=0;i<G.npool;i++) if(G.pools[i].guest==mem) {
        for(int k=0;k<4;k++){free(G.pools[i].use_rd[k]);free(G.pools[i].use_wr[k]);}
        buffer_free(&G.pools[i].v); G.pools[i]=G.pools[--G.npool];G.pool_revision++; pthread_mutex_unlock(&G.lock); return; }
    pthread_mutex_unlock(&G.lock); die("unknown memory pool delete",(long long)(uintptr_t)mem);
}
static int overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn) { return a<b+bn && b<a+an; }
static int fully_gpu_owned(Range range) {
    return nvn_vk_pool_fully_gpu_owned(range.addr,range.size,S.writes,(size_t)S.nwrites);
}
static int cached_snapshot(Range range,VBuffer **buffer) {
    *buffer=NULL;if(!S.read_snapshots_allowed)return -1;
    for(int i=0;i<S.nread_snapshots;i++){
        ReadSnapshot *s=&S.read_snapshots[i];if(s->range.addr!=range.addr||s->range.size!=range.size)continue;
        int changed=0;for(int j=s->write_count;j<S.nwrites;j++)if(overlap(range.addr,range.size,S.writes[j].addr,S.writes[j].size)){changed=1;break;}
        if(!changed){s->write_count=S.nwrites;*buffer=s->buffer;G.profile_snapshot_reuses++;}
        return i;
    }
    /* pool_read skips these entirely GPU-owned bytes, so host uploads cannot
     * change them. All later GPU/queued writes are tracked by mark_write. */
    if(S.nread_snapshots==LIMIT_WRITES || !fully_gpu_owned(range))return -1;
    int i=S.nread_snapshots++;S.read_snapshots[i]=(ReadSnapshot){.range=range};return i;
}
static void pool_upload_guest_span(VPool *p,uint64_t base,uint64_t base_off,uint64_t addr,uint64_t len) {
    pool_conflict_wait(p,base_off+(addr-base),len,1);
    G.profile_pool_uploaded_bytes+=nvn_vk_pool_copy_guest_span(p->v.map,base,base_off,addr,len,S.writes,S.nwrites);
}
static unsigned pool_range_bucket(uint64_t len) {
    static const uint64_t bounds[]={64,256,1024,4096,16384,65536,262144,1048576};
    unsigned i=0;while(i<8&&len>bounds[i])i++;return i;
}
static VPool *pool_read_resolved(VPool *p,uint64_t addr,uint64_t len,uint64_t *off) {
    uint64_t started=prof_now();
    unsigned range_bucket=0;
    if(G.pool_range_stats){range_bucket=pool_range_bucket(len);G.pool_ranges[range_bucket].reads++;G.pool_ranges[range_bucket].read_bytes+=len;}
    pool_materialize(p);pool_use_alloc(p);
    /* 先等旧写者发布，再查脏页；否则会把尚未写回的客体数据当作最新版本。
     * Wait for old writers before the dirty check, or stale guest bytes could be accepted as current.
     */
    if(G.slot_publication)pool_conflict_wait(p,*off,len,0);
    pool_use_mark(p,*off,len,0);
    /* The existing guest-span copier omits every byte in queued write ranges.
     * When their union covers this entire read, no CPU upload is possible.
     * Leave the upload cache untouched: a later uncovered read still needs
     * its ordinary generation check and upload after submission retirement. */
    if(G.pool_owned_bypass&&len&&S.nwrites&&fully_gpu_owned((Range){addr,len})){
        G.profile_owned_reads++;G.profile_owned_bytes+=len;
        G.profile_pool_ns+=prof_now()-started;return p;
    }
    /* Cache exact read ranges. On a dirty cache hit, copy only changed page
     * intersections with this exact range; bytes outside it may belong to a
     * different range referenced by already-recorded draws. Page generations
     * conservatively invalidate overlapping CPU changes, while pool serials
     * prevent address reuse from inheriting old uploads. */
    PoolUpload *upload=NULL;uint64_t stamp=0,upload_stamp=0;int partial_upload=0;
    if(len&&!G.disable_dirty_uploads&&nvn_dirty_tracked((const void *)(uintptr_t)addr,len)){
        uint64_t hash=addr^(addr>>19)^(len*0x9e3779b97f4a7c15ULL)^p->serial;
        PoolUpload *bucket=G.pool_uploads[hash&(UPLOAD_BUCKETS-1)];upload=&bucket[0];
        for(unsigned i=0;i<UPLOAD_WAYS;i++){
            PoolUpload *entry=&bucket[i];
            if(entry->pool_serial==p->serial&&entry->address==addr&&entry->size==len){
                upload=entry;upload_stamp=entry->stamp;
                if(!nvn_dirty_since((const void *)(uintptr_t)addr,len,entry->stamp)){
                    if(G.pool_range_stats)G.pool_ranges[range_bucket].hits++;
                    entry->age=++G.upload_age;G.profile_pool_reused_ranges++;G.profile_pool_reused_bytes+=len;
                    G.profile_pool_ns+=prof_now()-started;return p;
                }
                partial_upload=1;
                break;
            }
            if(entry->age<upload->age)upload=entry;
        }
        if(G.pool_range_stats){G.pool_ranges[range_bucket].protects++;G.pool_ranges[range_bucket].protect_bytes+=len;}
        stamp=nvn_dirty_now();nvn_dirty_protect((const void *)(uintptr_t)addr,len);
    }
    /* A GPU-written range in this command stream must retain its queued value. */
    if(partial_upload) {
        size_t cursor=0,range_offset=0,range_len=0;
        while(nvn_dirty_next_range((const void *)(uintptr_t)addr,(size_t)len,upload_stamp,&cursor,&range_offset,&range_len)) {
            if(!range_len || range_offset>len || range_len>len-range_offset)die("invalid dirty upload span",addr);
            pool_upload_guest_span(p,addr,*off,addr+range_offset,range_len);
        }
    } else {
        pool_upload_guest_span(p,addr,*off,addr,len);
    }
    if(upload)*upload=(PoolUpload){addr,len,p->serial,stamp,++G.upload_age};
    G.profile_pool_ns+=prof_now()-started;return p;
}
static VPool *pool_read(uint64_t addr,uint64_t len,uint64_t *off) {
    VPool *override=ghost_pool_override(addr,len,off);if(override)return override;
    VPool *p=pool_find(addr,len,off);if(!p)die("unmapped NVN buffer range",addr);
    return pool_read_resolved(p,addr,len,off);
}
static void mark_write(uint64_t addr,uint64_t len) {
    uint64_t woff=0;VPool *wp=pool_find(addr,len,&woff);if(!wp) die("unmapped write range",addr);pool_use_alloc(wp);pool_use_mark(wp,woff,len,1);
    nvn_dirty_mark_gpu_written((const void *)(uintptr_t)addr,(size_t)len);
    if(S.nwrites==LIMIT_WRITES) die("write range limit",S.nwrites);
    S.write_cpu[S.nwrites]=NULL;S.writes[S.nwrites++]=(Range){addr,len};
}
static void counter_op(uint32_t kind,uint32_t query,uint64_t address);
static void occlusion_end(void) {
    if(S.active_occlusion!=UINT32_MAX){
        vkCmdEndQuery(S.command,S.occlusion,S.active_occlusion);
        counter_op(1,S.active_occlusion,0);S.active_occlusion=UINT32_MAX;
    }
}
static void chunk_ts_write(int end);
static void pending_clears_flush(void);
static _Atomic int attachment_override=-1;
static _Atomic int attachment_applied=3;
static _Atomic int chunk_override=-1,chunk_applied=48;
static uint64_t chunk_control_sequence,chunk_applied_sequence,chunk_total;
void dm_vk_chunk_config(int draws) {if(draws < -1||draws>2048)die("chunk threshold bounds",draws);atomic_store(&chunk_override,draws);}
int dm_vk_chunk_applied(void) {return atomic_load(&chunk_applied);}
uint64_t dm_vk_chunk_count(void) {pthread_mutex_lock(&G.lock);uint64_t result=chunk_total;pthread_mutex_unlock(&G.lock);return result;}
/* Diagnostic control only. No file is polled unless enabled before startup.
 * A request takes effect at the next submission, never midway through a slot. */
static void chunk_control_poll(void) {
    const char *path=getenv("DM_VK_CHUNK_CONTROL");if(!path)return;
    static uint64_t last;uint64_t now=dm_android_nvn_clock_ns();if(now-last<100000000)return;last=now;
    FILE *f=fopen(path,"r");if(!f)return;
    char text[96]={0};size_t size=fread(text,1,sizeof text-1,f);int tail=fgetc(f),error=ferror(f);fclose(f);
    if(!size||error||tail!=EOF||memchr(text,0,size))return;
    uint64_t numbers[2]={0};const char *p=text;
    for(unsigned field=0;field<2;field++) {
        if(*p<'0'||*p>'9')return;
        uint64_t limit=field?2048:UINT64_MAX;
        do {unsigned digit=(unsigned)(*p++-'0');if(numbers[field]>(limit-digit)/10)return;numbers[field]=numbers[field]*10+digit;}while(*p>='0'&&*p<='9');
        if(!field&&*p!=' '&&*p!='\t')return;
        while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
    }
    if(*p||numbers[0]<=chunk_control_sequence)return;
    chunk_control_sequence=numbers[0];dm_vk_chunk_config((int)numbers[1]);
    fprintf(stderr,"[nvn-vulkan] CHUNK_CONTROL pid=%d sequence=%llu threshold=%d mono_ns=%llu\n",
        getpid(),(unsigned long long)numbers[0],(int)numbers[1],(unsigned long long)now);
}
int dm_vk_attachment_applied_flags(void) {return atomic_load(&attachment_applied);}
/* Diagnostic A/B control, latched at submit begin. Call between QueueFinish
 * boundaries to compare the same workload within one process. */
void dm_vk_attachment_config(int flags) { atomic_store(&attachment_override,flags<0?-1:flags&3); }
void dm_vk_attachment_stats(NvnAttachmentStats *out) {pthread_mutex_lock(&G.lock);*out=G.attachment_stats;pthread_mutex_unlock(&G.lock);}
/* Diagnostic only: requests are "monotonic_sequence flags" (flags 0..3).
 * Publish the override after a present; the next submission latches it.
 * These timestamps describe CPU present calls, not display/scanout times. */
static uint64_t attachment_ab_sequence;
static void attachment_ab_poll(uint64_t completed_frame) {
    const char *path=getenv("DM_VK_ATTACHMENT_CONTROL");if(!path)path=getenv("DM_VK_AB_CONTROL");if(!path)return;
    static uint64_t last_poll;uint64_t now=dm_android_nvn_clock_ns();if(now-last_poll<100000000)return;last_poll=now;
    FILE *f=fopen(path,"r");if(!f)return;
    char text[96]={0},extra;size_t size=fread(text,1,sizeof text-1,f);int tail=fgetc(f);fclose(f);
    unsigned long long sequence=0;int flags=-1;
    if(!size||tail!=EOF||sscanf(text,"%llu %d %c",&sequence,&flags,&extra)!=2||
       sequence<=attachment_ab_sequence||flags<0||flags>3)return;
    attachment_ab_sequence=sequence;dm_vk_attachment_config(flags);
    fprintf(stderr,"[nvn-vulkan] ATTACHMENT_MODE pid=%d sequence=%llu flags=%d after_frame=%llu mono_ns=%llu\n",
        getpid(),sequence,flags,(unsigned long long)completed_frame,(unsigned long long)dm_android_nvn_clock_ns());
}
static void attachment_ab_frame(uint64_t frame) {
    binding_plan_mode_poll(frame);
    query_skip_mode_poll(frame);
    cov_frame_report(frame);
    chunk_control_poll();
    if(!getenv("DM_VK_AB_CONTROL")){attachment_ab_poll(frame);return;}
    const NvnAttachmentStats *a=&G.attachment_stats;
    fprintf(stderr,"[nvn-vulkan] ATTACHMENT_FRAME pid=%d frame=%llu sequence=%llu flags=%d mono_ns=%llu width=%u height=%u draws=%llu clear_loads=%llu inline_clears=%llu transfer_clears=%llu discard_stores=%llu logical_load_bytes=%llu logical_store_bytes=%llu\n",
        getpid(),(unsigned long long)frame,(unsigned long long)attachment_ab_sequence,G.attachment_flags,
        (unsigned long long)dm_android_nvn_clock_ns(),G.swap_extent.width,G.swap_extent.height,
        (unsigned long long)G.draws,(unsigned long long)a->clear_loads,(unsigned long long)a->inline_clears,
        (unsigned long long)a->transfer_clears,(unsigned long long)a->discard_stores,
        (unsigned long long)a->load_bytes_avoided,(unsigned long long)a->store_bytes_avoided);
    attachment_ab_poll(frame);
}
/* Hot-path environment lookups: getenv scans environ, so resolve the value once. */
static const char *adapted_shader_dir(void){static int init;static const char *v;if(!init){v=getenv("DM_ADAPTED_SHADER_DIR");init=1;}return v;}
static int copy_view_log(void){static int init,enabled;if(!init){enabled=getenv("DM_DIAG_COPY_VIEW")!=NULL;init=1;}return enabled;}
static const char *frame_dump_dir(void){static int init;static const char *v;if(!init){v=getenv("DM_FRAME_DIR");init=1;}return v;}
/* Diagnostic (profile_detail): depth-state mix of the draws in the current pass and the depth attachment ops. */
static struct{uint8_t dt,dw,df,st,sf,fo,po,zo,wm,ref;uint32_t n;}dsmix[6];static int ndsmix;static int dbg_dload=-1,dbg_dstore=-1;
static void dsmix_note(void){uint8_t dt=G.state.ds.dt,dw=G.state.ds.dw,df=G.state.ds.df,st=G.state.ds.st,sf=G.state.ds.sf_f,fo=G.state.ds.fail_f,po=G.state.ds.zpass_f,zo=G.state.ds.zfail_f,wm=(uint8_t)G.state.stencil_write[0],ref=(uint8_t)G.state.stencil_ref[0];int i;
    for(i=0;i<ndsmix;i++)if(dsmix[i].dt==dt&&dsmix[i].dw==dw&&dsmix[i].df==df&&dsmix[i].st==st&&dsmix[i].sf==sf&&dsmix[i].fo==fo&&dsmix[i].po==po&&dsmix[i].zo==zo&&dsmix[i].wm==wm&&dsmix[i].ref==ref){dsmix[i].n++;return;}
    if(ndsmix<6){dsmix[ndsmix].dt=dt;dsmix[ndsmix].dw=dw;dsmix[ndsmix].df=df;dsmix[ndsmix].st=st;dsmix[ndsmix].sf=sf;dsmix[ndsmix].fo=fo;dsmix[ndsmix].po=po;dsmix[ndsmix].zo=zo;dsmix[ndsmix].wm=wm;dsmix[ndsmix].ref=ref;dsmix[ndsmix].n=1;ndsmix++;}}
static void dsmix_print_reset(void){fprintf(stderr," dload=%d dstore=%d ds=",dbg_dload,dbg_dstore);
    for(int i=0;i<ndsmix;i++)fprintf(stderr,"[t%u w%u f%u s%u cmp%u ops%u/%u/%u wm%02x ref%u x%u]",dsmix[i].dt,dsmix[i].dw,dsmix[i].df,dsmix[i].st,dsmix[i].sf,dsmix[i].fo,dsmix[i].po,dsmix[i].zo,dsmix[i].wm,dsmix[i].ref,dsmix[i].n);ndsmix=0;}
/* Diagnostic: DM_DIAG_DUMP_FRAME=N copies the first colour attachment of every pass in presented frame N to
 * DM_FRAME_DIR/pass-<index>-<w>x<h>-fmt<vk>-bpp<n>-s<serial>.bin so two drivers can be diffed pass by pass. */
static VTexture *texture_get(const tex_t *source,int upload);
static VkImageAspectFlags aspect(VkFormat f);
static const tex_t *dbg_dump_src;static uint32_t dbg_dump_level,dbg_dump_layer,dbg_dump_w,dbg_dump_h,dbg_dump_idx;static uint64_t dbg_dump_frame_seen;
static void dump_pass_color(void){
    const tex_t *src=dbg_dump_src;dbg_dump_src=NULL;if(!src||S.ntrace_captures>=500)return;
    VTexture *t=texture_get(src,0);uint32_t w=dbg_dump_w,h=dbg_dump_h,lvl=dbg_dump_level,lay_i=dbg_dump_layer;
    VkImageAspectFlags all=aspect(t->format),plane=(all&VK_IMAGE_ASPECT_COLOR_BIT)?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t texel=(plane==VK_IMAGE_ASPECT_COLOR_BIT)?(uint32_t)t->bpp:4;uint64_t bytes=(uint64_t)w*h*texel;
    VBuffer *b=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);VkImageLayout lay=t->layout;
    VkImageSubresourceRange rg={all,lvl,1,lay_i,1};
    VkImageMemoryBarrier br={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=lay,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->image,.subresourceRange=rg};
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&br);
    VkBufferImageCopy cp={.imageSubresource={plane,lvl,lay_i,1},.imageExtent={w,h,1}};
    vkCmdCopyImageToBuffer(S.command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b->b,1,&cp);
    br.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;br.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    br.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;br.newLayout=lay;
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&br);
    char name[128];snprintf(name,sizeof name,"pass-%03u-%ux%u-fmt%d-bpp%d-s%llu-l%u",dbg_dump_idx++,w,h,(int)t->format,(int)texel,(unsigned long long)src->serial,lay_i);
    trace_capture_queue(b,bytes,name);
}
static void pass_end_at(int line) { if(S.in_pass) { ghost_flush();G.pass_end_sites[line&4095]++;if(G.profile_detail&&G.profile_frames==30){fprintf(stderr,"[nvn-vulkan] PASSLOG seg=%u rt=%s ps=%.6s n=%d bb=%#x uses=%s draws=%llu end_line=%d",G.seg_index,G.pass_desc,G.last_ps,G.last_count,G.last_barrier_bits,G.last_uses,(unsigned long long)(G.draws-G.pass_draws0),line);dsmix_print_reset();fputc(10,stderr);}else ndsmix=0;occlusion_end();
    if(S.pt_open&&S.pass_stats)vkCmdEndQuery(S.command,S.pass_stats,S.npt);
    vkCmdEndRenderPass(S.command); S.in_pass=0;cov_probe_end();if(dbg_dump_src)dump_pass_color();
    if(S.pt_open){S.pt_open=0;vkCmdWriteTimestamp(S.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,S.pass_ts,S.pt[S.npt].q0+1);S.pt[S.npt].q1=S.pt[S.npt].q0+1;S.pt[S.npt].draws=G.draws-S.pt_draws0;S.npt++;} } }
#define pass_end() pass_end_at(__LINE__)
static void query_reset(VkQueryPool pool,uint32_t first,uint32_t count) {
    if(S.in_pass || S.active_occlusion!=UINT32_MAX)die("query reset inside render pass/query",first);
    vkCmdResetQueryPool(S.command,pool,first,count);
    G.profile_query_reset_calls++;G.profile_query_reset_slots+=count;
}
static void query_reserve(VkQueryPool pool,uint32_t *reset_count,uint32_t first,uint32_t count,uint32_t capacity) {
    if(!G.enable_query_reset_range)return;
    if(first>=capacity || !count || count>capacity-first)die("query reserve bounds",first);
    uint32_t required=first+count;
    if(required<=*reset_count)return;
    /* Each submission allocates query indices monotonically. Reset a fresh
     * prefix in 32-slot blocks before its first use, outside any render pass.
     * Never reset a previously used query: retirement still needs its result.
     * Command-buffer reuse starts with an empty prefix after the real fence. */
    pass_end();
    uint32_t end=(required+31u)&~31u;if(end>capacity)end=capacity;
    query_reset(pool,*reset_count,end-*reset_count);*reset_count=end;
}
#include "nvn_vk_draw_timing.inc"
static void pass_timing_collect(VkSubmission *sub) {
    draw_timing_collect(sub);
    if(!sub->pass_ts)return;
    if(sub->npt){uint64_t ticks[1024];CHECK(vkGetQueryPoolResults(G.device,sub->pass_ts,0,sub->npt*2,sizeof(ticks[0])*sub->npt*2,ticks,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
        uint64_t stats[512][4];
        if(sub->pass_stats)CHECK(vkGetQueryPoolResults(G.device,sub->pass_stats,0,sub->npt,sizeof(stats[0])*sub->npt,stats,sizeof(stats[0]),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
        uint64_t mask=G.timestamp_bits>=64?UINT64_MAX:((1ULL<<G.timestamp_bits)-1);
        double total_ms=0,max_ms=0;
        for(uint32_t i=0;i<sub->npt;i++){uint64_t a=ticks[sub->pt[i].q0],b=ticks[sub->pt[i].q1];
            double ms=(double)((b-a)&mask)*G.properties.limits.timestampPeriod/1e6;uint32_t k=0;
            total_ms+=ms;if(ms>max_ms)max_ms=ms;
            if(sub->pass_stats){G.pass_stats_passes++;for(unsigned c=0;c<4;c++)G.pass_stats_total[c]+=stats[i][c];}
            while(k<G.npacc&&!(G.pacc[k].key==sub->pt[i].key&&G.pacc[k].w==sub->pt[i].w&&G.pacc[k].h==sub->pt[i].h&&G.pacc[k].n==sub->pt[i].n))k++;
            if(k==G.npacc){if(k==128)continue;G.pacc[k]=(PassAcc){.key=sub->pt[i].key,.w=sub->pt[i].w,.h=sub->pt[i].h,.n=sub->pt[i].n};G.npacc++;}
            G.pacc[k].ms+=ms;G.pacc[k].passes++;G.pacc[k].draws+=sub->pt[i].draws;
            if(sub->pass_stats)for(unsigned c=0;c<4;c++)G.pacc[k].stats[c]+=stats[i][c];}
        if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] PASS_QUERY_STATS passes=%u chunks=%u total_ms=%.6f max_ms=%.6f\n",sub->npt,sub->chunk_index+1,total_ms,max_ms);
    }
    sub->npt=0;sub->pt_open=0; /* Query storage belongs to the completed context; keep it for reuse. */
}
/* Cumulative completed-query snapshot for the real backend device fixture.
 * Caller first finishes its NVN queue; this never drains or changes guest work. */
void dm_vk_pass_stats(uint64_t out[7]) {
    pthread_mutex_lock(&G.lock);
    out[0]=G.pass_stats;out[1]=G.pass_stats_passes;out[2]=G.pass_stats_omitted;
    memcpy(out+3,G.pass_stats_total,sizeof G.pass_stats_total);
    pthread_mutex_unlock(&G.lock);
}
enum { HZ_GV,HZ_GF,HZ_T,HZ_CS };
#define HZ_ATT_STAGES (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
#define HZ_ATT_ACCESS (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
#define HZ_SHADER_STAGES (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
#define HZ_COPY_SRC (G.precise_sync?((G.hz_diag&4)?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:(HZ_SHADER_STAGES|VK_PIPELINE_STAGE_TRANSFER_BIT)):VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
#define HZ_COPY_DST (G.precise_sync?((G.hz_diag&4)?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|(G.xition_vertex?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:0))):VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
static void hz_add(unsigned classes,VkPipelineStageFlags stages,VkAccessFlags access) {
    for(unsigned c=0;c<4;c++)if(classes&(1u<<c)){S.pend[c].stages|=stages;S.pend[c].access|=access;}
}
static void hz_note(int kind) {
    if(!G.precise_sync)return;
    switch(kind) {
    case HZ_DRAW:
        hz_add(1u<<HZ_GF|1u<<HZ_T|1u<<HZ_CS,HZ_ATT_STAGES,HZ_ATT_ACCESS);
        hz_add(1u<<HZ_T|1u<<HZ_CS,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|HZ_SHADER_STAGES,0);
        if(G.draw_store)hz_add(15u,HZ_SHADER_STAGES,VK_ACCESS_SHADER_WRITE_BIT);
        break;
    case HZ_ATTACH:hz_add(1u<<HZ_GF|1u<<HZ_T|1u<<HZ_CS,HZ_ATT_STAGES,HZ_ATT_ACCESS);break;
    case HZ_TRANSFER:hz_add(15u,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);break;
    case HZ_COMPUTE:hz_add(15u,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);break;
    }
}
static void hz_sync(unsigned cls,VkPipelineStageFlags dst_stages,VkAccessFlags dst_access) {
    if(!S.pend[cls].stages)return;
    if(G.hz_diag&2){S.pend[cls].stages=S.pend[cls].access=0;return;}
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=S.pend[cls].access,.dstAccessMask=dst_access};
    vkCmdPipelineBarrier(S.command,S.pend[cls].stages,dst_stages,0,1,&b,0,NULL,0,NULL);
    S.pend[cls].stages=S.pend[cls].access=0;G.profile_hz_barriers++;G.hz_count[2+(cls==HZ_CS)]++;
}
/* Before a render pass: attachment-only pending writes only gate fragment-side work, so the next pass's
 * vertex/tiler stages can overlap the previous pass's fragment work. */
static void hz_pass_sync(void) {
    VkPipelineStageFlags src=S.pend[HZ_GV].stages|S.pend[HZ_GF].stages;
    if(!src)return;
    if(G.hz_diag&8){S.pend[HZ_GV].stages=S.pend[HZ_GV].access=S.pend[HZ_GF].stages=S.pend[HZ_GF].access=0;return;}
    VkAccessFlags access=S.pend[HZ_GV].access|S.pend[HZ_GF].access;
    int wide=S.pend[HZ_GV].stages!=0;
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=access,
        .dstAccessMask=wide?(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT):
            (VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)};
    vkCmdPipelineBarrier(S.command,src,wide?VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT:(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|HZ_ATT_STAGES),0,1,&b,0,NULL,0,NULL);
    S.pend[HZ_GV].stages=S.pend[HZ_GV].access=S.pend[HZ_GF].stages=S.pend[HZ_GF].access=0;G.profile_hz_barriers++;G.hz_count[wide?1:0]++;
}
static void all_barrier_at(int line,int soft) {
    G.profile_barriers++;G.barrier_sites[line&4095]++;
    if(soft&&G.diag_barrier==2)return;
    if(G.precise_sync){
        pass_end_at(line);
        if(!soft)hz_sync(HZ_T,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        return;
    }
    pass_end(); VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
       .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    if(G.diag_barrier==1)return;
    if(!S.command_dirty&&!G.disable_barrier_elision){G.profile_barriers_skipped++;return;}
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,NULL,0,NULL);
    S.command_dirty=0;
}
#define all_barrier() all_barrier_at(__LINE__,0)
#define all_barrier_soft() all_barrier_at(__LINE__,1)
static uint64_t pso_key_hash(const PsoKey *key) {
    uint64_t started=prof_now();
    /* Four independent multiply-rotate lanes over 8-byte words; every key byte is hashed. */
    const uint8_t *bytes=(const uint8_t *)key;uint64_t h0=0x9e3779b97f4a7c15ULL,h1=0xc2b2ae3d27d4eb4fULL,h2=0x165667b19e3779f9ULL,h3=0x27d4eb2f165667c5ULL;size_t i=0;
    for(;i+32<=sizeof(*key);i+=32){uint64_t w[4];memcpy(w,bytes+i,32);
        h0=(h0^w[0])*0xff51afd7ed558ccdULL;h0^=h0>>32;h1=(h1^w[1])*0xc4ceb9fe1a85ec53ULL;h1^=h1>>32;
        h2=(h2^w[2])*0xff51afd7ed558ccdULL;h2^=h2>>32;h3=(h3^w[3])*0xc4ceb9fe1a85ec53ULL;h3^=h3>>32;}
    for(;i+8<=sizeof(*key);i+=8){uint64_t w;memcpy(&w,bytes+i,8);h0=(h0^w)*0xff51afd7ed558ccdULL;h0^=h0>>32;}
    for(;i<sizeof(*key);i++){h1=(h1^bytes[i])*1099511628211ULL;}
    uint64_t h=h0^(h1*31)^(h2*0x9e3779b97f4a7c15ULL)^(h3<<7);h^=h>>29;h*=0xbf58476d1ce4e5b9ULL;h^=h>>32;
    G.profile_pso_hash_calls++;G.profile_pso_hash_ns+=prof_now()-started;return h;
}
static void pso_probe_stats(uint64_t probes) {
    G.profile_pso_probes+=probes;if(probes>G.profile_pso_max_probe)G.profile_pso_max_probe=probes;
}
static Pso *pso_cached(const PsoKey *key,uint64_t hash) {
    uint64_t probes=0;
    if(!G.disable_pso_reuse){
        uint32_t slot=(uint32_t)hash&(PSO_TABLE_SIZE-1);
        while(G.pso_table[slot]){
            uint32_t index=G.pso_table[slot]-1;CachedPso *p=&G.psos[index];probes++;
            if(p->hash==hash&&!memcmp(&p->key,key,sizeof(*key))){
                G.profile_pso_hits++;G.profile_pso_table_hits++;G.last_pso_slot=index;pso_probe_stats(probes);return &p->pso;}
            slot=(slot+1)&(PSO_TABLE_SIZE-1);
        }
    }
    G.last_pso_slot=UINT32_MAX;G.profile_pso_misses++;pso_probe_stats(probes);return NULL;
}
static Pso *pso_lookup(const PsoKey *key,uint64_t *hash) {
    uint64_t started=prof_now();G.profile_pso_lookups++;*hash=0;Pso *cached=NULL;
    /* Only permanent append-only cache slots survive submission retirement.
     * Compare every byte of the existing key; no pipeline state is omitted. */
    if(G.enable_pso_last&&!G.disable_pso_reuse){
        if(G.last_pso_slot<G.npso&&!memcmp(&G.psos[G.last_pso_slot].key,key,sizeof(*key))){
            cached=&G.psos[G.last_pso_slot].pso;G.profile_pso_hits++;G.profile_pso_last_hits++;
        }else G.profile_pso_last_misses++;
    }
    if(!cached){*hash=pso_key_hash(key);cached=pso_cached(key,*hash);}
    G.profile_pso_lookup_ns+=prof_now()-started;return cached;
}
static Pso pso_remember(const PsoKey *key,uint64_t hash,Pso pso) {
    /* Cached objects live with the device, like their shader modules/layouts.
     * Once full, keep the original fence-owned temporary lifetime. Never evict
     * an object referenced by commands in the current submission. */
    if(!G.disable_pso_reuse&&G.npso<LIMIT_PSOS){
        uint32_t index=G.npso++,slot=(uint32_t)hash&(PSO_TABLE_SIZE-1);
        G.psos[index]=(CachedPso){.key=*key,.pso=pso,.hash=hash};
        while(G.pso_table[slot])slot=(slot+1)&(PSO_TABLE_SIZE-1);
        G.pso_table[slot]=index+1;G.last_pso_slot=index;
    }else{
        G.last_pso_slot=UINT32_MAX;
        if(S.npipeline==LIMIT_WRITES)die("temporary pipeline limit",S.npipeline);
        S.pipelines[S.npipeline]=pso.pipeline;S.layouts[S.npipeline++]=pso.layout;
    }
    return pso;
}
static VkDescriptorPool descriptor_pool_create(void) {
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16384},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,16384},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8192},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,16384},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4096},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,16384},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,4096},{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,4096},{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,4096}};
    VkDescriptorPoolCreateInfo d={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=8192,
        .poolSizeCount=sizeof sizes/sizeof sizes[0],.pPoolSizes=sizes};
    VkDescriptorPool pool=VK_NULL_HANDLE;CHECK(vkCreateDescriptorPool(G.device,&d,NULL,&pool));return pool;
}
/* Slot reuse: the slot's fence and publication completed, so every set it handed out is free again. Rebuild (a real pool
 * reset that forgets all sets) when an overflow pool was needed or the kept sets grew large. */
static void descriptor_sets_rewind(VkSubmission *sub) {
    if(G.disable_set_recycle||sub->nextra_pools||sub->sets_total>4096){
        for(uint32_t i=0;i<sub->nextra_pools;i++){CHECK(vkResetDescriptorPool(G.device,sub->extra_pools[i],0));vkDestroyDescriptorPool(G.device,sub->extra_pools[i],NULL);}
        sub->nextra_pools=0;sub->alloc_pool=0;
        CHECK(vkResetDescriptorPool(G.device,sub->descriptors,0));
        for(uint32_t i=0;i<sub->nset_buckets;i++){free(sub->set_buckets[i].sets);sub->set_buckets[i].sets=NULL;}
        sub->nset_buckets=0;sub->sets_total=0;return;
    }
    for(uint32_t i=0;i<sub->nset_buckets;i++)sub->set_buckets[i].used=0;
}
static void submission_pools_create(VkSubmission *sub) {
    VkCommandBufferAllocateInfo a={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=sub->command_pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};CHECK(vkAllocateCommandBuffers(G.device,&a,&sub->command));sub->command_primary=sub->command;sub->chunk_cb[0]=sub->command;sub->chunk_alloc=1;
    VkQueryPoolCreateInfo q={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=LIMIT_WRITES};
    CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->occlusion));q.queryType=VK_QUERY_TYPE_TIMESTAMP;
    CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->timestamps));
    if(G.pass_timing){q.queryCount=1024;CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->pass_ts));}
    if(G.draw_timing){q.queryCount=DT_CAP;CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->draw_ts));sub->dt=calloc(DT_CAP,sizeof(*sub->dt));if(!sub->dt)die("draw timing storage",DT_CAP);}
    if(G.profile_detail){q.queryCount=128;CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->chunk_ts));}
    if(G.pass_stats){q.queryType=VK_QUERY_TYPE_PIPELINE_STATISTICS;q.queryCount=512;q.pipelineStatistics=PASS_STATS_FLAGS;CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->pass_stats));}
    sub->descriptors=descriptor_pool_create();
    VkFenceCreateInfo f={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(vkCreateFence(G.device,&f,NULL,&sub->gpu_fence));
    G.profile_submission_creates++;
}
static void submission_gpu_done(VkSubmission *sub) {
    if(sub->phase!=SUBMISSION_INFLIGHT || !pthread_equal(sub->recorder,pthread_self()))die("submission completion ownership",sub->epoch);
    /* Wait success, not a software ticket, establishes safe query/readback access. */
    CHECK(vkGetFenceStatus(G.device,sub->gpu_fence));sub->phase=SUBMISSION_GPU_DONE;
    cov_probe_complete(sub);
    if(sub->depth_input_guard_active){
        uint32_t failed=0;memcpy(&failed,sub->depth_input_guard.map,4);
        if(failed){
            /* The real fence also covers explicitly requested diagnostic
             * captures. Preserve them without publishing any guest effects. */
            trace_capture_complete(sub);
            die("depth input projected sample outside calibrated device interval",failed);
        }
        fprintf(stderr,"[nvn-vulkan] DEPTH_INPUT_GUARD_PASS epoch=%llu\n",(unsigned long long)sub->epoch);
    }
}
static void submission_publish(VkSubmission *sub) {
    if(sub->phase!=SUBMISSION_GPU_DONE || !pthread_equal(sub->recorder,pthread_self()))die("submission publication ownership",sub->epoch);
    /* Called after original writeback, counter and event operations, in that order. */
    atomic_store(&G.completed,sub->ticket);sub->phase=SUBMISSION_PUBLISHED;G.profile_submission_publications++;
}
static void submission_released(VkSubmission *sub) {
    if(sub->phase!=SUBMISSION_PUBLISHED || sub->ntemporary || sub->ntrace_captures)die("submission retirement before publication",sub->epoch);
    sub->phase=SUBMISSION_RETIRED;G.active=0;
}
/* Segment-end copy of GPU-written pool ranges into a HOST_CACHED staging buffer. Reading the write-combined
 * pool mirror with the CPU is very slow; the cached copy is read after the fence with one invalidate. */
static int wb_stage_has_cached_type(void) {
    for(uint32_t i=0;i<G.memory.memoryTypeCount;i++){VkMemoryPropertyFlags f=G.memory.memoryTypes[i].propertyFlags;
        if((f&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)&&(f&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)&&!(f&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return 1;}
    return 0;
}
static void wb_stage_record(void) {
    S.wb_staged=0;
    if(!G.wb_stage_enabled||G.enable_writeback_coalesce||!S.nwrites||!wb_stage_has_cached_type())return;
    uint64_t total=0;for(int i=0;i<S.nwrites;i++)total+=S.writes[i].size;
    if(!total)return;
    if(S.wb_stage.size<total){
        if(S.wb_stage.b)buffer_free(&S.wb_stage);
        uint64_t want=total+(total>>1);if(want<(1ULL<<20))want=1ULL<<20;
        G.want_cached=1;S.wb_stage=buffer_create_kind(want,VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,DM_VK_MEMORY_WRITEBACK);G.want_cached=0;
    }
    all_barrier();
    VkBufferCopy regions[64];uint32_t nregions=0;VkBuffer source=VK_NULL_HANDLE;uint64_t cursor=0;
    for(int i=0;i<S.nwrites;i++){
        Range r=S.writes[i];uint64_t off=0;VPool *p=pool_find(r.addr,r.size,&off);
        if(!p||!p->v.b)die("writeback stage pool lifetime",r.addr);
        if(nregions&&(source!=p->v.b||nregions==64)){vkCmdCopyBuffer(S.command,source,S.wb_stage.b,nregions,regions);nregions=0;}
        source=p->v.b;regions[nregions++]=(VkBufferCopy){off,cursor,r.size};cursor+=r.size;
    }
    if(nregions)vkCmdCopyBuffer(S.command,source,S.wb_stage.b,nregions,regions);
    S.wb_stage_total=total;S.wb_staged=1;
}
static void wb_stage_invalidate(VkSubmission *sub) {
    uint64_t atom=G.properties.limits.nonCoherentAtomSize;if(!atom)atom=1;
    uint64_t size=(sub->wb_stage_total+atom-1)/atom*atom;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=sub->wb_stage.m,.offset=0,.size=size<sub->wb_stage.allocation_size?size:VK_WHOLE_SIZE};
    CHECK(vkInvalidateMappedMemoryRanges(G.device,1,&range));
}
static void submission_complete_effects(VkSubmission *sub) {
    if(sub->phase!=SUBMISSION_GPU_DONE || !(G.active||G.completing) || !pthread_equal(sub->recorder,pthread_self()))
        die("guest effects before owned GPU completion",sub->epoch);
    pass_timing_collect(sub);
    trace_capture_complete(sub);
    uint64_t wb_cpu=0,wb_faults=0,wb_preps=0,wb_mp_calls=0,wb_mp_pages=0,wb_mp_ns=0;
    if(sub->nwrites){nvn_dirty_stats(&wb_faults,&wb_preps);nvn_dirty_mp_stats(&wb_mp_calls,&wb_mp_pages,&wb_mp_ns);wb_cpu=dm_android_nvn_thread_cpu_ns();}
    size_t copy_count=(size_t)sub->nwrites;uint64_t raw_bytes=0,copied_bytes=0;
    if(G.enable_writeback_coalesce && sub->nwrites) {
        uint64_t plan_cpu=dm_android_nvn_thread_cpu_ns();
        for(int i=0;i<sub->nwrites;i++) {
            Range r=sub->writes[i];VPool *p=pool_find(r.addr,r.size,NULL);
            if(!p || !p->v.map)die("writeback plan pool lifetime",r.addr);
            sub->writeback_plan[i]=(NvnVkWritebackSpan){r.addr,r.size,p->serial};raw_bytes+=r.size;
        }
        copy_count=nvn_vk_writeback_plan(sub->writeback_plan,(size_t)sub->nwrites);
        if(copy_count==SIZE_MAX)die("invalid writeback plan",sub->epoch);
        G.profile_writeback_plan_cpu_ns+=dm_android_nvn_thread_cpu_ns()-plan_cpu;
    }
    uint64_t stage_cursor=0;
    if(sub->wb_staged&&sub->nwrites)wb_stage_invalidate(sub);
    for(size_t i=0;i<copy_count;i++) {
       Range r=G.enable_writeback_coalesce?(Range){sub->writeback_plan[i].addr,sub->writeback_plan[i].size}:sub->writes[i];
       uint64_t off=0;VPool *p=pool_find(r.addr,r.size,&off);
       if(!p || !p->v.map || (G.enable_writeback_coalesce && p->serial!=sub->writeback_plan[i].pool_serial))die("writeback pool lifetime",r.addr);
       /* The real fence has completed; prepare preserves dirty generations
        * while avoiding a write fault on each protected guest page. */
       if(G.enable_writeback_prepare)dm_guest_write_prepare((void *)(uintptr_t)r.addr,r.size);
       if(sub->wb_staged){memcpy((void *)(uintptr_t)r.addr,sub->wb_stage.map+stage_cursor,r.size);stage_cursor+=r.size;}
       else memcpy((void *)(uintptr_t)r.addr,p->v.map+off,r.size);copied_bytes+=r.size;
       if(!G.enable_writeback_coalesce)raw_bytes+=r.size;
    }
    G.profile_writeback_bytes+=copied_bytes;G.profile_writeback_raw_bytes+=raw_bytes;
    G.profile_writeback_raw_ranges+=sub->nwrites;G.profile_writeback_copy_ranges+=copy_count;
    if(getenv("DM_RESOURCE_STATS") && sub->nwrites)fprintf(stderr,"[nvn-vulkan] WRITEBACK_STATS epoch=%llu enabled=%u raw_ranges=%d copied_ranges=%zu raw_bytes=%llu copied_bytes=%llu\n",(unsigned long long)sub->epoch,G.enable_writeback_coalesce,sub->nwrites,copy_count,(unsigned long long)raw_bytes,(unsigned long long)copied_bytes);
    if(sub->nwrites){
        G.profile_writeback_cpu_ns+=dm_android_nvn_thread_cpu_ns()-wb_cpu;G.profile_writeback_calls++;
        uint64_t faults=0,preps=0,mp_calls=0,mp_pages=0,mp_ns=0;nvn_dirty_stats(&faults,&preps);nvn_dirty_mp_stats(&mp_calls,&mp_pages,&mp_ns);
        /* Existing dirty counters are process-wide: concurrent guest writes
         * can contribute to these observed deltas during the copy loop. */
        G.profile_writeback_global_faults+=faults-wb_faults;G.profile_writeback_global_preps+=preps-wb_preps;
        G.profile_writeback_global_mp_calls+=mp_calls-wb_mp_calls;G.profile_writeback_global_mp_pages+=mp_pages-wb_mp_pages;
    }
    uint64_t occ[LIMIT_WRITES],times[LIMIT_WRITES];
    if(sub->nocc)CHECK(vkGetQueryPoolResults(G.device,sub->occlusion,0,sub->nocc,sub->nocc*8,occ,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    if(G.present_count<2 || G.trace_frame)for(uint32_t i=0;i<sub->nocc;++i)fprintf(stderr,"[nvn-vulkan] DRAW_SAMPLES query=%u samples=%llu\n",i,(unsigned long long)occ[i]);
    if(sub->ntime)CHECK(vkGetQueryPoolResults(G.device,sub->timestamps,0,sub->ntime,sub->ntime*8,times,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    for(int i=0;i<sub->ncounter;++i) {CounterOp *op=&sub->counter_ops[i];
        if(op->kind==0)G.samples_passed=0;
        else if(op->kind==1)G.samples_passed+=occ[op->query];
        else {uint64_t ticks=times[op->query];if(G.timestamp_bits<64)ticks&=(1ULL<<G.timestamp_bits)-1;
            uint64_t stamp=(uint64_t)((long double)ticks*G.properties.limits.timestampPeriod*12/625);
            uint64_t result[2]={op->kind==2?G.samples_passed:stamp,stamp},off=0;
            VPool *p=pool_find(op->address,16,&off);if(!p)die("counter report pool lifetime",op->address);
            if(!G.slot_publication&&p->v.map)memcpy(p->v.map+off,result,16);
            if(G.slot_publication)dm_guest_write_prepare((void *)(uintptr_t)op->address,16);
            memcpy((void *)(uintptr_t)op->address,result,16);}
    }
    for(int i=0;i<sub->nevents;i++) { Event *e=&sub->events[i]; if(e->mode) atomic_fetch_add((_Atomic uint32_t *)e->addr,e->value);
       else atomic_store((_Atomic uint32_t *)e->addr,e->value); }
    submission_publish(sub);
}
static pthread_cond_t g_done_cv=PTHREAD_COND_INITIALIZER,g_job_cv=PTHREAD_COND_INITIALIZER;
typedef struct { VkSubmission *ctx;int kind; } AsyncJob;
static AsyncJob g_jobs[8];static int g_jhead,g_jcount,g_worker_up;
static int ctx_busy(const VkSubmission *c) { return c->phase==SUBMISSION_INFLIGHT||c->phase==SUBMISSION_GPU_DONE||c->phase==SUBMISSION_PUBLISHED; }
/* Caller holds G.lock; the condition wait releases it so the completion worker can publish. */
static void async_drain_locked(void) {
    if(G.slot_publication){nvn_vk_publication_drain(&g_publication);publication_retire_ready_locked();return;}
    if(!g_jcount)return;
    uint64_t started=dm_android_nvn_clock_ns();
    uint64_t work_wait=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_FENCE);
    while(g_jcount)pthread_cond_wait(&g_done_cv,&G.lock);
    dm_android_nvn_work_wait_end(work_wait);
    G.profile_drain_wait_ns+=dm_android_nvn_clock_ns()-started;
}
static void sub_begin_ctx(VkSubmission *ctx) {
    mtl_init();uint64_t begin_cpu=dm_android_nvn_thread_cpu_ns();pthread_mutex_lock(&G.lock);
    if(G.slot_publication)publication_retire_ready_locked();
    if(ctx_busy(ctx)){
        uint64_t w0=dm_android_nvn_clock_ns();
        uint64_t work_wait=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_FENCE);
        if(G.slot_publication){nvn_vk_publication_wait(&g_publication,ctx->ticket);publication_retire_ready_locked();}
        else while(ctx_busy(ctx))pthread_cond_wait(&g_done_cv,&G.lock);
        dm_android_nvn_work_wait_end(work_wait);
        uint64_t waited=dm_android_nvn_clock_ns()-w0;
        G.profile_wait_ns+=waited;G.profile_slot_wait_ns+=waited;
    }
    G.current_submission=ctx;if(ctx->slot_index>=0&&G.render_slots_n>1)pool_use_clear(ctx->slot_index);
    if(G.active || (S.phase!=SUBMISSION_UNUSED && S.phase!=SUBMISSION_RETIRED))die("nested/unretired submission",S.epoch);
    if(S.phase==SUBMISSION_UNUSED)submission_pools_create(&S);
    else {
        /* Executable buffers release references before descriptor sets are reset.
         * The slot's fence and publication both completed; no in-flight object is reset. */
        CHECK(vkGetFenceStatus(G.device,S.gpu_fence));
        CHECK(vkResetCommandPool(G.device,S.command_pool,0));descriptor_sets_rewind(&S);
        CHECK(vkResetFences(G.device,1,&S.gpu_fence));G.profile_submission_reuses++;
    }
    resource_cache_begin(&S);
    S.command=S.command_primary;S.chunk_index=0;S.chunk_mark=G.draws;G.seg_begin_ns=dm_android_nvn_clock_ns();
    S.phase=SUBMISSION_RECORDING;S.epoch=++G.recording_epoch;S.ticket=0;S.recorder=pthread_self();G.active=1;atomic_store(&G.recording_active,1);S.publication_cpu_ns=S.publication_wait_ns=0;S.publication_overlap=0;
    if(!G.present_count&&!chunk_control_sequence)chunk_control_poll();
    {int target=atomic_load(&chunk_override);S.chunk_draws=target<0?(uint32_t)G.chunk_draws:(uint32_t)target;
        if(atomic_load(&chunk_applied)!=(int)S.chunk_draws||chunk_applied_sequence!=chunk_control_sequence){
            fprintf(stderr,"[nvn-vulkan] CHUNK_MODE pid=%d sequence=%llu threshold=%u epoch=%llu mono_ns=%llu\n",getpid(),
                (unsigned long long)chunk_control_sequence,S.chunk_draws,(unsigned long long)S.epoch,(unsigned long long)dm_android_nvn_clock_ns());
            chunk_applied_sequence=chunk_control_sequence;
        }
        atomic_store(&chunk_applied,(int)S.chunk_draws);}
    S.nwrites=S.nevents=S.ntemporary=S.npipeline=S.nread_snapshots=S.ndirect_reads=0;S.in_pass=0;
    S.pending_clear=0;S.flushing_clear=0;S.pass_discard=0;
    if(!G.present_count&&!attachment_ab_sequence)attachment_ab_poll(0);
    {int flags=atomic_load(&attachment_override);G.attachment_flags=flags<0?G.attachment_defaults:flags;atomic_store(&attachment_applied,G.attachment_flags);}
    S.depth_input_guard_active=S.depth_input=S.pass_depth_input=0;
    S.ndriver_tables=0;memset(S.driver_table_slots,0,sizeof S.driver_table_slots);memset(S.last_driver_table,0,sizeof S.last_driver_table);
    S.nocc=S.ntime=0;S.ncounter=0;S.nbuffer_view=0;
    S.occlusion_reset_count=S.timestamp_reset_count=S.pass_reset_count=S.pass_stats_reset_count=0;
    S.ndesc=0;S.desc_arena_used=0;memset(S.desc_table,0,sizeof S.desc_table);S.command_dirty=1;memset(S.pend,0,sizeof S.pend);
    S.submission_cpu_readback=S.submission_event_wait=0;S.active_occlusion=UINT32_MAX;
    VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(S.command,&b));
    if(G.render_slots_n>1){VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,NULL,0,NULL);}
    nvn_vk_command_state_reset(&S.command_state);
    if(S.chunk_ts){vkCmdResetQueryPool(S.command,S.chunk_ts,0,128);S.nchunk_ts=0;chunk_ts_write(0);}
    if(!G.enable_query_reset_range){query_reset(S.occlusion,0,LIMIT_WRITES);query_reset(S.timestamps,0,LIMIT_WRITES);S.occlusion_reset_count=S.timestamp_reset_count=LIMIT_WRITES;}
    if(S.pass_ts){if(!G.enable_query_reset_range){query_reset(S.pass_ts,0,1024);S.pass_reset_count=1024;}S.npt=0;S.pt_open=0;}
    if(S.pass_stats&&!G.enable_query_reset_range){query_reset(S.pass_stats,0,512);S.pass_stats_reset_count=512;}
    if(S.draw_ts){query_reset(S.draw_ts,0,DT_CAP);S.ndt=0;S.dt_pass=0;}
    G.profile_begin_cpu_ns+=dm_android_nvn_thread_cpu_ns()-begin_cpu;G.profile_begin_calls++;
}
void mtl_sub_begin(void) {
    int i=G.render_ring_next;G.render_ring_next=(i+1)%G.render_slots_n;
    sub_begin_ctx(i?&G.extra_render_slots[i-1]:&G.submission_slot);
}
void mtl_sub_copy(uint64_t src,uint64_t dst,uint64_t len) {
    all_barrier(); uint64_t so=0,doff=0; VPool *s=pool_read(src,len,&so),*d=pool_find(dst,len,&doff);
    if(!d || overlap(src,len,dst,len)) die("buffer copy bounds/overlap",dst);pool_materialize(d);
    VkBufferCopy copy={so,doff,len}; vkCmdCopyBuffer(S.command,s->v.b,d->v.b,1,&copy); mark_write(dst,len);
}
void mtl_sub_event(volatile uint32_t *addr,uint32_t mode,uint32_t value) {
    if(mode>1 || S.nevents==LIMIT_WRITES) die("event mode/count",mode);
    S.events[S.nevents++]=(Event){addr,mode,value};
}
static void counter_op(uint32_t kind,uint32_t query,uint64_t address) {
    if(S.ncounter==LIMIT_WRITES)die("counter operation limit",S.ncounter);
    S.counter_ops[S.ncounter++]=(CounterOp){kind,query,address};
    if(G.slot_publication&&kind>=2){uint64_t off=0;VPool *p=pool_find(address,16,&off);
        if(!p)die("counter publication range",address);pool_use_alloc(p);pool_use_mark(p,off,16,1);
        nvn_dirty_mark_gpu_written((void *)(uintptr_t)address,16);
    }
}
void mtl_sub_counter(uint64_t addr,uint32_t type) {
    if(type!=NVN_COUNTER_TYPE_SAMPLES_PASSED && type!=NVN_COUNTER_TYPE_TIMESTAMP && type!=NVN_COUNTER_TYPE_TIMESTAMP_TOP)
        die("GPU counter type implementation pending",type);
    if(!pool_find(addr,16,NULL) || addr%8)die("GPU counter report range/alignment",addr);
    if(!G.timestamp_bits || S.ntime==LIMIT_WRITES)die("GPU timestamp support/count",S.ntime);
    pass_end();uint32_t q=S.ntime++;
    query_reserve(S.timestamps,&S.timestamp_reset_count,q,1,LIMIT_WRITES);
    vkCmdWriteTimestamp(S.command,type==NVN_COUNTER_TYPE_TIMESTAMP_TOP?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,S.timestamps,q);
    counter_op(type==NVN_COUNTER_TYPE_SAMPLES_PASSED?2:3,q,addr);
}
typedef struct { unsigned reports;int candidate,traced;uint64_t wait_cpu,wait_start; } CommitInfo;
static uint64_t commit_after_gpu(CommitInfo ci,uint64_t waited);
static CommitInfo commit_submit(void) {
    uint64_t prepare_cpu=dm_android_nvn_thread_cpu_ns();
    if(!G.active) die("commit without begin",0); pending_clears_flush();pass_end();
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] QUERY_RESET_STATS enabled=%d occ_reset=%u time_reset=%u pass_reset=%u occ_used=%u time_used=%u pass_used=%u\n",G.enable_query_reset_range,S.occlusion_reset_count,S.timestamp_reset_count,S.pass_reset_count,S.nocc,S.ntime,S.npt*2);
    /* Observed candidates only: resource lifetime and mapped-pool hazards have
     * not been solved. Internal counter reset/add operations are not reports. */
    unsigned reports=0;for(int i=0;i<S.ncounter;i++)if(S.counter_ops[i].kind==2 || S.counter_ops[i].kind==3)reports++;
    int traced=G.trace_frame || S.ntrace_captures;
    int candidate=!S.nwrites && !S.nevents && !reports && !traced && !S.submission_cpu_readback && !S.submission_event_wait;
    if(G.slot_publication)publication_stage_record();else wb_stage_record();
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    if(S.chunk_draws&&S.chunk_index&&G.chunk_log)fprintf(stderr,"[nvn-vulkan] CHUNKED_SUBMIT chunks=%u threshold=%u\n",S.chunk_index+1,S.chunk_draws);
    chunk_ts_write(1);CHECK(vkEndCommandBuffer(S.command));VkFence fence=S.gpu_fence;VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&S.command};
    if(S.phase!=SUBMISSION_RECORDING)die("submit phase",S.phase);
    CHECK(vkQueueSubmit(G.queue,1,&si,fence));S.ticket=++G.fence;S.phase=SUBMISSION_INFLIGHT;
    uint64_t wait_cpu=dm_android_nvn_thread_cpu_ns();G.profile_commit_prepare_cpu_ns+=wait_cpu-prepare_cpu;G.profile_commit_calls++;
    CommitInfo ci={reports,candidate,traced,wait_cpu,dm_android_nvn_clock_ns()};
    /* Snapshot recorder-owned counters before another context begins or present
     * resets the frame's segment index. Async completion must not read them. */
    S.profile_segment=G.seg_index<3?G.seg_index:3;
    G.seg_record_ns[S.profile_segment]+=ci.wait_start-G.seg_begin_ns;
    G.seg_draws[S.profile_segment]+=G.draws-S.chunk_mark;
    G.seg_chunks[S.profile_segment]+=S.chunk_index+1;G.seg_index++;
    return ci;
}
uint64_t mtl_sub_commit(void) {
    if(G.slot_publication)async_drain_locked();
    CommitInfo ci=commit_submit();VkFence fence=S.gpu_fence;
    uint64_t wait_start=ci.wait_start;CHECK(cpu_work_wait_fences(G.device,1,&fence,VK_TRUE,UINT64_MAX));uint64_t waited=dm_android_nvn_clock_ns()-wait_start;
    uint64_t ticket=commit_after_gpu(ci,waited);
    atomic_store(&G.recording_active,0);pthread_mutex_unlock(&G.lock);return ticket;
}
static uint64_t commit_after_gpu(CommitInfo ci,uint64_t waited) {
    uint64_t wait_cpu=ci.wait_cpu;unsigned reports=ci.reports;int candidate=ci.candidate,traced=ci.traced;
    if(S.chunk_ts&&S.nchunk_ts>=2){unsigned si=S.profile_segment;uint64_t ts[128];
        if(vkGetQueryPoolResults(G.device,S.chunk_ts,0,S.nchunk_ts&~1u,sizeof ts,ts,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)==VK_SUCCESS){
            uint64_t mask=G.timestamp_bits>=64?~0ULL:((1ULL<<G.timestamp_bits)-1);double period=G.properties.limits.timestampPeriod;uint64_t busy=0,gap=0;
            if(G.profile_frames==30&&si==0)for(uint32_t i=0;i+1<(S.nchunk_ts&~1u);i+=2)fprintf(stderr,"[nvn-vulkan] CHUNK_TIME i=%u dur_us=%.0f gap_us=%.0f passes=%llu draws=%llu\n",i/2,(double)(((ts[i+1]-ts[i])&mask)*period/1000.0),i?(double)(((ts[i]-ts[i-1])&mask)*period/1000.0):0.0,(unsigned long long)(S.chunk_pass0[i+1]-S.chunk_pass0[i]),(unsigned long long)(S.chunk_draw0[i+1]-S.chunk_draw0[i]));
            for(uint32_t i=0;i+1<(S.nchunk_ts&~1u);i+=2){busy+=(ts[i+1]-ts[i])&mask;if(i>=2){uint64_t g=(ts[i]-ts[i-1])&mask;if(g<(1ULL<<40))gap+=g;}}
            G.gts_busy[si]+=(uint64_t)(busy*period);G.gts_gap[si]+=(uint64_t)(gap*period);G.gts_span[si]+=(uint64_t)(((ts[(S.nchunk_ts&~1u)-1]-ts[0])&mask)*period);G.gts_chunks[si]+=S.nchunk_ts/2;}}
    G.seg_wait_ns[S.profile_segment]+=waited;
    int published=G.slot_publication&&S.phase==SUBMISSION_PUBLISHED;
    if(published){cov_probe_complete(&S);pass_timing_collect(&S);trace_capture_complete(&S);}
    else submission_gpu_done(&S);
    uint64_t retire_cpu=dm_android_nvn_thread_cpu_ns();G.profile_commit_wait_cpu_ns+=retire_cpu-wait_cpu;
    G.profile_wait_ns+=waited;G.profile_submits++;
    if(candidate){G.profile_submit_candidates++;G.profile_candidate_wait_ns+=waited;}
    G.profile_write_submits+=S.nwrites!=0;G.profile_event_submits+=S.nevents!=0;G.profile_report_submits+=reports!=0;
    G.profile_trace_submits+=traced!=0;G.profile_cpu_readback_submits+=S.submission_cpu_readback!=0;G.profile_event_wait_submits+=S.submission_event_wait!=0;
    G.profile_events+=S.nevents;G.profile_reports+=reports;
    if(published){
        G.profile_writeback_bytes+=S.wb_stage_total;G.profile_writeback_raw_bytes+=S.publication_raw_bytes;
        G.profile_writeback_raw_ranges+=S.nwrites;G.profile_writeback_copy_ranges+=S.publication_copy_count;
        G.profile_writeback_cpu_ns+=S.publication_cpu_ns;G.profile_writeback_calls+=S.nwrites!=0;
    }else submission_complete_effects(&S);
    for(int i=0;i<S.npipeline;i++) { vkDestroyPipeline(G.device,S.pipelines[i],NULL); vkDestroyPipelineLayout(G.device,S.layouts[i],NULL); }
    resource_cache_retire(&S);
    temporaries_retire(&S);
    for(int i=0;i<S.nbuffer_view;++i)vkDestroyBufferView(G.device,S.buffer_views[i],NULL);
    if(getenv("DM_PSO_STATS"))fprintf(stderr,"[nvn-vulkan] PSO_STATS cached=%u hits=%llu misses=%llu\n",G.npso,(unsigned long long)G.profile_pso_hits,(unsigned long long)G.profile_pso_misses);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] VIEW_CACHE_STATS view_hits=%llu view_misses=%llu framebuffer_hits=%llu framebuffer_misses=%llu\n",(unsigned long long)G.profile_view_hits,(unsigned long long)G.profile_view_misses,(unsigned long long)G.profile_framebuffer_hits,(unsigned long long)G.profile_framebuffer_misses);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] POOL_UPLOAD_STATS uploaded_bytes=%llu reused_ranges=%llu reused_bytes=%llu\n",(unsigned long long)G.profile_pool_uploaded_bytes,(unsigned long long)G.profile_pool_reused_ranges,(unsigned long long)G.profile_pool_reused_bytes);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] POOL_OWNED_READ_STATS enabled=%d bypassed_ranges=%llu bypassed_bytes=%llu\n",G.pool_owned_bypass,(unsigned long long)G.profile_owned_reads,(unsigned long long)G.profile_owned_bytes);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] RESOURCE_STATS driver_tables=%llu driver_table_reuses=%llu buffer_allocs=%llu buffer_bytes=%llu\n",(unsigned long long)G.profile_driver_tables,(unsigned long long)G.profile_driver_table_reuses,(unsigned long long)G.profile_buffer_allocs,(unsigned long long)G.profile_buffer_bytes);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] TEMP_STATS reuses=%llu cached=%u cached_bytes=%llu aligned_bytes=%llu comparison_bytes=%llu driver_bytes=%llu other_bytes=%llu\n",(unsigned long long)G.profile_temp_reuses,G.nretired,(unsigned long long)G.retired_bytes,(unsigned long long)G.profile_temp_bytes[TEMP_ALIGNED],(unsigned long long)G.profile_temp_bytes[TEMP_COMPARISON],(unsigned long long)G.profile_temp_bytes[TEMP_DRIVER],(unsigned long long)G.profile_temp_bytes[TEMP_OTHER]);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] STORAGE_BIAS_STATS bindings=%llu avoided_snapshot_bytes=%llu\n",(unsigned long long)G.profile_storage_bias_bindings,(unsigned long long)G.profile_storage_bias_bytes);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] QUERY_STATS queries=%u counter_ops=%d\n",S.nocc,S.ncounter);
    if(getenv("DM_RESOURCE_STATS"))command_state_profile("COMMAND_STATE_STATS",G.present_count+1,G.profile_frames);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] PSO_LOOKUP_STATS last_cache=%d lookups=%llu hash_calls=%llu last_hits=%llu last_misses=%llu\n",
        G.enable_pso_last,(unsigned long long)G.profile_pso_lookups,(unsigned long long)G.profile_pso_hash_calls,
        (unsigned long long)G.profile_pso_last_hits,(unsigned long long)G.profile_pso_last_misses);
    if(getenv("DM_RESOURCE_STATS"))dirty_query_profile("DIRTY_QUERY_STATS",G.present_count+1,G.profile_frames,0);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] SUBMISSION_POOL_STATS epoch=%llu ticket=%llu phase=%u creates=%llu reuses=%llu command=%llx descriptors=%llx query=%llx fence=%llx gpu_complete=1 effects_published=1\n",
        (unsigned long long)S.epoch,(unsigned long long)S.ticket,(unsigned)S.phase,(unsigned long long)G.profile_submission_creates,(unsigned long long)G.profile_submission_reuses,
        (unsigned long long)(uintptr_t)S.command,(unsigned long long)(uintptr_t)S.descriptors,(unsigned long long)(uintptr_t)S.timestamps,(unsigned long long)(uintptr_t)S.gpu_fence);
    uint64_t ticket=S.ticket;submission_released(&S);G.profile_retire_cpu_ns+=dm_android_nvn_thread_cpu_ns()-retire_cpu;return ticket;
}
static void present_after_gpu(void) {
    int published=G.slot_publication&&S.phase==SUBMISSION_PUBLISHED;
    if(published)cov_probe_complete(&S);else submission_gpu_done(&S);
    pass_timing_collect(&S);trace_capture_complete(&S);
    vkDestroySemaphore(G.device,S.present_available,NULL);S.present_available=VK_NULL_HANDLE;
    resource_cache_retire(&S);
    temporaries_retire(&S);
    if(S.nwrites || S.nevents || S.ncounter)die("present has unpublished guest effects",S.epoch);
    if(!published)submission_publish(&S);submission_released(&S);
}
#include "nvn_vk_publication_runtime.inc"
/* Completion worker: publishes finished submissions strictly in submission order while the guest thread records
 * the next frame. It runs the same completion code as the synchronous path with G.lock held. */
static void *async_worker(void *arg) {
    (void)arg;
    for(;;) {
        pthread_mutex_lock(&G.lock);
        while(!g_jcount)pthread_cond_wait(&g_job_cv,&G.lock);
        AsyncJob job=g_jobs[g_jhead];
        pthread_mutex_unlock(&G.lock);
        VkFence fence=job.ctx->gpu_fence;
        uint64_t wait_started=dm_android_nvn_clock_ns();
        CHECK(vkWaitForFences(G.device,1,&fence,VK_TRUE,UINT64_MAX));
        uint64_t waited=dm_android_nvn_clock_ns()-wait_started;
        pthread_mutex_lock(&G.lock);
        G.profile_completion_wait_ns+=waited;G.profile_completion_waits++;
        VkSubmission *saved=G.current_submission;int saved_active=G.active;G.current_submission=job.ctx;G.completing=1;job.ctx->recorder=pthread_self();
        if(job.kind==0){CommitInfo ci;memcpy(&ci,&job.ctx->commit_info,sizeof ci);ci.wait_cpu=dm_android_nvn_thread_cpu_ns();commit_after_gpu(ci,0);}
        else present_after_gpu();
        G.completing=0;G.current_submission=saved;G.active=saved_active;
        g_jhead=(g_jhead+1)%8;g_jcount--;
        pthread_cond_broadcast(&g_done_cv);
        pthread_mutex_unlock(&G.lock);
    }
}
static void async_enqueue_locked(VkSubmission *ctx,int kind) {
    if(G.slot_publication){publication_enqueue(ctx,kind);return;}
    if(g_jcount==8)die("async job queue overflow",g_jcount);
    if(!g_worker_up){pthread_t thread;if(pthread_create(&thread,NULL,async_worker,NULL))die("async worker creation",0);pthread_detach(thread);g_worker_up=1;}
    g_jobs[(g_jhead+g_jcount)%8]=(AsyncJob){ctx,kind};g_jcount++;
    pthread_cond_signal(&g_job_cv);
}
/* Submit the recorded segment and return without waiting; the worker waits for the fence and publishes effects. */
void mtl_sub_commit_async(void) {
    if(!G.async_enabled){mtl_sub_commit();return;}
    CommitInfo ci=commit_submit();
    memcpy(&S.commit_info,&ci,sizeof ci);G.active=0;atomic_store(&G.recording_active,0);
    async_enqueue_locked(&S,0);
    if(G.async_drain_after_submit)async_drain_locked();
    pthread_mutex_unlock(&G.lock);
}
int mtl_async_enabled(void) { mtl_init();return G.async_enabled; }
uint64_t mtl_last_fence(void) { return G.fence; }
int mtl_last_commit_error(void) { return 0; } /* Errors terminate at CHECK before completion is published. */
int mtl_wait(uint64_t fence,uint64_t timeout) {
    if(fence<=atomic_load(&G.completed))return 1;
    if(!timeout)return 0;
    uint64_t start=dm_android_nvn_clock_ns();
    while(fence>atomic_load(&G.completed)){
        if(timeout!=UINT64_MAX&&dm_android_nvn_clock_ns()-start>=timeout)return 0;
        struct timespec pause={0,100000};nanosleep(&pause,NULL);
    }
    return 1;
}
void mtl_finish(void) { mtl_init(); pthread_mutex_lock(&G.lock); async_drain_locked(); CHECK(cpu_work_queue_idle(G.queue)); pipeline_cache_checkpoint(0); pthread_mutex_unlock(&G.lock); }
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
    case NVN_FORMAT_RGB10A2SN:return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case NVN_FORMAT_RGB10A2I:return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case NVN_FORMAT_RGB10A2_UI2F:return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case NVN_FORMAT_RGB10A2_I2F:return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
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
/* Access types implied by a layout. Read-only layouts carry no source access: earlier writes were made visible
 * to every read type when the image entered that layout. */
static VkAccessFlags layout_access(VkImageLayout layout,int as_source) {
    const VkAccessFlags reads=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    switch(layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:return 0;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:return as_source?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:return as_source?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:return as_source?VK_ACCESS_TRANSFER_WRITE_BIT:(VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT);
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:return as_source?0:reads;
    default:return VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    }
}
static void texture_barrier_levels(VTexture *t,VkImageLayout old,VkImageLayout layout,uint32_t base,uint32_t count) {
    pass_end();
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
       .srcAccessMask=old==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,
       .dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,.oldLayout=old,.newLayout=layout,
       .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->image,
       .subresourceRange={aspect(t->format),base,count,0,t->source.L.layers}};
    if(G.precise_sync){
        VkPipelineStageFlags src=old==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
            (HZ_ATT_STAGES|HZ_SHADER_STAGES|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        VkPipelineStageFlags dst=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        switch(layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:dst=HZ_ATT_STAGES;break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            dst=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            if(G.xition_vertex)dst|=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:dst=VK_PIPELINE_STAGE_TRANSFER_BIT;break;
        default:break;
        }
        if(G.enable_narrow_access){b.srcAccessMask=layout_access(old,1);b.dstAccessMask=layout_access(layout,0);}
        if(G.hz_diag&1){src=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;dst=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;}
        vkCmdPipelineBarrier(S.command,src,dst,0,0,NULL,0,NULL,1,&b);
        G.profile_hz_barriers++;G.hz_count[4+(layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL?0:(dst==HZ_ATT_STAGES?1:(dst==VK_PIPELINE_STAGE_TRANSFER_BIT?2:3)))]++;
    }else
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
}
/* Whole-image transitions unify any per-mip layouts first; per-mip transitions let a mip chain be sampled and rendered
 * in place without the sampling copy that devices lacking feedback layouts would otherwise need. */
static void texture_transition(VTexture *t,VkImageLayout layout) {
    if(!t->mixed){if(t->layout==layout)return;texture_barrier_levels(t,t->layout,layout,0,t->source.d.levels);t->layout=layout;return;}
    uint32_t n=t->source.d.levels;
    for(uint32_t i=0;i<n;){if(t->lvl[i]==layout){i++;continue;}uint32_t j=i+1;while(j<n&&t->lvl[j]==t->lvl[i])j++;texture_barrier_levels(t,t->lvl[i],layout,i,j-i);i=j;}
    t->mixed=0;t->layout=layout;
}
static int texture_range_ok(const VTexture *t) {
    return !G.disable_subres&&!G.feedback_layout&&t->source.d.levels>1&&t->source.d.levels<=16&&t->source.L.layers==1&&
        t->source.d.target==NVN_TEXTURE_TARGET_2D&&aspect(t->format)==VK_IMAGE_ASPECT_COLOR_BIT;
}
static void texture_transition_levels(VTexture *t,uint32_t base,uint32_t count,VkImageLayout layout) {
    uint32_t n=t->source.d.levels;
    if(base==0&&count>=n){texture_transition(t,layout);return;}
    if(!t->mixed){if(t->layout==layout)return;for(uint32_t i=0;i<n;i++)t->lvl[i]=t->layout;t->mixed=1;}
    for(uint32_t i=base;i<base+count;){if(t->lvl[i]==layout){i++;continue;}VkImageLayout old=t->lvl[i];uint32_t j=i+1;while(j<base+count&&t->lvl[j]==old)j++;
        texture_barrier_levels(t,old,layout,i,j-i);for(uint32_t k=i;k<j;k++)t->lvl[k]=layout;i=j;}
    uint32_t k=1;while(k<n&&t->lvl[k]==t->lvl[0])k++;
    if(k==n){t->mixed=0;t->layout=t->lvl[0];}
}
#include "nvn_vk_texture_upload.inc"
#include "nvn_vk_texture_refresh.inc"
/* Resident slots never move. Both the descriptor registry and shader plans
 * retain slot+incarnation references; deleting/reusing a slot cannot inherit
 * an old image. The serial table is used only to acquire a new reference. */
static uint32_t texture_table_slot(uint64_t serial,int *found) {
    uint64_t hash=serial;hash^=hash>>30;hash*=0xbf58476d1ce4e5b9ULL;hash^=hash>>27;
    uint32_t first=UINT32_MAX,slot=(uint32_t)hash&(2*LIMIT_OBJECTS-1);
    for(unsigned n=0;n<2*LIMIT_OBJECTS;n++,slot=(slot+1)&(2*LIMIT_OBJECTS-1)){
        uint32_t entry=G.texture_table[slot];
        if(!entry){*found=0;return first==UINT32_MAX?slot:first;}
        if(entry==UINT32_MAX){if(first==UINT32_MAX)first=slot;continue;}
        if(G.textures[entry-1].serial==serial){*found=1;return slot;}
    }
    if(first!=UINT32_MAX){*found=0;return first;}
    die("texture registry capacity",serial);return 0;
}
static TextureRef texture_reference(VTexture *t) {
    return (TextureRef){(uint32_t)(t-G.textures),t->incarnation};
}
static VTexture *texture_reference_get(TextureRef ref) {
    if(!ref.incarnation||ref.slot>=G.texture_used)return NULL;
    VTexture *t=&G.textures[ref.slot];return t->serial&&t->incarnation==ref.incarnation?t:NULL;
}
#include "nvn_vk_image_usage.inc"
static VTexture *texture_acquire(const tex_t *source,int upload,TextureRef ref) {
    if(!source || !source->serial) die("texture serial",0);
    if(source->d.levels<1||source->d.levels>16)die("texture mip count",source->d.levels);
    VTexture *t=texture_reference_get(ref);int found=0;uint32_t registry_slot=0;
    if(t&&t->serial!=source->serial)die("texture reference serial",source->serial);
    if(!t){registry_slot=texture_table_slot(source->serial,&found);if(found)t=&G.textures[G.texture_table[registry_slot]-1];}
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
    uint32_t resident_slot=G.texture_free_count?G.texture_free[--G.texture_free_count]:G.texture_used++;
    if(resident_slot>=LIMIT_OBJECTS)die("texture registry slot",resident_slot);
    t=&G.textures[resident_slot];memset(t,0,sizeof(*t));t->serial=source->serial;t->incarnation=++G.texture_incarnation;
    G.ntexture++;G.texture_table[registry_slot]=resident_slot+1;t->source=*source;t->version=1;
    t->width=source->d.width; t->height=one?1:source->d.height;t->depth=volume?source->d.depth:1;t->layout=VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t metal_format=0; int decompressed_bpp=0;
    int bc=nvn_bc_unpacked(source->d.format,&metal_format,&decompressed_bpp);
    if(bc&&texture_native_bc(source,&t->format,&t->bc_block))t->bpp=decompressed_bpp;
    else if(bc) {
        t->bc_block=0;
        switch(metal_format) { case 10:t->format=VK_FORMAT_R8_UNORM;break;case 12:t->format=VK_FORMAT_R8_SNORM;break;
          case 30:t->format=VK_FORMAT_R8G8_UNORM;break;case 32:t->format=VK_FORMAT_R8G8_SNORM;break;
          case 70:t->format=VK_FORMAT_R8G8B8A8_UNORM;break;case 71:t->format=VK_FORMAT_R8G8B8A8_SRGB;break;
          case 115:t->format=VK_FORMAT_R16G16B16A16_SFLOAT;break;default:die("BC decoded format",metal_format); }
        t->bpp=decompressed_bpp;
    } else { t->format=format(source->d.format); t->bpp=source->L.bpb; }
    VkFormatProperties fp=format_properties(t->format);
    VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(aspect(t->format)&VK_IMAGE_ASPECT_DEPTH_BIT){usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;if(G.depth_order)usage|=VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;}
    else if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int attachment_image=(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))!=0;
    /* Retain STORAGE for guest IMAGE textures. A first late storage binding
     * otherwise upgrades the allocation, preserving all subresources. */
    if((fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)&&
       (!(G.image_usage_by_flags||(G.diag_img_usage>=1&&attachment_image))||(source->d.flags&NVN_TEXTURE_FLAGS_IMAGE)))usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    if(G.feedback_layout && attachment_image && G.diag_img_usage<2)usage|=VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    t->usage=usage;
    if(usage&VK_IMAGE_USAGE_STORAGE_BIT)G.image_usage_storage++;
    else if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)G.image_usage_omitted++;
    texture_image_allocate(t,usage,&t->image,&t->memory,&t->view,&t->allocation_size);
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
            memcpy(snapshot,source->mem,source->L.size);texture_snapshot_set(t,snapshot,source->L.size);upload_memory=snapshot;}
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        uint64_t decode_start=dm_android_nvn_clock_ns();
        texture_upload(t,source,upload_memory,upload_bc);
        G.profile_decode_ns+=dm_android_nvn_clock_ns()-decode_start;
        t->cpu_owned=1;t->upload_stamp=stamp;tex_dirty(t); G.uploads++;
    }
    return t;
}
static VTexture *texture_get(const tex_t *source,int upload) {
    return texture_acquire(source,upload,(TextureRef){0});
}
static VTexture *slot_texture(TSlot *slot,int upload) {
    /* Ghost candidates retain the original draw's image incarnation/version.
     * Re-uploading CPU storage here could both change that image and end the
     * pass recursively; replay samples the already validated resident image. */
    if(ghost.busy)upload=0;
    VTexture *t=texture_acquire(&slot->texture,upload,slot->resident);slot->resident=texture_reference(t);return t;
}
static uint64_t texture_derived_release(VTexture *t) {
    uint64_t freed=(t->shadow?t->shadow_allocation_size:0)+(t->sample_copy?t->sample_allocation_size:0);
    if(t->shadow){vkDestroyImageView(G.device,t->shadow_view,NULL);vkDestroyImage(G.device,t->shadow,NULL);
        memory_release(t->shadow_memory,t->shadow_allocation_size,DM_VK_MEMORY_SHADOW);}
    if(t->sample_copy){vkDestroyImageView(G.device,t->sample_view,NULL);vkDestroyImage(G.device,t->sample_copy,NULL);
        memory_release(t->sample_memory,t->sample_allocation_size,DM_VK_MEMORY_SAMPLE_COPY);}
    t->shadow=VK_NULL_HANDLE;t->shadow_view=VK_NULL_HANDLE;t->shadow_memory=VK_NULL_HANDLE;t->shadow_allocation_size=0;
    t->shadow_layout=VK_IMAGE_LAYOUT_UNDEFINED;t->shadow_version=0;
    t->sample_copy=VK_NULL_HANDLE;t->sample_view=VK_NULL_HANDLE;t->sample_memory=VK_NULL_HANDLE;t->sample_allocation_size=0;
    t->sample_layout=VK_IMAGE_LAYOUT_UNDEFINED;t->sample_other=0;t->sc_valid=0;
    t->sc_level=t->sc_levels=t->sc_layer=t->sc_layers=0;
    return freed;
}
void mtl_tex_release(tex_t *source) {
    if(G.active) die("texture release during submission",source->serial);
    pthread_mutex_lock(&G.lock);async_drain_locked();int found=0;uint32_t slot=texture_table_slot(source->serial,&found);
    if(found){VTexture *t=&G.textures[G.texture_table[slot]-1];
       /* Both slots have completed and published after async_drain_locked.
        * Clear dependent framebuffers/views before destroying any image. */
       resource_cache_clear(&G.submission_slot);resource_cache_clear(&G.submission_slot2);for(int i=1;i<G.render_slots_n;i++)resource_cache_clear(&G.extra_render_slots[i-1]);
       if(t->before_storage){vkDestroyImageView(G.device,t->before_storage_view,NULL);vkDestroyImage(G.device,t->before_storage,NULL);memory_release(t->before_storage_memory,t->before_storage_allocation_size,DM_VK_MEMORY_TEXTURE);G.image_retired_live--;}
       texture_snapshot_set(t,NULL,0);texture_derived_release(t);
       vkDestroyImageView(G.device,t->view,NULL); vkDestroyImage(G.device,t->image,NULL); memory_release(t->memory,t->allocation_size,DM_VK_MEMORY_TEXTURE);
       G.texture_free[G.texture_free_count++]=(uint32_t)(t-G.textures);t->serial=0;G.ntexture--;G.texture_table[slot]=UINT32_MAX;G.registry_revision++;
    }
    pthread_mutex_unlock(&G.lock);
}
void mtl_reg_tex(int id,const tex_t *t,const view_t *v) {
    mtl_init();pthread_mutex_lock(&G.lock);
    if(id<0 || id>=G.texture_pool_n || (size_t)id>=G.texture_slots)die("texture descriptor id",id); G.tslot[id].live=t!=NULL;
    if(t) G.tslot[id].texture=*t; G.tslot[id].has_view=v!=NULL; if(v)G.tslot[id].view=*v;
    G.tslot[id].resident=(TextureRef){0};G.tslot[id].revision=++G.registry_revision;pthread_mutex_unlock(&G.lock);
}
static VkSamplerAddressMode wrap(unsigned w) { switch(w) {
    case NVN_WRAP_MODE_REPEAT:return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:if(!G.mirror_clamp)die("sampler mirror clamp extension unavailable",w);return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_CLAMP_TO_EDGE:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;default:die("sampler wrap mode",w); } }
void mtl_reg_samp(int id,const samp_par *p) {
    mtl_init();pthread_mutex_lock(&G.lock); if(id<0 || id>=LIMIT_OBJECTS || !p)die("sampler descriptor id",id); SSlot *s=&G.sslot[id];
    async_drain_locked();if(s->live) { if(G.active)die("sampler re-register in flight",id); vkDestroySampler(G.device,s->sampler,NULL);
        if(s->nearest)vkDestroySampler(G.device,s->nearest,NULL);
        if(s->integer_nearest)vkDestroySampler(G.device,s->integer_nearest,NULL);
        s->nearest=s->integer_nearest=VK_NULL_HANDLE; }
    s->source=*p;
    VkSamplerCreateInfo i={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
       .magFilter=p->magf==NVN_MAG_FILTER_NEAREST?VK_FILTER_NEAREST:VK_FILTER_LINEAR,
       .minFilter=(p->minf==NVN_MIN_FILTER_NEAREST || p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST || p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR)?VK_FILTER_NEAREST:VK_FILTER_LINEAR,
       .mipmapMode=(p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf==NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR)?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,
       .addressModeU=wrap(p->ws),.addressModeV=wrap(p->wt),.addressModeW=wrap(p->wr),.minLod=p->lod_min,.maxLod=p->lod_max,
       .mipLodBias=p->lod_bias+(getenv("DM_DIAG_LOD_BIAS")?(float)atof(getenv("DM_DIAG_LOD_BIAS")):0.f),.anisotropyEnable=p->aniso>1,.maxAnisotropy=p->aniso>1?p->aniso:1,
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
    nvn_vk_sampler_limits(&i,&G.enabled_features,&G.properties.limits);
    s->info=i;
    CHECK(vkCreateSampler(G.device,&i,NULL,&s->sampler)); s->live=1;G.registry_revision++;pthread_mutex_unlock(&G.lock);
}
static VkSampler sampler_for_format(SSlot *slot,VkFormat format) {
    VkSamplerCreateInfo info=slot->info;VkFormatProperties properties=format_properties(format);
    if(!nvn_vk_sampler_filter(&info,format,properties.optimalTilingFeatures))return slot->sampler;
    VkSampler *variant=nvn_vk_format_integer(format)?&slot->integer_nearest:&slot->nearest;
    if(!*variant){CHECK(vkCreateSampler(G.device,&info,NULL,variant));
        fprintf(stderr,"[nvn-vulkan] SAMPLER_FALLBACK format=%u integer=%d\n",format,nvn_vk_format_integer(format));}
    return *variant;
}
void mtl_set_tex_pool_n(int n) {
    mtl_init();pthread_mutex_lock(&G.lock);
    /* NVN handles encode a 20-bit texture descriptor ID. Descriptor slots are
     * CPU metadata, independent of the number of resident Vulkan images. */
    if(n<0 || n>(1<<20))die("texture pool capacity",n);
    if((size_t)n>G.texture_slots) {
        TSlot *slots=realloc(G.tslot,(size_t)n*sizeof(*slots));
        if(!slots)die("texture descriptor allocation",n);
        memset(slots+G.texture_slots,0,((size_t)n-G.texture_slots)*sizeof(*slots));
        G.tslot=slots;G.texture_slots=(size_t)n;
    }
    G.texture_pool_n=n;G.registry_revision++;
    fprintf(stderr,"[nvn-vulkan] texture descriptor slots=%d resident image limit=%d\n",n,LIMIT_OBJECTS);
    pthread_mutex_unlock(&G.lock);
}

static uint8_t *read_file(const char *name,size_t *size) {
    char path[1200]; if(!G.shader_root[0] || snprintf(path,sizeof(path),"%s/%s",G.shader_root,name)>=(int)sizeof(path))die("shader cache path",0);
    FILE *f=fopen(path,"rb"); if(!f)die(path,errno); if(fseek(f,0,SEEK_END))die("shader cache seek",0);
    /* Full-code indexes include original NVN bytecode for every program;
     * the complete title cache exceeds the per-module payload limit. */
    long limit=!strcmp(name,"shader_index.bin")?128L*1024*1024:64L*1024*1024;
    long n=ftell(f); if(n<0 || n>limit || fseek(f,0,SEEK_SET))die("shader cache file size",n);
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
void dm_vk_program_capture(const NVNshaderData *shaders,int count) {
    const char *root=getenv("DM_SHADER_CAPTURE_ROOT");if(!root)return;
    pthread_mutex_lock(&G.lock);
    static unsigned sequence;
    for(int i=0;i<count;i++) {
        const uint8_t *control=shaders[i].control;uint32_t size,offset;
        if(!control)die("capture null shader control",i);
        memcpy(&size,control+0x704,4);memcpy(&offset,control+0x708,4);
        uint64_t pool_offset=0;VPool *pool=pool_find(shaders[i].data,size,&pool_offset);
        if(!size || size>16*1024*1024 || !pool)die("capture invalid shader pool range",shaders[i].data);
        const uint8_t *code=pool->guest+pool_offset;
        char file[1200];unsigned current=sequence++;
        fprintf(stderr,"[nvn-vulkan] SHADER_CAPTURE sequence=%u address=%#llx size=%u program_offset=%u first=%02x%02x%02x%02x\n",current,(unsigned long long)shaders[i].data,size,offset,code[0],code[1],code[2],code[3]);
        snprintf(file,sizeof file,"%s/shader-%u.control.bin",root,current);FILE *f=fopen(file,"wb");
        if(!f || fwrite(control,1,0x880,f)!=0x880 || fclose(f))die("capture control write",current);
        snprintf(file,sizeof file,"%s/shader-%u.code.bin",root,current);f=fopen(file,"wb");
        if(!f || fwrite(code,1,size,f)!=size || fclose(f))die("capture code write",current);
    }
    pthread_mutex_unlock(&G.lock);
}
static uint64_t control_hash(const void *control,uint32_t stage) {
    const uint8_t *bytes=control;uint64_t hash=14695981039346656037ULL^stage;
    for(unsigned i=0;i<0x880;i++){hash^=bytes[i];hash*=1099511628211ULL;}return hash;
}
static uint64_t shader_code_hash(uint64_t hash,const uint8_t *code,uint32_t size) {
    for(uint32_t i=0;i<size;i++){hash^=code[i];hash*=1099511628211ULL;}return hash;
}
static const uint8_t *indexed_control(const uint8_t *entry) {return entry+(G.shader_index_version==2?24:20);}
static BindingPlan *binding_plan_compile(VShader *s) {
    size_t bytes=sizeof(BindingPlan)+(size_t)s->h.nb*sizeof(BindingResource);
    BindingPlan *plan=calloc(1,bytes);if(!plan)die("shader binding plan allocation",bytes);
    plan->eligible=1;
    for(uint32_t i=0;i<s->h.nb;i++){
        const RBinding *b=&s->b[i];
        if(b->kind==2||b->kind==3||b->kind==9||b->kind==10)plan->buffers[plan->nbuffer++]=(uint8_t)i;
        else if(b->kind==4||b->kind==6||b->kind==11)plan->images[plan->nimage++]=(uint8_t)i;
        else if(b->kind==8)plan->drivers[plan->ndriver++]=(uint8_t)i;
        if(b->kind==1||b->kind==7)continue;
        if(b->kind==2&&b->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&b->slot>=0&&b->slot<16)continue;
        if(b->kind==4&&(b->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||b->type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)&&b->slot>=0&&b->slot<128&&!(s->dref_bindings&(1ull<<i)))continue;
        plan->eligible=0;
    }
    return plan;
}
static const uint8_t *shader_entry(const void *control,uint32_t stage,uint64_t address) {
    if(!control)die("null shader control",stage);
    if(!G.shader_index) { G.shader_index=read_file("shader_index.bin",&G.index_bytes);
        if(G.index_bytes<12)die("shader index header",G.index_bytes); uint32_t *h=(uint32_t *)G.shader_index;
        if(h[0]!=0x494b5644 || (h[1]!=1&&h[1]!=2) || (h[1]==1 && G.index_bytes!=12+(size_t)h[2]*(20+0x880)))die("shader index schema",h[0]);
        G.shader_index_version=h[1];
        if(G.shader_code_identity && h[1]!=2)die("title requires full-code shader identity",h[1]); }
    if(!G.shader_entries){
        uint32_t count=((uint32_t *)G.shader_index)[2];if(count>1000000)die("shader control index count",count);
        size_t capacity=16;while(capacity<(size_t)count*2)capacity*=2;
        G.shader_entries=calloc(capacity,sizeof(*G.shader_entries));if(!G.shader_entries)die("shader control index allocation",capacity);
        G.shader_entry_capacity=capacity;size_t cursor=12;
        for(uint32_t i=0;i<count;i++){
            nvn_shader_index_record record;
            if(!nvn_shader_index_read(G.shader_index,G.index_bytes,G.shader_index_version,&cursor,&record))die("shader index record validation",i);
            const uint8_t *entry=record.entry;
            uint64_t hash=control_hash(record.control,record.stage);
            if(record.code_size)hash=shader_code_hash(hash,record.code,record.code_size);
            size_t slot=hash&(capacity-1);
            while(G.shader_entries[slot].entry){const uint8_t *old=G.shader_entries[slot].entry;
                if(G.shader_entries[slot].hash==hash){size_t old_cursor=(size_t)(old-G.shader_index);nvn_shader_index_record previous;
                    if(!nvn_shader_index_read(G.shader_index,G.index_bytes,G.shader_index_version,&old_cursor,&previous))die("shader index previous record",i);
                    if(nvn_shader_index_equal(&record,&previous))break;}
                slot=(slot+1)&(capacity-1);}
            /* Preserve the first matching verified entry, including duplicates. */
            if(!G.shader_entries[slot].entry)G.shader_entries[slot]=(ShaderEntry){entry,hash};
        }
        if(cursor!=G.index_bytes)die("shader index trailing data",cursor);
        fprintf(stderr,"[nvn-vulkan] SHADER_CONTROL_INDEX version=%u entries=%u slots=%zu bytes=%zu full_code_verified=%d\n",G.shader_index_version,count,capacity,capacity*sizeof(*G.shader_entries),G.shader_index_version==2);
    }
    uint32_t code_size=0;const uint8_t *code=NULL;uint64_t code_pool_serial=0;
    ShaderLookup *memo=NULL;
    if(G.shader_index_version==2) {
        uint32_t nvn_stage;memcpy(&nvn_stage,(const uint8_t *)control+0x714,4);
        if(nvn_stage!=stage)return NULL;
        memcpy(&code_size,(const uint8_t *)control+0x704,4);uint64_t offset=0;
        VPool *pool=pool_find(address,code_size,&offset);
        if(!pool || !code_size || code_size>16*1024*1024)die("shader identity GPU code range",address);
        code=pool->guest+offset;code_pool_serial=pool->serial;
        uint64_t mix=(((uint64_t)(uintptr_t)control>>4)^(address*0x9e3779b97f4a7c15ULL)^((uint64_t)stage<<56))*0xbf58476d1ce4e5b9ULL;
        memo=&G.shader_lookup[(mix>>40)&(SHADER_LOOKUP-1)];
        if(memo->entry&&memo->control==control&&memo->address==address&&memo->stage==stage&&memo->code_size==code_size) {
            /* Same verification as the hashed lookup below, without hashing ~2 KB of control and the whole code. */
            const uint8_t *ctrl=indexed_control(memo->entry);
            /* The shader code is static game data that sits in a tracked memory pool. When none of its pages was written
             * since the last full comparison, the comparison would give the same answer. DM_DISABLE_SHADER_STAMP
             * restores the byte comparison on every draw. */
            int code_same;
            if(memo->stamp&&memo->pool_serial==pool->serial&&!G.disable_shader_stamp&&!nvn_dirty_since(code,code_size,memo->stamp)){
                code_same=1;G.profile_shader_stamp_hits++;
            }else{
                uint64_t stamp=0;
                if(!G.disable_shader_stamp&&nvn_dirty_tracked(code,code_size)){stamp=nvn_dirty_now();nvn_dirty_protect(code,code_size);}
                code_same=!memcmp(ctrl+0x880,code,code_size);
                memo->stamp=code_same?stamp:0;memo->pool_serial=pool->serial;
            }
            if(code_same&&!memcmp(ctrl,control,0x880)){G.profile_control_hits++;return memo->entry;}
        }
        G.profile_control_misses++;
    }
    uint64_t hash=control_hash(control,stage);
    if(code)hash=shader_code_hash(hash,code,code_size);
    size_t slot=hash&(G.shader_entry_capacity-1);
    while(G.shader_entries[slot].entry){ShaderEntry *candidate=&G.shader_entries[slot];uint32_t st;memcpy(&st,candidate->entry+16,4);
        const uint8_t *ctrl=indexed_control(candidate->entry);
        if(candidate->hash==hash&&st==stage&&!memcmp(ctrl,control,0x880)) {
            uint32_t stored=0;if(G.shader_index_version==2)memcpy(&stored,candidate->entry+20,4);
            if(G.shader_index_version==1 || (stored==code_size && !memcmp(ctrl+0x880,code,code_size))) {
                if(memo)*memo=(ShaderLookup){control,address,candidate->entry,stage,code_size,code_pool_serial,0};
                return candidate->entry;
            }
        }
        slot=(slot+1)&(G.shader_entry_capacity-1);}
    return NULL;
}
/* The compiler worker owns its shader copies; it never touches replay state,
 * the live shader registry, C1 buffers or submission resources. */
static void shader_load_into(VShader *s,const char *key,uint32_t stage,int warm) {
    strcpy(s->key,key);
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
       if(cap==4427 && stage==0 && (G.shader_draw_parameters||G.portable_draw_parameters))continue; /* Actual draw arguments, native or specialized. */
       if(cap==25 && G.shader_image_gather_extended)continue;
       /* Check actual clip/cull declarations after portable normalization;
        * the cache may contain unused per-vertex block members. */
       if(cap==32 || cap==33)continue;
       if(cap!=1 && cap!=46 && cap!=47 && cap!=50 && cap!=55 && cap!=56){fprintf(stderr,"[nvn-vulkan] shader feature key=%s stage=%u cap=%u\n",key,stage,cap);die("shader feature needs device/semantic adaptation",cap);}
    }
    if(s->h.c1n&&!warm){s->c1=buffer_make(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);memcpy(s->c1.map,p,s->h.c1n);}free(bytes);
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
    if(!G.shader_clip_distance||!G.shader_cull_distance){
        const char *error=NULL;size_t words=0;uint32_t removed=0;
        if(stage==0&&!G.shader_clip_distance){
            uint32_t *module=nvn_vk_redundant_clip((const uint32_t *)bytes,size/4,&words,&s->requires_native_depth_clip,&error);
            if(!module){fprintf(stderr,"[nvn-vulkan] CLIP_PROOF_REJECT shader=%s reason=%s\n",s->key,error);die("redundant clip module",0);}
            if(words!=size/4)fprintf(stderr,"[nvn-vulkan] REDUNDANT_CLIP_REMOVED shader=%s native_depth_clip_required=%d\n",s->key,s->requires_native_depth_clip);
            free(bytes);bytes=(uint8_t *)module;size=words*4;
        }
        uint32_t *module=nvn_vk_unused_distance((const uint32_t *)bytes,size/4,&words,&removed,&error);
        if(!module){fprintf(stderr,"[nvn-vulkan] UNUSED_DISTANCE_REJECT shader=%s reason=%s\n",s->key,error);die("unused distance module",0);}
        if(removed)fprintf(stderr,"[nvn-vulkan] UNUSED_DISTANCE_REMOVED shader=%s mask=%u\n",s->key,removed);
        free(bytes);bytes=(uint8_t *)module;size=words*4;
    }
    size_t distance_words=0;uint32_t distance_mask=0;const char *distance_error=NULL;
    uint32_t *distance_module=nvn_vk_distance_capabilities((const uint32_t *)bytes,size/4,&distance_words,&distance_mask,&distance_error);
    if(!distance_module){fprintf(stderr,"[nvn-vulkan] DISTANCE_CAPABILITY_REJECT shader=%s reason=%s\n",s->key,distance_error);die("distance capability module",0);}
    if(((distance_mask&NVN_DISTANCE_CLIP)&&!G.shader_clip_distance)||((distance_mask&NVN_DISTANCE_CULL)&&!G.shader_cull_distance)){
        fprintf(stderr,"[nvn-vulkan] DISTANCE_FEATURE_UNAVAILABLE shader=%s stage=%u required=%u\n",s->key,stage,distance_mask);
        die("shader clip/cull distance feature unavailable",distance_mask);
    }
    if(distance_words!=size/4)fprintf(stderr,"[nvn-vulkan] DISTANCE_CAPABILITY_REPAIR shader=%s mask=%u\n",s->key,distance_mask);
    free(bytes);bytes=(uint8_t *)distance_module;size=distance_words*4;
    if(stage==0){
        const uint32_t *w=(const uint32_t *)bytes;uint32_t bound=w[3];if(!bound||bound>1000000)die("vertex input type bound",bound);
        uint8_t *kind=calloc(bound,1),*input=calloc(bound,1);uint32_t *location=malloc((size_t)bound*4);
        if(!kind||!input||!location)die("vertex input reflection allocation",bound);
        for(uint32_t i=0;i<bound;i++)location[i]=UINT32_MAX;
        for(size_t at=5;at<size/4;){uint32_t n=w[at]>>16,op=w[at]&0xffff;const uint32_t *a=w+at;
            if(!n||n>size/4-at)die("vertex input reflection shape",at);
            if(op==71&&n==4&&a[1]<bound&&a[2]==30)location[a[1]]=a[3];
            if(op==22&&n==3&&a[1]<bound&&a[2]==32)kind[a[1]]=1;
            if(op==21&&n==4&&a[1]<bound&&a[2]==32)kind[a[1]]=a[3]?2:3;
            if(op==23&&n==4&&a[1]<bound&&a[2]<bound)kind[a[1]]=kind[a[2]];
            if(op==32&&n==4&&a[1]<bound&&a[3]<bound)kind[a[1]]=kind[a[3]];
            if(op==59&&n>=4&&a[1]<bound&&a[2]<bound&&a[3]==1){kind[a[2]]=kind[a[1]];input[a[2]]=1;}
            at+=n;
        }
        for(uint32_t i=0;i<s->h.na;i++){uint32_t found=0;
            for(uint32_t id=1;id<bound;id++)if(input[id]&&location[id]==s->attr[i][0]){if(!kind[id]||found)die("vertex input numeric type",id);s->attr[i][1]=kind[id]-1;found=1;}
            if(!found)die("vertex attribute reflection missing",s->attr[i][0]);
        }
        free(kind);free(input);free(location);
    }
    bytes=buffer_image_module(bytes,&size,s);
    if(stage==5&&nvn_vk_quirk_enabled(&G.quirks,NVN_VK_QUIRK_LOWER_COMPUTE_BALLOT)){
        unsigned ballots=0;uint8_t *adapted=dm_compute_ballot_apply(bytes,&size,&ballots);
        if(!adapted)die("compute ballot adaptation bounds/interface",0);bytes=adapted;
        if(ballots)fprintf(stderr,"[nvn-vulkan] COMPUTE_BALLOT_REDUCE shader=%s ballots=%u driver=Qualcomm bytes=%zu\n",key,ballots,size);
    }
    if((stage==0||stage==1)&&nvn_vk_quirk_enabled(&G.quirks,NVN_VK_QUIRK_LOWER_SKIN_UBO)){
        unsigned accesses=0;uint64_t skip_bindings=0;for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==8&&s->b[i].binding<64)skip_bindings|=1ull<<s->b[i].binding;
        uint8_t *adapted=dm_skin_ubo_apply(bytes,&size,&accesses,skip_bindings);
        if(!adapted)die("skin UBO adaptation bounds/interface",0);bytes=adapted;
        if(accesses)fprintf(stderr,"[nvn-vulkan] SKIN_UBO_VEC4_EXTRACT shader=%s accesses=%u driver=Qualcomm bytes=%zu\n",key,accesses,size);
    }
    NvnVkImageType image_types[64];size_t image_count=0;const char *image_error=NULL;
    if(!nvn_vk_image_types((const uint32_t *)bytes,size/4,image_types,&image_count,&image_error)){
        fprintf(stderr,"[nvn-vulkan] IMAGE_TYPE_REFLECTION_REJECT shader=%s reason=%s\n",s->key,image_error);
        die("shader image type reflection",0);
    }
    for(size_t j=0;j<image_count;j++)for(unsigned i=0;i<s->h.nb;i++)
        if(s->b[i].set==image_types[j].descriptor_set&&s->b[i].binding==image_types[j].binding){
            if(s->image_type_mask&(1ull<<i))die("duplicate shader image type",i);
            s->image_types[i]=image_types[j];s->image_type_mask|=1ull<<i;
        }
    if(stage==0&&G.portable_draw_parameters){
        size_t count=0;uint32_t *lowered=nvn_vk_draw_parameters((const uint32_t *)bytes,size/4,&count,&s->draw_parameter_mask);
        if(!lowered)die("portable draw parameter module",0);free(bytes);bytes=(uint8_t *)lowered;size=count*4;
        if(s->draw_parameter_mask)fprintf(stderr,"[nvn-vulkan] PORTABLE_DRAW_PARAMETERS shader=%s mask=%u\n",s->key,s->draw_parameter_mask);
    }
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
                /* clustered_culling's original code computes flattened global
                 * index = globalY * (c0[0x200] * localSizeX) + globalX.
                 * This field is the live X workgroup count, not a zero word. */
                if(stage==5 && (offset==0x200||offset==0x204))valid=1;
            }else valid=(offset>=0x120&&offset<0x168&&(offset-0x120)%8==0)||(offset>=0x168&&offset<0x568&&(offset-0x168)%8==0)||(offset>=0x568&&offset<0x968&&(offset-0x568)%8==0);
            if(!valid){fprintf(stderr,"[nvn-vulkan] DRIVER_UNMAPPED shader=%s stage=%u bank=%d binding=%u offset=%llu\n",s->key,stage,b->slot,b->binding,(unsigned long long)offset);die("driver constant access outside verified binding tables",offset);}
        }
    }
    free(constants);free(known);
    if(!getenv("DM_DISABLE_STORAGE_BIAS")){
        SBResource resources[LIMIT_BINDINGS];
        for(unsigned i=0;i<s->h.nb;i++)resources[i]=(SBResource){s->b[i].binding,(s->b[i].kind==3||s->b[i].kind==9)&&!s->b[i].flags&&s->b[i].type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        bytes=dm_storage_bias_apply(bytes,&size,resources,s->h.nb,&s->storage_bias);if(!bytes)die("storage index compensation module bounds",0);
        if(s->storage_bias.mask)fprintf(stderr,"[nvn-vulkan] STORAGE_BIAS_MODULE shader=%s mask=%#llx bytes=%zu\n",s->key,(unsigned long long)s->storage_bias.mask,size);
    }
    const char *dump=getenv("DM_ADAPTED_SHADER_DIR");
    if(dump&&!warm){char path[1200];if(snprintf(path,sizeof(path),"%s/%s.spv",dump,s->key)>=(int)sizeof path)die("adapted shader dump path",0);
        FILE *out=fopen(path,"wb");if(!out||fwrite(bytes,1,size,out)!=size)die("adapted shader dump",size);fclose(out);}
    {const char *ov=getenv("DM_SHADER_OVERRIDE_DIR");
        if(ov&&!warm){char opath[1200];FILE *of;
            if(snprintf(opath,sizeof opath,"%s/%s.spv",ov,s->key)<(int)sizeof opath&&(of=fopen(opath,"rb"))){
                long on=(fseek(of,0,SEEK_END)==0)?ftell(of):-1;uint8_t *ob=(on>20&&on%4==0&&fseek(of,0,SEEK_SET)==0)?malloc((size_t)on):NULL;
                if(ob&&fread(ob,1,(size_t)on,of)==(size_t)on){free(bytes);bytes=ob;size=(size_t)on;fprintf(stderr,"[nvn-vulkan] SHADER_OVERRIDE shader=%s bytes=%ld\n",s->key,on);}else free(ob);
                fclose(of);}}}
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=(uint32_t *)bytes};
    CHECK(vkCreateShaderModule(G.device,&mi,NULL,&s->module));
    if(stage==0){
        if(!nvn_vk_vertex_inputs((uint32_t *)bytes,size/4,s->vertex_inputs))die("vertex input SPIR-V ABI",0);
        uint32_t locations=0;
        for(unsigned i=0;i<s->h.na;i++){unsigned l=s->attr[i][0];if(l>=16||!s->vertex_inputs[l].type||(locations&(1u<<l)))die("vertex input metadata/IR mismatch",l);locations|=1u<<l;}
        for(unsigned l=0;l<16;l++)if(s->vertex_inputs[l].type&&!(locations&(1u<<l)))die("vertex input missing metadata",l);
        s->vertex_spv=bytes;s->vertex_bytes=size;
    }
    s->spirv=(uint32_t *)bytes;s->spirv_words=size/4;
    fprintf(stderr,"[nvn-vulkan] original shader key=%s stage=%u loaded\n",s->key,stage);
    /* Uniform ranges change every draw, while their pool identity is stable.
     * Dynamic descriptors keep the descriptor set stable and move only the
     * offset into vkCmdBindDescriptorSets. Offsets are consumed in layout
     * order, so enable this only for ascending bindings. */
    s->dyn_bindings=0;s->dyn_count=0;
    if(!getenv("DM_DISABLE_DYNAMIC_UBO")&&
       (G.profile.driver_id==VK_DRIVER_ID_MESA_TURNIP||getenv("DM_ENABLE_DYNAMIC_UBO"))){
     int ordered=1;uint32_t previous=0;
     for(uint32_t i=0;i<s->h.nb;i++){if(i&&s->b[i].binding<previous)ordered=0;previous=s->b[i].binding;}
     for(uint32_t i=0;ordered&&i<s->h.nb;i++)
        if(s->b[i].kind==2&&s->b[i].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&
           s->b[i].slot>=0&&s->b[i].slot<16){s->dyn_bindings|=1ull<<i;s->dyn_count++;}}
    VkDescriptorSetLayoutBinding bindings[LIMIT_BINDINGS];for(uint32_t i=0;i<s->h.nb;i++){
       if(s->b[i].count!=1 || s->b[i].set!=(stage==1?1u:0u))die("shader descriptor set/count",s->b[i].set);
       VkDescriptorType type=(VkDescriptorType)s->b[i].type;
       if((s->dyn_bindings>>i)&1)type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
       bindings[i]=(VkDescriptorSetLayoutBinding){s->b[i].binding,type,1,stage==0?VK_SHADER_STAGE_VERTEX_BIT:stage==1?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_COMPUTE_BIT,NULL}; }
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=s->h.nb,.pBindings=bindings};
    CHECK(vkCreateDescriptorSetLayout(G.device,&li,NULL,&s->layout));if(!warm)s->plan=binding_plan_compile(s);
}
static VkShaderModule shader_module(VShader *s,uint64_t mask);
static VkShaderModule vertex_module(VShader *s,const NvnVkVertexAdapt *plan,uint64_t null_bindings) {
    unsigned active=plan->split;for(unsigned l=0;l<16;l++)active|=plan->convert[l];
    if(!active)return shader_module(s,null_bindings);
    for(VertexModule *v=s->vertex_modules;v;v=v->next)if(v->null_bindings==null_bindings&&!memcmp(&v->plan,plan,sizeof *plan))return v->module;
    size_t bytes=s->vertex_bytes;uint8_t *adapted=nvn_vk_vertex_adapt(s->vertex_spv,&bytes,plan);
    if(!adapted)die("vertex input adaptation unsupported SPIR-V pointer/interface",plan->split);
    if(null_bindings){
        uint32_t bindings[LIMIT_BINDINGS];size_t count=0;
        for(unsigned i=0;i<s->h.nb;i++)if(null_bindings&(1ull<<i))bindings[count++]=s->b[i].binding;
        uint32_t *lowered=NULL;size_t words=0;const char *error=NULL;
        if(!nvn_vk_null_shader((const uint32_t *)adapted,bytes/4,bindings,count,&lowered,&words,&error)){
            fprintf(stderr,"[nvn-vulkan] NULL_VERTEX_REJECT shader=%s reason=%s\n",s->key,error);die("vertex/null shader adaptation",0);}
        free(adapted);adapted=(uint8_t *)lowered;bytes=words*4;
    }
    VertexModule *v=calloc(1,sizeof *v);if(!v)die("vertex shader variant allocation",0);v->plan=*plan;v->null_bindings=null_bindings;
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=(uint32_t *)adapted};
    CHECK(vkCreateShaderModule(G.device,&mi,NULL,&v->module));
    uint32_t u=0,i=0;for(unsigned l=0;l<16;l++){if(plan->convert[l]==1)u|=1u<<l;if(plan->convert[l]==2)i|=1u<<l;}
    const char *dump=getenv("DM_VERTEX_SHADER_DIR");
    if(dump){char path[1400];int n=snprintf(path,sizeof path,"%s/%s-%x-%x-%x.spv",dump,s->key,plan->split,u,i);if(n<0||n>=(int)sizeof path)die("vertex shader dump path",0);FILE *f=fopen(path,"wb");if(!f||fwrite(adapted,1,bytes,f)!=bytes)die("vertex shader dump",bytes);fclose(f);}
    free(adapted);v->next=s->vertex_modules;s->vertex_modules=v;
    fprintf(stderr,"[nvn-vulkan] VERTEX_INPUT_ADAPT shader=%s split=%#x unsigned=%#x signed=%#x bytes=%zu\n",s->key,plan->split,u,i,bytes);
    return v->module;
}
/* Index entry -> loaded shader. G.shaders is append-only and entries are stable, so the direct-mapped table is never
 * stale; a collision just falls back to the scan. Removes ~130 strcmp calls per draw once many shaders are loaded. */
#define SHADER_GET_CACHE 512
static struct {const uint8_t *entry;uint32_t stage;VShader *shader;} g_shader_get_cache[SHADER_GET_CACHE];
static VShader *shader_get(const uint8_t *entry,uint32_t stage) {
    if(!entry)die("original shader not in verified Vulkan cache",stage);
    unsigned slot=(unsigned)(((uintptr_t)entry>>4)*0x9E3779B1u>>8)&(SHADER_GET_CACHE-1);
    static int cache_off=-1;if(cache_off<0)cache_off=getenv("DM_DISABLE_SHADER_GET_CACHE")!=NULL;
    if(!cache_off&&g_shader_get_cache[slot].entry==entry&&g_shader_get_cache[slot].stage==stage)return g_shader_get_cache[slot].shader;
    char key[17]={0};memcpy(key,entry,16);
    for(int i=0;i<G.nshader;i++)if(!strcmp(G.shaders[i].key,key)){g_shader_get_cache[slot].entry=entry;g_shader_get_cache[slot].stage=stage;g_shader_get_cache[slot].shader=&G.shaders[i];return &G.shaders[i];}
    if(G.nshader==LIMIT_OBJECTS)die("shader object limit",G.nshader);
    VShader *s=&G.shaders[G.nshader++];s->identity=G.nshader;{uint64_t t=dm_android_nvn_clock_ns();shader_load_into(s,key,stage,0);G.profile_shader_ns+=dm_android_nvn_clock_ns()-t;}return s;
}
static VkShaderModule shader_module(VShader *s,uint64_t mask) {
    if(!mask)return s->module;
    for(NvnNullModule *v=s->null_modules;v;v=v->next)if(v->mask==mask)return v->module;
    if(!s->spirv||s->null_module_count>=64)die("portable null shader variant limit",s->null_module_count);
    uint32_t bindings[LIMIT_BINDINGS];size_t n=0;
    for(uint32_t i=0;i<s->h.nb;i++)if(mask&(1ull<<i))bindings[n++]=s->b[i].binding;
    uint32_t *words=NULL;size_t length=0;const char *error=NULL;
    if(!nvn_vk_null_shader(s->spirv,s->spirv_words,bindings,n,&words,&length,&error)){
        fprintf(stderr,"[nvn-vulkan] NULL_SHADER_REJECT shader=%s mask=%#llx reason=%s\n",s->key,(unsigned long long)mask,error);die("portable null shader lowering",0);
    }
    const char *dump=getenv("DM_ADAPTED_SHADER_DIR");if(dump){char path[1200];snprintf(path,sizeof path,"%s/%s-null-%llx.spv",dump,s->key,(unsigned long long)mask);FILE *f=fopen(path,"wb");if(!f||fwrite(words,4,length,f)!=length||fclose(f))die("null shader diagnostic output",0);}
    NvnNullModule *v=calloc(1,sizeof *v);if(!v)die("portable null shader allocation",0);
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=length*4,.pCode=words};
    CHECK(vkCreateShaderModule(G.device,&mi,NULL,&v->module));free(words);
    v->mask=mask;v->next=s->null_modules;s->null_modules=v;s->null_module_count++;
    fprintf(stderr,"[nvn-vulkan] PORTABLE_NULL_SHADER shader=%s mask=%#llx words=%zu\n",s->key,(unsigned long long)mask,length);return v->module;
}
/* Explicit, device-calibrated experiment. The base shader and its descriptor
 * ABI remain immutable; no other draw can inherit this input attachment. */
enum { RESOURCE_DEPTH_INPUT=12,RESOURCE_DEPTH_GUARD=13 };
static VShader *depth_input_shader(VShader *base) {
    if(base->depth_input_variant)return base->depth_input_variant;
    if(!base->spirv||base->h.stage!=1||base->h.nb>=LIMIT_BINDINGS)
        die("depth input shader source/binding capacity",base->h.nb);
    unsigned target=LIMIT_BINDINGS;
    for(unsigned i=0;i<base->h.nb;i++){
        if(base->b[i].binding==63)die("depth input guard binding collision",63);
        if(base->b[i].set==1&&base->b[i].binding==3&&base->b[i].kind==4)target=i;
    }
    if(target==LIMIT_BINDINGS)die("depth input source descriptor",3);
    NvnVkDepthInputGuard guard={1,63,1280,720,1.0f/512.0f,1.0f/512.0f};
    NvnVkDepthInputGuardInfo info;size_t words=0;const char *error=NULL;
    uint32_t *code=G.depth_input_calibrated_ties?
        nvn_vk_depth_input_calibrated_ties_guarded_variant(base->spirv,base->spirv_words,1,3,0,&guard,&words,&info,&error):
        nvn_vk_depth_input_guarded_variant(base->spirv,base->spirv_words,1,3,0,&guard,&words,&info,&error);
    if(!code){fprintf(stderr,"[nvn-vulkan] DEPTH_INPUT_REJECT shader=%s reason=%s\n",base->key,error);die("guarded depth input shader lowering",0);}
    VShader *s=malloc(sizeof *s);if(!s)die("depth input shader allocation",0);*s=*base;
    s->identity=base->identity|0x80000000u;s->depth_input=1;s->depth_input_variant=NULL;
    s->module=VK_NULL_HANDLE;s->layout=VK_NULL_HANDLE;s->spirv=code;s->spirv_words=words;
    s->null_modules=NULL;s->null_module_count=0;s->plan=NULL;
    s->b[target].kind=RESOURCE_DEPTH_INPUT;s->b[target].type=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    s->b[s->h.nb++]=(RBinding){1,63,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,RESOURCE_DEPTH_GUARD,-1,-1,1,0};
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=words*4,.pCode=code};
    CHECK(vkCreateShaderModule(G.device,&mi,NULL,&s->module));
    VkDescriptorSetLayoutBinding bindings[LIMIT_BINDINGS];
    for(unsigned i=0;i<s->h.nb;i++){
        VkDescriptorType type=(VkDescriptorType)s->b[i].type;
        if((s->dyn_bindings>>i)&1)type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[i]=(VkDescriptorSetLayoutBinding){s->b[i].binding,type,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    }
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=s->h.nb,.pBindings=bindings};
    CHECK(vkCreateDescriptorSetLayout(G.device,&li,NULL,&s->layout));
    const char *dump=getenv("DM_ADAPTED_SHADER_DIR");if(dump){char path[1200];snprintf(path,sizeof path,"%s/%s-depth-input-guard.spv",dump,s->key);
        FILE *f=fopen(path,"wb");if(!f||fwrite(code,4,words,f)!=words||fclose(f))die("depth input shader diagnostic output",0);}
    fprintf(stderr,"[nvn-vulkan] DEPTH_INPUT_VARIANT shader=%s identity=%u samples=%u guards=%u words=%zu guard=%s\n",s->key,s->identity,info.transform.sample_count,info.guard_count,words,G.depth_input_calibrated_ties?"exact-calibrated-ties":"conservative-open");
    base->depth_input_variant=s;return s;
}
#include "nvn_vk_pipeline.inc"
#include "nvn_vk_coverage_runtime.inc"
void mtl_prog_release(prog_t *p) { (void)p; /* Programs own only immutable shared shader-cache entries; no per-program GPU allocation. */ }
static void aligned_writeback(void) {
    for(int i=0;i<S.naligned;i++)if(S.aligned[i].written){AlignedBinding *b=&S.aligned[i];uint64_t offset=0;
        VPool *p=pool_find(b->range.addr,b->range.size,&offset);if(!p)die("aligned binding pool lifetime",b->range.addr);
        all_barrier();VkBufferCopy copy={0,offset,b->range.size};vkCmdCopyBuffer(S.command,b->buffer->b,p->v.b,1,&copy);
    }
    S.naligned=0;
}
static int depth_read_only(void) {
    const ds_st *d=&G.state.ds;if(d->dw)return 0;
    if(!d->st || (!G.state.stencil_write[0]&&!G.state.stencil_write[1]))return 1;
    return d->fail_f==NVN_STENCIL_OP_KEEP&&d->zfail_f==NVN_STENCIL_OP_KEEP&&d->zpass_f==NVN_STENCIL_OP_KEEP&&d->fail_b==NVN_STENCIL_OP_KEEP&&d->zfail_b==NVN_STENCIL_OP_KEEP&&d->zpass_b==NVN_STENCIL_OP_KEEP;
}
/* A shader texture binding with no resource: handle 0 while texture id 0 was never created, or the NVN invalid-id
 * sentinel 0xfffff (guest left the slot pointing past the pool, e.g. an unused sampler of a small room's material). */
static int texture_binding_null(uint64_t handle) {
    if((handle&0xfffff)==0xfffff)return 1;
    return !handle && (!G.texture_slots||!G.tslot[0].live);
}
static void collect_image_uses(VShader *s) {
    if(!s)return;
    for(unsigned i=0;i<s->h.nb;i++) {RBinding *b=&s->b[i];if(b->kind!=4&&b->kind!=6&&b->kind!=11)continue;
        if(b->slot<0 || b->slot>=(b->kind!=4?32:128))die("image-use shader slot",b->slot);
        uint64_t handle=b->kind!=4?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot];unsigned tid=handle&0xfffff;
        if(texture_binding_null(handle))continue;
        if(tid>=G.texture_slots||!G.tslot[tid].live){fprintf(stderr,"[nvn-vulkan] IMAGE_MISSING shader=%s stage=%u kind=%u slot=%d tid=%u\n",s->key,s->h.stage,b->kind,b->slot,tid);
            for(unsigned slot=0;slot<32;slot++)fprintf(stderr,"[nvn-vulkan] IMAGE_BIND slot=%u handle=%llx\n",slot,(unsigned long long)G.state.img[s->h.stage][slot]);
            for(unsigned slot=0;slot<128;slot++)if(G.state.tex[s->h.stage][slot])fprintf(stderr,"[nvn-vulkan] TEXTURE_BIND slot=%u handle=%llx\n",slot,(unsigned long long)G.state.tex[s->h.stage][slot]);
            die("image-use texture descriptor",tid);}
        uint64_t serial=G.tslot[tid].texture.serial;unsigned j=0;while(j<S.nimage_uses&&S.image_uses[j].serial!=serial)j++;
        if(j==S.nimage_uses){if(j==2*LIMIT_BINDINGS)die("image-use capacity",j);S.image_uses[S.nimage_uses++]=(ImageUse){serial,0};}
        S.image_uses[j].roles|=b->kind==6?2u:1u;
        if(b->kind==6)texture_require_storage(slot_texture(&G.tslot[tid],1));
    }
}
static int storage_image_use(uint64_t serial) {
    for(unsigned i=0;i<S.nimage_uses;i++)if(S.image_uses[i].serial==serial)return (S.image_uses[i].roles&2)!=0;
    return 0;
}
/* Disjoint mip/layer reads are legal NVN rendering, not attachment feedback.
 * The current backend tracks layouts for an entire image. On drivers without
 * feedback layouts, isolate those sampled subresources with a bit-exact GPU
 * copy so layout transitions cannot invalidate the attachment or descriptor. */
static int disjoint_sampling(VTexture *t,TSlot *slot,unsigned stage) {
    if(G.feedback_layout || stage==5 || aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)return 0;
    view_t *v=&slot->view;
    unsigned level=slot->has_view&&v->set_levels?v->base_level:0;
    unsigned levels=slot->has_view&&v->set_levels?v->nlevels:t->source.d.levels;
    unsigned layer=slot->has_view&&v->set_layers?v->min_layer:0;
    unsigned layers=slot->has_view&&v->set_layers?v->nlayers:t->source.L.layers;
    if(level>=t->source.d.levels || !levels || levels>t->source.d.levels-level ||
       layer>=t->source.L.layers || !layers || layers>t->source.L.layers-layer)die("sample copy view bounds",t->serial);
    int found=0;
    for(int i=0;i<G.state.rt.n;i++)if(G.state.rt.color[i]&&G.state.rt.color[i]->serial==t->serial){
        view_t *a=&G.state.rt.cview[i];unsigned al=G.state.rt.has_cview[i]&&a->set_levels?a->base_level:0;
        unsigned ar=G.state.rt.has_cview[i]&&a->set_layers?a->min_layer:0;
        if(al>=level && al-level<levels && ar>=layer && ar-layer<layers)return 0;
        found=1;
    }
    if(found && storage_image_use(t->serial))die("disjoint sample copy with storage writes",t->serial);
    return found;
}
static int sampling_copy_required(VTexture *t,TSlot *slot,unsigned stage) {
    if(G.feedback_layout||stage==5||aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)return 0;
    if(disjoint_sampling(t,slot,stage))return !texture_range_ok(t);
    for(int i=0;i<G.state.rt.n;i++)if(G.state.rt.color[i]&&G.state.rt.color[i]->serial==t->serial){
        if(storage_image_use(t->serial))die("attachment sample copy with storage writes",t->serial);
        /* NVN's ordered attachment read observes the preceding draws. Take
         * that GPU state before this draw; the attachment remains writable.
         * This also avoids relying on an optional feedback-loop extension. */
        return 1;
    }
    return 0;
}
static void prepare_sampling_copies(VShader *s) {
    if(!s || G.feedback_layout || s->h.stage==5)return;
    for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==4 && !(s->dref_bindings&(1ull<<i))){
        uint64_t handle=G.state.tex[s->h.stage][s->b[i].slot];unsigned tid=handle&0xfffff;
        if(texture_binding_null(handle))continue;
        if(tid>=G.texture_slots||!G.tslot[tid].live)die("sample copy descriptor",tid);
        TSlot *slot=&G.tslot[tid];
        VTexture *t=slot_texture(slot,1);if(!sampling_copy_required(t,slot,s->h.stage))continue;
        view_t *sv=&slot->view;unsigned slevel=slot->has_view&&sv->set_levels?sv->base_level:0,slevels=slot->has_view&&sv->set_levels?sv->nlevels:t->source.d.levels;
        unsigned slayer=slot->has_view&&sv->set_layers?sv->min_layer:0,slayers=slot->has_view&&sv->set_layers?sv->nlayers:t->source.L.layers;
        uint32_t smask=(slevels>=32?~0u:((1u<<slevels)-1u))<<slevel;
        if(!G.disable_sample_copy_cache&&t->sample_copy&&t->sc_valid&&t->sample_other==t->other_version&&!(t->dirty_levels&smask)&&
           slevel>=t->sc_level&&slevel+slevels<=t->sc_level+t->sc_levels&&slayer>=t->sc_layer&&slayer+slayers<=t->sc_layer+t->sc_layers){G.profile_sample_copy_hits++;continue;}
        G.profile_sample_copy_misses++;
        int cube=t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP||t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
        if(t->source.d.target!=NVN_TEXTURE_TARGET_2D && t->source.d.target!=NVN_TEXTURE_TARGET_2D_ARRAY && !cube)die("disjoint sample copy dimensionality",t->source.d.target);
        if(!t->sample_copy){
            VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
                .imageType=VK_IMAGE_TYPE_2D,.format=t->format,.extent={t->width,t->height,1},.mipLevels=t->source.d.levels,
                .arrayLayers=t->source.L.layers,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
                .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
            CHECK(vkCreateImage(G.device,&ci,NULL,&t->sample_copy));VkMemoryRequirements r;vkGetImageMemoryRequirements(G.device,t->sample_copy,&r);
            VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            t->sample_allocation_size=r.size;t->sample_memory=memory_allocate(&ai,DM_VK_MEMORY_SAMPLE_COPY);CHECK(vkBindImageMemory(G.device,t->sample_copy,t->sample_memory,0));
            VkImageViewType type=cube?(t->source.L.layers==6?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY):t->source.d.target==NVN_TEXTURE_TARGET_2D_ARRAY?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
            VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->sample_copy,.viewType=type,.format=t->format,
                .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t->source.d.levels,0,t->source.L.layers}};
            CHECK(vkCreateImageView(G.device,&vi,NULL,&t->sample_view));t->sample_layout=VK_IMAGE_LAYOUT_UNDEFINED;
            fprintf(stderr,"[nvn-vulkan] ATTACHMENT_SAMPLE_COPY serial=%llu levels=%u layers=%u\n",(unsigned long long)t->serial,t->source.d.levels,t->source.L.layers);
        }
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=t->sample_layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=t->sample_layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->sample_copy,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t->source.d.levels,0,t->source.L.layers}};
        vkCmdPipelineBarrier(S.command,HZ_COPY_SRC,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        view_t *v=&slot->view;unsigned level=slot->has_view&&v->set_levels?v->base_level:0,levels=slot->has_view&&v->set_levels?v->nlevels:t->source.d.levels;
        unsigned layer=slot->has_view&&v->set_layers?v->min_layer:0,layers=slot->has_view&&v->set_layers?v->nlayers:t->source.L.layers;
        for(unsigned l=level;l<level+levels;l++){
            unsigned w=t->width>>l,h=t->height>>l;if(!w)w=1;if(!h)h=1;
            VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,layer,layers},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,layer,layers},.extent={w,h,1}};
            vkCmdCopyImage(S.command,t->image,t->layout,t->sample_copy,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        }
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,HZ_COPY_DST,0,0,NULL,0,NULL,1,&barrier);t->sample_layout=barrier.newLayout;
        t->sample_other=t->other_version;t->dirty_levels&=~smask;t->sc_valid=1;t->sc_level=slevel;t->sc_levels=slevels;t->sc_layer=slayer;t->sc_layers=slayers;
    }
}
static VkImageLayout sampled_layout(VTexture *t,unsigned stage) {
    if(storage_image_use(t->serial))return VK_IMAGE_LAYOUT_GENERAL;
    if(stage!=5){
        if(G.state.rt.depth && G.state.rt.depth->serial==t->serial){S.depth_sampled=1;
            if(S.depth_input)die("ordinary sampled alias in depth input experiment",t->serial);
            if(depth_read_only())return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            if(!G.feedback_layout){
                const ds_st *d=&G.state.ds;
                fprintf(stderr,"[nvn-vulkan] DEPTH_FEEDBACK_UNSUPPORTED serial=%llu draw=%llu dt=%u dw=%u df=%u stencil=%u write_masks=%#x,%#x ops=%u,%u,%u/%u,%u,%u\n",(unsigned long long)t->serial,(unsigned long long)G.draws,d->dt,d->dw,d->df,d->st,G.state.stencil_write[0],G.state.stencil_write[1],d->fail_f,d->zfail_f,d->zpass_f,d->fail_b,d->zfail_b,d->zpass_b);
                if(G.capture_feedback){G.capture_feedback=0;all_barrier();mtl_sub_commit();}
                die("NVN writable depth feedback requires device support",t->serial);
            }
            S.feedback_flags|=VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
            return VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        }
        for(int i=0;i<G.state.rt.n;i++)if(G.state.rt.color[i]&&G.state.rt.color[i]->serial==t->serial){
            if(!G.feedback_layout)die("NVN color feedback requires device support",t->serial);
            S.feedback_flags|=VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
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
        t->shadow_allocation_size=r.size;t->shadow_memory=memory_allocate(&ai,DM_VK_MEMORY_SHADOW);CHECK(vkBindImageMemory(G.device,t->shadow,t->shadow_memory,0));
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
    vkCmdPipelineBarrier(S.command,HZ_COPY_SRC,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    for(unsigned level=0;level<t->source.d.levels;level++){
        unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
        uint64_t bytes=(uint64_t)width*height*t->source.L.layers*(f==VK_FORMAT_D16_UNORM?2:4);
        VBuffer *bridge=temporary_named((bytes+3)&~3ULL,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,TEMP_COMPARISON);
        VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,t->source.L.layers},.imageExtent={width,height,1}};
        vkCmdCopyImageToBuffer(S.command,t->image,t->layout,bridge->b,1,&region);
        VkBufferMemoryBarrier bb={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=bridge->b,.offset=0,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&bb,0,NULL);
        region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(S.command,bridge->b,t->shadow,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    }
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,HZ_COPY_DST,0,0,NULL,0,NULL,1,&barrier);
    t->shadow_layout=barrier.newLayout;t->shadow_version=t->version;
}
static void prepare_comparison_images(VShader *s) {
    if(!s)return;
    for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==4&&(s->dref_bindings&(1ull<<i))){
        uint64_t handle=G.state.tex[s->h.stage][s->b[i].slot];unsigned tid=handle&0xfffff;
        if(texture_binding_null(handle))continue;
        if(tid>=G.texture_slots||!G.tslot[tid].live)die("comparison image descriptor",tid);
        VTexture *t=slot_texture(&G.tslot[tid],1);
        if(aspect(t->format)==VK_IMAGE_ASPECT_COLOR_BIT)comparison_image(t);
    }
}
static void trace_texture(VTexture *t,const VShader *s,const RBinding *binding) {
    if(t->bc_block)return; /* This optional capture serializes decoded texels. */
    const char *trace_shader=G.trace_ui?"9a650561e5c40791":G.trace_shader[0]?G.trace_shader:"5c1ca4e7c3e4657c";
    if(!G.trace_frame||s->h.stage!=1||strcmp(s->key,trace_shader)||G.ntrace_textures==24||t->depth!=1)return;
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
        vkCmdCopyImageToBuffer(S.command,t->image,t->layout,capture->b,1,&region);
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
static VBuffer *driver_table_build(VShader *s,const RBinding *b) {
    if(s->h.stage>=6||b->type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||(b->slot!=0&&b->slot!=2))die("driver table bank/stage/type",b->slot);
    uint8_t bytes[DRIVER_TABLE_BYTES];memset(bytes,0,sizeof bytes);
    if(b->slot==0){
        /* Clustered culling uses the X count; the original screen-space
         * compute shader also multiplies the Y count by its local size. */
        if(s->h.stage==5)memcpy(bytes+0x200,G.state.dispatch,8);
        unsigned base=s->h.stage==5?0x310:s->h.stage==1?0x510:0x110;
        for(unsigned j=0;j<16;j++){
            Range r=G.state.storage[s->h.stage][j];if(r.size>UINT32_MAX)die("driver buffer size",r.size);uint32_t n=r.size;
            memcpy(bytes+base+j*16,&r.addr,8);memcpy(bytes+base+j*16+8,&n,4);
            if(s->h.stage==5){r=G.state.uniform[5][j];if(r.size>UINT32_MAX)die("driver uniform size",r.size);n=r.size;
                memcpy(bytes+0x210+j*16,&r.addr,8);memcpy(bytes+0x218+j*16,&n,4);}
        }
    }else{
        for(unsigned j=0;j<128;j++){uint32_t t=G.state.tex[s->h.stage][j],sp=G.state.samp[s->h.stage][j];
            memcpy(bytes+0x168+j*8,&t,4);memcpy(bytes+0x568+j*8,&sp,4);}
        for(unsigned j=0;j<9;j++){uint32_t image=G.state.img[s->h.stage][j];memcpy(bytes+0x120+j*8,&image,4);}
    }
    VBuffer **last=&S.last_driver_table[s->h.stage][b->slot/2];uint32_t hash=0,slot=0;
    if(!G.disable_driver_table_reuse){
        if(*last&&!memcmp((*last)->map,bytes,sizeof bytes)){G.profile_driver_table_reuses++;return *last;}
        hash=(uint32_t)crc32(0,bytes,sizeof bytes);slot=hash&(DRIVER_TABLE_SLOTS-1);
        while(S.driver_table_slots[slot]){
            DriverTable *entry=&S.driver_tables[S.driver_table_slots[slot]-1];
            if(entry->hash==hash&&!memcmp(entry->buffer->map,bytes,sizeof bytes)){
                G.profile_driver_table_reuses++;*last=entry->buffer;return *last;
            }
            slot=(slot+1)&(DRIVER_TABLE_SLOTS-1);
        }
    }
    /* Preserve all 64KB, including zeros beyond the populated words. The
     * uniform descriptor is immutable and cannot be written by guest shaders.
     * Its existing temporary registry owns it until the submission fence. */
    VBuffer *table=temporary_named(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,TEMP_DRIVER);memcpy(table->map,bytes,sizeof bytes);G.profile_driver_tables++;
    if(!G.disable_driver_table_reuse&&S.ndriver_tables<LIMIT_WRITES){
        uint32_t index=S.ndriver_tables++;S.driver_tables[index]=(DriverTable){hash,table};S.driver_table_slots[slot]=index+1;*last=table;
    }
    return table;
}
static VBuffer *driver_table(VShader *s,const RBinding *b) {
    if(s->h.stage>=6||b->type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||(b->slot!=0&&b->slot!=2))die("driver table bank/stage/type",b->slot);
    /* The table content depends only on this stage's storage (bank 0) or texture/sampler/image (bank 2) bindings. */
    unsigned bank=b->slot/2,stage=s->h.stage;
    DriverCacheEntry *dc=&G.driver_cache[stage][bank];
    uint64_t revision_ssbo=G.ssbo_revision[stage],revision_tex=G.texbind_revision[stage];
    if(!G.disable_driver_table_reuse&&dc->table&&dc->epoch==S.epoch&&(bank==0?dc->ssbo==revision_ssbo:dc->tex==revision_tex)){G.profile_driver_table_reuses++;return dc->table;}
    VBuffer *table=driver_table_build(s,b);
    dc->epoch=S.epoch;dc->ssbo=revision_ssbo;dc->tex=revision_tex;dc->table=table;
    return table;
}
static void direct_storage_read(Range r) {
    int entry=-1,from=0;
    for(int i=0;i<S.ndirect_reads;i++)if(S.direct_reads[i].addr==r.addr&&S.direct_reads[i].size==r.size){entry=i;from=S.direct_read_write_count[i];break;}
    int dependency=0;for(int i=from;i<S.nwrites;i++)if(overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size)){dependency=1;break;}
    if(dependency)all_barrier_soft();
    if(entry<0&&S.ndirect_reads<LIMIT_WRITES){entry=S.ndirect_reads++;S.direct_reads[entry]=r;}
    if(entry>=0)S.direct_read_write_count[entry]=S.nwrites;
}
typedef struct {uint32_t bias[LIMIT_BINDINGS];int32_t draw[3];} ShaderSpecialization;
static VkSpecializationInfo storage_specialization(VShader *s,VkSpecializationMapEntry entries[LIMIT_BINDINGS+3],ShaderSpecialization *values,const int32_t draw[3]) {
    uint32_t count=0;
    memcpy(values->bias,S.storage_bias[s->h.stage],sizeof values->bias);memset(values->draw,0,sizeof values->draw);
    for(unsigned i=0;i<s->h.nb;i++)if(s->storage_bias.mask&(1ull<<i))entries[count++]=(VkSpecializationMapEntry){s->storage_bias.spec_id[i],i*4,4};
    if(draw)for(unsigned f=0;f<3;f++)if(s->draw_parameter_mask&(1u<<f)){values->draw[f]=draw[f];entries[count++]=(VkSpecializationMapEntry){NVN_DRAW_SPEC_BASE+f,offsetof(ShaderSpecialization,draw)+f*4,4};}
    return (VkSpecializationInfo){count,entries,sizeof *values,values};
}
static VkImageView submission_image_view(const VkImageViewCreateInfo *info) {
    ImageViewKey key;memset(&key,0,sizeof key);
    key.image=info->image;key.flags=info->flags;key.type=info->viewType;key.format=info->format;
    key.components=info->components;key.range=info->subresourceRange;
    uint32_t slot=0;int cacheable=!G.disable_resource_cache&&!info->pNext;
    if(cacheable){
        uint64_t hash=14695981039346656037ULL;const uint8_t *bytes=(const uint8_t *)&key;
        for(size_t i=0;i<sizeof key;i++)hash=(hash^bytes[i])*1099511628211ULL;
        for(slot=(uint32_t)hash&(VIEW_TABLE_SIZE-1);S.view_table[slot];slot=(slot+1)&(VIEW_TABLE_SIZE-1)){
            uint32_t index=S.view_table[slot]-1;
            if(!memcmp(&S.view_keys[index],&key,sizeof key)){G.profile_view_hits++;return S.temporary_views[index];}
        }
    }
    if(S.nview-S.retained_views==LIMIT_WRITES)die("submission image view count",S.nview);
    uint32_t index=S.nview++;CHECK(vkCreateImageView(G.device,info,NULL,&S.temporary_views[index]));
    G.profile_view_misses++;G.resource_views++;
    if(cacheable){S.view_keys[index]=key;S.view_table[slot]=index+1;}
    return S.temporary_views[index];
}
static void binding_plan_remember(VShader *s,VkDescriptorSet set,const BindingResource *resources,int safe) {
    BindingPlan *plan=s->plan;
    if(!plan||!safe||G.disable_binding_plans||G.disable_desc_cache||G.trace_frame||!set){if(plan)plan->resolved=0;return;}
    if(s->h.nb)memcpy(plan->resources,resources,(size_t)s->h.nb*sizeof(*resources));
    plan->epoch=S.epoch;plan->state_revision=G.binding_revision[s->h.stage];
    plan->registry_revision=G.registry_revision;plan->pool_revision=G.pool_revision;
    plan->set=set;plan->resolved=1;
}
static void binding_plan_miss(unsigned reason) {
    G.profile_plan_misses++;
    if(reason<PLAN_MISS_COUNT)G.profile_plan_miss_reason[reason]++;
}
static void binding_plan_fallback(unsigned reason) {
    G.profile_plan_fallbacks++;
    if(reason<PLAN_MISS_COUNT)G.profile_plan_miss_reason[reason]++;
}
static VkDescriptorSet binding_plan_try(VShader *s) {
    BindingPlan *plan=s->plan;
    if(!plan){binding_plan_fallback(PLAN_MISS_NO_PLAN);return VK_NULL_HANDLE;}
    if(G.disable_binding_plans||G.disable_desc_cache){binding_plan_fallback(PLAN_MISS_DISABLED);return VK_NULL_HANDLE;}
    if(G.trace_frame){binding_plan_fallback(PLAN_MISS_TRACE);return VK_NULL_HANDLE;}
    if(!plan->eligible){binding_plan_fallback(PLAN_MISS_INELIGIBLE);return VK_NULL_HANDLE;}
    if(!plan->resolved||!plan->set){binding_plan_miss(PLAN_MISS_UNRESOLVED);return VK_NULL_HANDLE;}
    if(plan->epoch!=S.epoch){binding_plan_miss(PLAN_MISS_EPOCH);return VK_NULL_HANDLE;}
    if(!G.plan_precise_state&&plan->state_revision!=G.binding_revision[s->h.stage]){
        binding_plan_miss(PLAN_MISS_STATE_REVISION);return VK_NULL_HANDLE;
    }
    if(plan->registry_revision!=G.registry_revision){binding_plan_miss(PLAN_MISS_REGISTRY_REVISION);return VK_NULL_HANDLE;}
    if(plan->pool_revision!=G.pool_revision){binding_plan_miss(PLAN_MISS_POOL_REVISION);return VK_NULL_HANDLE;}
    uint32_t live_dyn[LIMIT_BINDINGS];memset(live_dyn,0,(size_t)s->h.nb*sizeof(*live_dyn));
    for(uint32_t n=0;n<plan->nbuffer;n++){
        unsigned i=plan->buffers[n];const RBinding *b=&s->b[i];
        Range r=G.state.uniform[s->h.stage][b->slot];uint64_t offset=0;
        if(!r.addr||!r.size){binding_plan_miss(PLAN_MISS_BUFFER_RANGE);return VK_NULL_HANDLE;}
        VPool *pool=pool_read(r.addr,r.size,&offset);const BindingBuffer *cached=&plan->resources[i].buffer;
        int dynamic=(s->dyn_bindings>>i)&1;
        if(offset%G.properties.limits.minUniformBufferOffsetAlignment){binding_plan_miss(PLAN_MISS_BUFFER_ALIGNMENT);return VK_NULL_HANDLE;}
        if(!dynamic&&(r.addr!=cached->range.addr||r.size!=cached->range.size)){binding_plan_miss(PLAN_MISS_BUFFER_RANGE);return VK_NULL_HANDLE;}
        if(pool->serial!=cached->pool_serial){binding_plan_miss(PLAN_MISS_BUFFER_POOL);return VK_NULL_HANDLE;}
        if(!dynamic&&offset!=cached->offset){binding_plan_miss(PLAN_MISS_BUFFER_OFFSET);return VK_NULL_HANDLE;}
        if(dynamic&&offset>UINT32_MAX){binding_plan_miss(PLAN_MISS_BUFFER_OFFSET);return VK_NULL_HANDLE;}
        if(dynamic)live_dyn[i]=(uint32_t)offset;
        G.profile_plan_buffers++;
    }
    for(uint32_t n=0;n<plan->nimage;n++){
        unsigned i=plan->images[n];const RBinding *b=&s->b[i];
        uint64_t handle=G.state.tex[s->h.stage][b->slot];unsigned tid=(unsigned)handle&0xfffff;
        if(tid>=(unsigned)G.texture_pool_n||tid>=G.texture_slots||!G.tslot[tid].live){binding_plan_miss(PLAN_MISS_IMAGE_INVALID);return VK_NULL_HANDLE;}
        TSlot *slot=&G.tslot[tid];VTexture *t=slot_texture(slot,1);const BindingImage *cached=&plan->resources[i].image;
        if(t->source.d.target!=NVN_TEXTURE_TARGET_2D||
           (slot->has_view&&(slot->view.set_levels||slot->view.set_layers||slot->view.set_format||slot->view.set_target||slot->view.set_swizzle||slot->view.depth_mode))){binding_plan_miss(PLAN_MISS_IMAGE_VIEW);return VK_NULL_HANDLE;}
        if(cached->descriptor_id!=tid||cached->texture.incarnation!=t->incarnation||cached->texture.slot!=(uint32_t)(t-G.textures)){binding_plan_miss(PLAN_MISS_IMAGE_IDENTITY);return VK_NULL_HANDLE;}
        if(disjoint_sampling(t,slot,s->h.stage)){binding_plan_miss(PLAN_MISS_IMAGE_DISJOINT);return VK_NULL_HANDLE;}
        VkImageLayout layout=sampled_layout(t,s->h.stage);texture_transition(t,layout);
        if(t->view==VK_NULL_HANDLE){binding_plan_miss(PLAN_MISS_IMAGE_VIEW);return VK_NULL_HANDLE;}
        if(layout!=cached->layout){binding_plan_miss(PLAN_MISS_IMAGE_LAYOUT);return VK_NULL_HANDLE;}
        if(b->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER){
            unsigned sid=(unsigned)((b->sampler>=0?G.state.samp[s->h.stage][b->sampler]:handle)>>20)&0xfff;
            VkSampler sampler;
            if(sid>=LIMIT_OBJECTS||!G.sslot[sid].live){
                if(!(s->fetch_bindings&(1ull<<i))||sid||!G.fetch_sampler){binding_plan_miss(PLAN_MISS_SAMPLER);return VK_NULL_HANDLE;}
                sampler=G.fetch_sampler;
            }else sampler=G.sslot[sid].sampler;
            if(G.plan_precise_state&&sampler!=cached->sampler){
                binding_plan_miss(PLAN_MISS_SAMPLER);return VK_NULL_HANDLE;
            }
        }
        G.profile_plan_images++;
    }
    if(s->dyn_count){
        uint32_t k=0;for(uint32_t i=0;i<s->h.nb;i++)if((s->dyn_bindings>>i)&1)S.dyn_offset[s->h.stage][k++]=live_dyn[i];
        S.dyn_count[s->h.stage]=k;
    }
    G.profile_plan_hits++;return plan->set;
}
static VkSampler fetch_sampler(void) {
    if(!G.fetch_sampler){VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=VK_LOD_CLAMP_NONE};CHECK(vkCreateSampler(G.device,&ci,NULL,&G.fetch_sampler));}
    return G.fetch_sampler;
}
static unsigned vertex_scalar_kind(unsigned nvn) {
    switch(nvn){
    case NVN_FORMAT_R8UI:case NVN_FORMAT_RG8UI:case NVN_FORMAT_RGBA8UI:
    case NVN_FORMAT_R32UI:case NVN_FORMAT_RG32UI:case NVN_FORMAT_RGBA32UI:return 2;
    case NVN_FORMAT_RGB10A2I:return 1;
    default:return 0;
    }
}
static int shader_image_target(const NvnVkImageType *type,const VTexture *t,unsigned layers,int cube_compatible) {
    int source=t->source.d.target;
    if(type->multisampled)die("shader multisampled image view",type->binding);
    if(type->dim==0&&(source==NVN_TEXTURE_TARGET_1D||source==NVN_TEXTURE_TARGET_1D_ARRAY)){
        if(!type->arrayed&&layers!=1)die("non-array 1D shader image layers",layers);
        return type->arrayed?NVN_TEXTURE_TARGET_1D_ARRAY:NVN_TEXTURE_TARGET_1D;
    }
    if(type->dim==1&&(source==NVN_TEXTURE_TARGET_2D||source==NVN_TEXTURE_TARGET_2D_ARRAY||source==NVN_TEXTURE_TARGET_CUBEMAP||source==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)){
        if(!type->arrayed&&layers!=1)die("non-array 2D shader image layers",layers);
        return type->arrayed?NVN_TEXTURE_TARGET_2D_ARRAY:NVN_TEXTURE_TARGET_2D;
    }
    if(type->dim==2&&!type->arrayed&&source==NVN_TEXTURE_TARGET_3D)return NVN_TEXTURE_TARGET_3D;
    if(type->dim==3&&cube_compatible){
        if(!layers||layers%6||(!type->arrayed&&layers!=6))die("cube shader image layers",layers);
        return type->arrayed?NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:NVN_TEXTURE_TARGET_CUBEMAP;
    }
    fprintf(stderr,"[nvn-vulkan] IMAGE_TARGET_UNSUPPORTED binding=%u dim=%u arrayed=%u source_target=%d layers=%u\n",type->binding,type->dim,type->arrayed,source,layers);
    die("shader image dimensionality requires compatible texture",type->binding);return source;
}
static VkImageViewType texture_view_type(int target) {
    switch(target){
    case NVN_TEXTURE_TARGET_1D:return VK_IMAGE_VIEW_TYPE_1D;
    case NVN_TEXTURE_TARGET_1D_ARRAY:return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case NVN_TEXTURE_TARGET_2D:return VK_IMAGE_VIEW_TYPE_2D;
    case NVN_TEXTURE_TARGET_2D_ARRAY:return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case NVN_TEXTURE_TARGET_3D:return VK_IMAGE_VIEW_TYPE_3D;
    case NVN_TEXTURE_TARGET_CUBEMAP:return VK_IMAGE_VIEW_TYPE_CUBE;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default:die("shader image view target",target);return VK_IMAGE_VIEW_TYPE_2D;
    }
}
static VkImageSubresourceRange attachment_range(const tex_t *source,const view_t *v);
static void depth_input_guard_prepare(void) {
    if(S.depth_input_guard_active)return;
    pass_end();
    if(!S.depth_input_guard.b){
        if(G.want_cached)die("depth input guard requires coherent allocation",0);
        S.depth_input_guard=buffer_make(4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    memset(S.depth_input_guard.map,0,4);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&barrier,0,NULL,0,NULL);
    S.depth_input_guard_active=1;S.submission_cpu_readback=1;
}
/* DM_DESC_DIAG: why do descriptor-cache lookups miss? Per shader and set of changed bindings, relative to the previous
 * entry of the same shader in this submission. */
static struct {VShader *shader;uint64_t changed_mask;uint64_t count;int none;uint8_t fields[64];} desc_diag[256];
static unsigned desc_diag_n;
static void desc_miss_note(VShader *s,const VkDescriptorBufferInfo *bi,const VkDescriptorImageInfo *ii,uint32_t nb) {
    uint64_t mask=0;int found=0;uint8_t fields[64]={0};
    for(int e=S.ndesc-1;e>=0;e--){const DescEntry *d=&S.desc_entries[e];if(d->shader!=s)continue;found=1;
        const VkDescriptorBufferInfo *ob=(const VkDescriptorBufferInfo *)(S.desc_arena+d->off);
        const VkDescriptorImageInfo *oi=(const VkDescriptorImageInfo *)(S.desc_arena+d->off+nb*sizeof(VkDescriptorBufferInfo));
        for(uint32_t i=0;i<nb&&i<64;i++)if(memcmp(&ob[i],&bi[i],sizeof bi[i])||memcmp(&oi[i],&ii[i],sizeof ii[i])){mask|=1ull<<i;
            fields[i]=(uint8_t)((ob[i].buffer!=bi[i].buffer?1:0)|(ob[i].offset!=bi[i].offset?2:0)|(ob[i].range!=bi[i].range?4:0)|(memcmp(&oi[i],&ii[i],sizeof ii[i])?8:0));}
        break;}
    unsigned k=0;while(k<desc_diag_n&&!(desc_diag[k].shader==s&&desc_diag[k].changed_mask==mask&&desc_diag[k].none==!found))k++;
    if(k==desc_diag_n){if(k==256)return;memset(&desc_diag[k],0,sizeof desc_diag[k]);desc_diag[k].shader=s;desc_diag[k].changed_mask=mask;desc_diag[k].none=!found;desc_diag_n++;}
    desc_diag[k].count++;memcpy(desc_diag[k].fields,fields,sizeof fields);
}
static void desc_miss_report(void) {
    for(unsigned a=0;a<desc_diag_n;a++)for(unsigned b=a+1;b<desc_diag_n;b++)if(desc_diag[b].count>desc_diag[a].count){typeof(desc_diag[0]) t=desc_diag[a];desc_diag[a]=desc_diag[b];desc_diag[b]=t;}
    for(unsigned a=0;a<desc_diag_n&&a<14;a++){const VShader *s=desc_diag[a].shader;
        char kinds[160]={0};size_t n=0;
        for(uint32_t i=0;i<s->h.nb&&i<64&&n<70;i++)if((desc_diag[a].changed_mask>>i)&1)n+=snprintf(kinds+n,sizeof kinds-n,"b%u(kind%d,slot%d,dyn%d,f%u) ",s->b[i].binding,s->b[i].kind,s->b[i].slot,(int)((s->dyn_bindings>>i)&1),desc_diag[a].fields[i]);
        fprintf(stderr,"[nvn-vulkan] DESC_MISS rank=%u shader=%s stage=%u misses=%llu first_of_shader=%d changed=%s\n",a,s->key,s->h.stage,(unsigned long long)desc_diag[a].count,desc_diag[a].none,kinds);}
    desc_diag_n=0;
}
/* Hand out a descriptor set for this layout. Sets are kept by layout and reused once the slot's previous work has
 * completed (descriptor_sets_rewind only rewinds the counters), so the driver allocates a layout's sets once, not every
 * frame. DM_DISABLE_SET_RECYCLE restores one vkAllocateDescriptorSets per draw. */
static VkDescriptorSet descriptor_set_acquire(VShader *s,int fresh) {
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo a={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=S.descriptors,
      .descriptorSetCount=1,.pSetLayouts=&s->layout};
    if(G.disable_set_recycle){CHECK(vkAllocateDescriptorSets(G.device,&a,&set));return set;}
    struct DescSetBucket *b=NULL;
    /* fresh：这次有绑定被跳过（驱动没有 nullDescriptor），回收的集合会留着上次的描述符，所以新分配、不进回收桶。
     * fresh: a binding was skipped this time (the driver has no nullDescriptor); a recycled set would keep the previous user's
     * descriptor there, so allocate a new one and keep it out of the buckets. */
    for(uint32_t i=0;!fresh&&i<S.nset_buckets;i++)if(S.set_buckets[i].layout==s->layout){b=&S.set_buckets[i];break;}
    if(!b&&!fresh){
        if(S.nset_buckets==S.set_buckets_cap){uint32_t cap=S.set_buckets_cap?S.set_buckets_cap*2:64;
            struct DescSetBucket *grown=realloc(S.set_buckets,cap*sizeof *grown);if(!grown)die("descriptor set bucket allocation",cap);
            S.set_buckets=grown;S.set_buckets_cap=cap;}
        b=&S.set_buckets[S.nset_buckets++];*b=(struct DescSetBucket){.layout=s->layout};
    }
    if(b&&b->used<b->count)return b->sets[b->used++];
    /* 从最近一个还有空位的池分配；耗尽就换下一个额外池，下次回收时整体重建。
     * Allocate from the latest pool that still had room; when it runs dry move on to the next extra pool, and the next
     * slot reuse rebuilds everything. */
    a.descriptorPool=S.alloc_pool?S.extra_pools[S.alloc_pool-1]:S.descriptors;
    VkResult r=vkAllocateDescriptorSets(G.device,&a,&set);
    while(r==VK_ERROR_OUT_OF_POOL_MEMORY||r==VK_ERROR_FRAGMENTED_POOL){
        if(S.alloc_pool>=4)die("descriptor pool exhausted",r);
        if(S.alloc_pool>=S.nextra_pools)S.extra_pools[S.nextra_pools++]=descriptor_pool_create();
        a.descriptorPool=S.extra_pools[S.alloc_pool++];r=vkAllocateDescriptorSets(G.device,&a,&set);
    }
    CHECK(r);
    S.sets_total++;                       /* 计入池预算，保证到阈值时整体重建 | counts against the pool budget so the rebuild threshold still triggers */
    if(!b)return set;
    if(b->count==b->cap){uint32_t cap=b->cap?b->cap*2:16;VkDescriptorSet *grown=realloc(b->sets,cap*sizeof *grown);
        if(!grown)die("descriptor set list allocation",cap);b->sets=grown;b->cap=cap;}
    b->sets[b->count++]=set;b->used=b->count;
    return set;
}
static VkDescriptorSet bind_shader(VShader *s) {
    memset(S.storage_bias[s->h.stage],0,sizeof S.storage_bias[s->h.stage]);
    S.null_bindings[s->h.stage]=0;
    S.dyn_count[s->h.stage]=0;
    VkDescriptorSet planned=binding_plan_try(s);if(planned)return planned;
    VkWriteDescriptorSet w[LIMIT_BINDINGS];VkDescriptorBufferInfo bi[LIMIT_BINDINGS];VkDescriptorImageInfo ii[LIMIT_BINDINGS];
    uint32_t live_dyn[LIMIT_BINDINGS];memset(live_dyn,0,(size_t)s->h.nb*sizeof(*live_dyn));
    BindingResource plan_resources[LIMIT_BINDINGS];memset(plan_resources,0,(size_t)s->h.nb*sizeof(*plan_resources));
    int plan_safe=s->plan&&s->plan->eligible&&!G.disable_binding_plans&&!G.disable_desc_cache&&!G.trace_frame;
    memset(w,0,s->h.nb*sizeof(*w));memset(bi,0,s->h.nb*sizeof(*bi));memset(ii,0,s->h.nb*sizeof(*ii));
    int cacheable=!G.disable_desc_cache&&!G.trace_frame;VkDescriptorSet set=VK_NULL_HANDLE;
    /* A requested pre-draw diagnostic must save effective buffers even when
     * an earlier image binding stops on unsupported feedback. Descriptor
     * indices stay unchanged; ordinary draws retain their original order. */
    uint32_t order[LIMIT_BINDINGS],norder=0;
    if(G.capture_feedback){
        for(uint32_t i=0;i<s->h.nb;i++)if(s->b[i].kind==1||s->b[i].kind==2||s->b[i].kind==3||s->b[i].kind==7||s->b[i].kind==8||s->b[i].kind==9)order[norder++]=i;
        for(uint32_t i=0;i<s->h.nb;i++)if(s->b[i].kind!=1&&s->b[i].kind!=2&&s->b[i].kind!=3&&s->b[i].kind!=7&&s->b[i].kind!=8&&s->b[i].kind!=9)order[norder++]=i;
    }else for(uint32_t i=0;i<s->h.nb;i++)order[norder++]=i;
    for(uint32_t step=0;step<norder;step++) { uint32_t i=order[step];RBinding *b=&s->b[i]; w[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstBinding=b->binding,.descriptorCount=1,.descriptorType=(s->dyn_bindings>>i)&1?
           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:(VkDescriptorType)b->type};
       if(b->kind==RESOURCE_DEPTH_GUARD){
           if(!s->depth_input||!S.depth_input)die("depth input guard descriptor state",0);
           depth_input_guard_prepare();bi[i]=(VkDescriptorBufferInfo){S.depth_input_guard.b,0,4};
       }
       else if(b->kind==RESOURCE_DEPTH_INPUT){
           if(!s->depth_input||!S.depth_input||!G.state.rt.depth)die("depth input descriptor state",0);
           VTexture *t=texture_get(G.state.rt.depth,0);texture_transition(t,VK_IMAGE_LAYOUT_GENERAL);
           VkImageSubresourceRange range=attachment_range(G.state.rt.depth,G.state.rt.has_dview?&G.state.rt.dview:NULL);
           range.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
           VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,
               .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=t->format,.subresourceRange=range};
           ii[i]=(VkDescriptorImageInfo){.imageView=submission_image_view(&view),.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
           w[i].pImageInfo=&ii[i];S.depth_sampled=1;continue;
       }
       else if(b->kind==1)bi[i]=(VkDescriptorBufferInfo){G.support.b,0,1392};
       else if(b->kind==7) { if(!s->c1.b)die("missing shader c1",i);bi[i]=(VkDescriptorBufferInfo){s->c1.b,0,65536}; }
       else if(b->kind==10) {
           if(b->slot<0 || b->slot>=128)die("texel buffer slot",b->slot);
           uint32_t tid=(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot])&0xfffff;
           if(tid>=G.texture_slots||!G.tslot[tid].live)die("unbound texel buffer",tid);
           TSlot *slot=&G.tslot[tid];tex_t *t=&slot->texture;
           if(t->d.target!=NVN_TEXTURE_TARGET_BUFFER||t->d.levels!=1||(slot->has_view&&(slot->view.set_layers||slot->view.set_levels)))die("texel buffer target/view",tid);
           int nvn=slot->has_view&&slot->view.set_format?slot->view.format:t->d.format;
           VkFormat f=format(nvn);VkFormatProperties properties=format_properties(f);
           VkFormatFeatureFlags required=b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER?VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT:VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
           if(!(properties.bufferFeatures&required))die("texel buffer format feature",nvn);
           uint64_t offset=0;VPool *p=pool_read((uint64_t)(uintptr_t)t->mem,t->L.size,&offset);VkBuffer buffer=p->v.b;
           if(offset%G.properties.limits.minTexelBufferOffsetAlignment){VBuffer *aligned=temporary((t->L.size+3)&~3ULL,VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
               if(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER||offset%4||t->L.size%4)die("writable/word-unaligned texel buffer",offset);
               all_barrier();VkBufferCopy copy={offset,0,t->L.size};vkCmdCopyBuffer(S.command,p->v.b,aligned->b,1,&copy);all_barrier();buffer=aligned->b;offset=0;}
           if(S.nbuffer_view==LIMIT_WRITES)die("texel buffer view count",S.nbuffer_view);
           VkBufferViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=buffer,.format=f,.offset=offset,.range=t->L.size};
           CHECK(vkCreateBufferView(G.device,&vi,NULL,&S.buffer_views[S.nbuffer_view]));w[i].pTexelBufferView=&S.buffer_views[S.nbuffer_view++];
           if(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)mark_write((uint64_t)(uintptr_t)t->mem,t->L.size);
           cacheable=0;continue;
       }
       else if(b->kind==8) {
           VBuffer *table=driver_table(s,b);
           bi[i]=(VkDescriptorBufferInfo){table->b,0,65536};
       }
       else if(b->kind==2 || b->kind==3 || b->kind==9) {
           if(b->slot<0 || b->slot>=16)die("shader buffer slot",b->slot);
           Range r=(b->kind==2||b->kind==9)?G.state.uniform[s->h.stage][b->slot]:G.state.storage[s->h.stage][b->slot];
           if(G.pool_range_stats&&b->kind==2)G.pool_ranges[pool_range_bucket(r.size)].uniforms++;
           /* A zero-address, zero-size NVN binding has no resource. Preserve
            * that state with a real null descriptor; never allocate substitute
            * backing storage or hide a malformed half-bound range. */
           if(!r.addr&&!r.size){
               if(G.null_descriptors){bi[i]=(VkDescriptorBufferInfo){VK_NULL_HANDLE,0,VK_WHOLE_SIZE};w[i].pBufferInfo=&bi[i];}
               else{S.null_bindings[s->h.stage]|=1ull<<i;w[i].descriptorCount=0;}
               plan_safe=0;continue;
           }
           if(!r.addr || !r.size){fprintf(stderr,"[nvn-vulkan] BUFFER_MISSING shader=%s stage=%u binding=%u kind=%u slot=%d addr=%#llx size=%llu\n",s->key,s->h.stage,b->binding,b->kind,b->slot,(unsigned long long)r.addr,(unsigned long long)r.size);die("unbound original shader buffer",b->slot);}
           uint64_t offset=0;VPool *p=pool_read(r.addr,r.size,&offset);
           uint64_t alignment=b->kind==2?G.properties.limits.minUniformBufferOffsetAlignment:G.properties.limits.minStorageBufferOffsetAlignment;
           uint64_t prefix=offset%alignment;
           if(plan_safe){if(b->kind!=2||prefix)plan_safe=0;else plan_resources[i].buffer=(BindingBuffer){.range=r,.pool_serial=p->serial,.offset=offset};}
           if(prefix&&S.read_snapshots_allowed&&(s->storage_bias.mask&(1ull<<i))&&prefix%4==0&&prefix/4<=UINT32_MAX&&r.size%4==0&&prefix<=G.properties.limits.maxStorageBufferRange&&r.size<=G.properties.limits.maxStorageBufferRange-prefix){
               /* Keep the complete guest range in its original pool. Shader
                * indices compensate for the aligned descriptor prefix. Any
                * queued producer write is made visible before the first read
                * after it; later readonly draws need no additional copy. */
               direct_storage_read(r);S.storage_bias[s->h.stage][i]=(uint32_t)(prefix/4);
               bi[i]=(VkDescriptorBufferInfo){p->v.b,offset-prefix,r.size+prefix};G.profile_storage_bias_bindings++;G.profile_storage_bias_bytes+=r.size;
           }else if(prefix||(((s->dyn_bindings>>i)&1)&&offset>UINT32_MAX)){
               if(r.size>=1024*1024&&!(s->large_bindings_seen&(1ull<<i))){
                   s->large_bindings_seen|=1ull<<i;
                   fprintf(stderr,"[nvn-vulkan] UNALIGNED_LARGE_BIND shader=%s stage=%u binding=%u kind=%u slot=%d bytes=%llu prefix=%llu alignment=%llu writable=%u\n",s->key,s->h.stage,b->binding,b->kind,b->slot,(unsigned long long)r.size,(unsigned long long)(offset%alignment),(unsigned long long)alignment,b->flags);
               }
               AlignedBinding *aligned=NULL;
               for(int j=0;j<S.naligned;j++)if(overlap(r.addr,r.size,S.aligned[j].range.addr,S.aligned[j].range.size)){
                   if(r.addr!=S.aligned[j].range.addr || r.size!=S.aligned[j].range.size)die("overlapping unaligned shader ranges",r.addr);
                   aligned=&S.aligned[j];break;
               }
               if(!aligned){if(S.naligned==2*LIMIT_BINDINGS)die("aligned binding count",S.naligned);
                   if(offset%4 || r.size%4)die("unaligned copy requires word granularity",offset);
                   VBuffer *temp=NULL;int snapshot_index=cached_snapshot(r,&temp);
                   if(!temp){
                   temp=temporary_named((r.size+3)&~3ULL,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,TEMP_ALIGNED);
                   int cpu_only=(b->kind==2||((b->kind==3||b->kind==9)&&!b->flags))&&!G.disable_cpu_uniform_snapshot;
                   for(int j=0;j<S.nwrites&&cpu_only;j++)if(!S.write_cpu[j]&&overlap(r.addr,r.size,S.writes[j].addr,S.writes[j].size))cpu_only=0;
                   if(cpu_only){
                       G.profile_cpu_snapshots++;
                       /* Unwritten bytes come from CPU backing; every queued
                        * overlapping write has an exact CPU payload. Overlay
                        * those immutable command payloads in recording order
                        * to snapshot the value this draw would read on GPU. */
                       pool_conflict_wait(p,offset,r.size,0);memcpy(temp->map,p->v.map+offset,r.size);
                       for(int j=0;j<S.nwrites;j++)if(S.write_cpu[j]&&overlap(r.addr,r.size,S.writes[j].addr,S.writes[j].size)){
                           Range w=S.writes[j];uint64_t lo=r.addr>w.addr?r.addr:w.addr,hi=r.addr+r.size<w.addr+w.size?r.addr+r.size:w.addr+w.size;
                           memcpy(temp->map+(lo-r.addr),S.write_cpu[j]+(lo-w.addr),hi-lo);
                       }
                   }else{
                       G.profile_gpu_copies++;
                       all_barrier();VkBufferCopy copy={offset,0,r.size};vkCmdCopyBuffer(S.command,p->v.b,temp->b,1,&copy);all_barrier();
                   }
                   if(snapshot_index>=0){S.read_snapshots[snapshot_index].buffer=temp;S.read_snapshots[snapshot_index].write_count=S.nwrites;}
                   }
                   aligned=&S.aligned[S.naligned++];*aligned=(AlignedBinding){r,temp,0};
               }
               if(b->kind==3&&b->flags)aligned->written=1;
               bi[i]=(VkDescriptorBufferInfo){aligned->buffer->b,0,r.size};
           }else bi[i]=(VkDescriptorBufferInfo){p->v.b,offset,r.size};
           if(b->kind==3 && b->flags)mark_write(r.addr,r.size);
           if((s->dyn_bindings>>i)&1){
               live_dyn[i]=(uint32_t)bi[i].offset;
               bi[i].offset=0;
               /* Keep the declared UBO extent: Turnip uses it for bounds when
                * adding the per-bind dynamic offset. */
               bi[i].range=r.size;
           }
           if(G.capture_feedback&&(b->kind==2||b->kind==9)){
               uint64_t bytes=bi[i].range;if(bytes%4)die("feedback uniform diagnostic alignment",bytes);
               VBuffer *capture=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);all_barrier();
               VkBufferCopy copy={bi[i].offset,0,bytes};vkCmdCopyBuffer(S.command,bi[i].buffer,capture->b,1,&copy);
               char name[160];snprintf(name,sizeof(name),"feedback-effective-%04llu-%llu-%s-ubo%d",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,s->h.stage?"ps":"vs",b->slot);trace_capture_queue(capture,bytes,name);
           }
       } else if(b->kind==4 || b->kind==6 || b->kind==11) {
           if(b->slot<0 || b->slot>=(b->kind!=4?32:128))die("texture shader slot",b->slot);
           if(b->kind==11 && b->type!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->type!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)die("image-bank query descriptor type",b->type);
           uint64_t handle=b->kind!=4?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot];
           unsigned tid=(unsigned)handle&0xfffff;
           if(texture_binding_null(handle)){
               if(!G.null_descriptors){S.null_bindings[s->h.stage]|=1ull<<i;w[i].descriptorCount=0;plan_safe=0;continue;}
               if(!G.fetch_sampler){VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=VK_LOD_CLAMP_NONE};CHECK(vkCreateSampler(G.device,&ci,NULL,&G.fetch_sampler));}
               ii[i]=(VkDescriptorImageInfo){.sampler=b->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?G.fetch_sampler:VK_NULL_HANDLE,
                   .imageView=VK_NULL_HANDLE,.imageLayout=b->kind==6?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
               w[i].pImageInfo=&ii[i];plan_safe=0;continue;
           }
           if(tid>=(unsigned)G.texture_pool_n || tid>=G.texture_slots || !G.tslot[tid].live)die("unbound texture descriptor",tid);
           TSlot *slot=&G.tslot[tid];
           VTexture *t=slot_texture(slot,1);
           if(b->kind==6&&!(t->usage&VK_IMAGE_USAGE_STORAGE_BIT)){
               fprintf(stderr,"[nvn-vulkan] STORAGE_USAGE_REQUIRED shader=%s binding=%u serial=%llu flags=%#x usage=%#x\n",
                   s->key,b->binding,(unsigned long long)t->serial,t->source.d.flags,t->usage);
               die("storage image without declared Vulkan usage",t->serial);
           }
           if(G.capture_feedback&&b->kind==4&&G.state.rt.depth&&G.state.rt.depth->serial==t->serial){
               unsigned sid=(unsigned)((b->sampler>=0?G.state.samp[s->h.stage][b->sampler]:handle)>>20)&0xfff;
               const samp_par *p=sid<LIMIT_OBJECTS&&G.sslot[sid].live?&G.sslot[sid].source:NULL;
               fprintf(stderr,"[nvn-vulkan] FEEDBACK_SAMPLE shader=%s binding=%u slot=%d extent=%u,%u levels=%u layers=%u sampler=%u live=%d\n",s->key,b->binding,b->slot,t->width,t->height,t->source.d.levels,t->source.L.layers,sid,p!=NULL);
               const view_t *v=&slot->view,*a=&G.state.rt.dview;
               fprintf(stderr,"[nvn-vulkan] FEEDBACK_VIEW serial=%llu image=%p format=%u samples=%u sampled_flags=%u,%u,%u,%u,%u levels=%d,%d layers=%d,%d format=%d swizzle=%u,%u,%u,%u depth_mode=%u attachment_flags=%u,%u levels=%d,%d layers=%d,%d\n",(unsigned long long)t->serial,(void *)t->image,t->format,t->source.d.samples,slot->has_view&&v->set_levels,slot->has_view&&v->set_layers,slot->has_view&&v->set_format,slot->has_view&&v->set_swizzle,slot->has_view&&v->set_depth_mode,v->base_level,v->nlevels,v->min_layer,v->nlayers,v->format,v->swizzle[0],v->swizzle[1],v->swizzle[2],v->swizzle[3],v->depth_mode,G.state.rt.has_dview&&a->set_levels,G.state.rt.has_dview&&a->set_layers,a->base_level,a->nlevels,a->min_layer,a->nlayers);
               if(p)fprintf(stderr,"[nvn-vulkan] FEEDBACK_SAMPLER min=%u mag=%u wrap=%u,%u,%u lod=%g,%g bias=%g aniso=%g compare=%u,%u\n",p->minf,p->magf,p->ws,p->wt,p->wr,p->lod_min,p->lod_max,p->lod_bias,p->aniso,p->cmp_mode,p->cmp_func);
               fprintf(stderr,"[nvn-vulkan] FEEDBACK_IMAGE_STATE serial=%llu source_target=%u source_format=%u source_extent=%u,%u,%u sampled_has_view=%u sampled_target=%u,%d attachment_has_view=%u attachment_target=%u,%d attachment_format=%u,%d attachment_swizzle=%u,%u,%u,%u,%u attachment_depth_mode=%u,%u raster_samples=%u samples=%u\n",(unsigned long long)t->serial,t->source.d.target,t->source.d.format,t->source.d.width,t->source.d.height,t->source.d.depth,slot->has_view,v->set_target,v->target,G.state.rt.has_dview,a->set_target,a->target,a->set_format,a->format,a->set_swizzle,a->swizzle[0],a->swizzle[1],a->swizzle[2],a->swizzle[3],a->set_depth_mode,a->depth_mode,G.state.ms.raster_samples,G.state.ms.samples);
           }
           if(adapted_shader_dir()&&!(s->texture_layout_logged&(1ull<<i))){
               s->texture_layout_logged|=1ull<<i;
               const view_t *v=&slot->view;
               fprintf(stderr,"[nvn-vulkan] TEXTURE_LAYOUT shader=%s stage=%u binding=%u slot=%d tid=%u serial=%llu target=%u layers=%u levels=%u has_view=%u view_target=%u,%d view_layers=%u,%d,%d view_levels=%u,%d,%d\n",
                   s->key,s->h.stage,b->binding,b->slot,tid,(unsigned long long)t->serial,t->source.d.target,t->source.L.layers,t->source.d.levels,slot->has_view,v->set_target,v->target,v->set_layers,v->min_layer,v->nlayers,v->set_levels,v->base_level,v->nlevels);
           }
           trace_texture(t,s,b);
           int alias=b->kind==4&&(s->dref_bindings&(1ull<<i))&&aspect(t->format)==VK_IMAGE_ASPECT_COLOR_BIT;
           int sampled_copy=b->kind==4&&!alias&&sampling_copy_required(t,slot,s->h.stage);
           int ranged=b->kind==4&&!alias&&disjoint_sampling(t,slot,s->h.stage)&&texture_range_ok(t);
           if(!G.feedback_layout && !sampled_copy && !ranged && b->kind==4 && s->h.stage!=5){
               for(int rt=0;rt<G.state.rt.n;rt++)if(G.state.rt.color[rt]&&G.state.rt.color[rt]->serial==t->serial){
                   view_t *v=&slot->view,*a=&G.state.rt.cview[rt];
                   fprintf(stderr,"[nvn-vulkan] COLOR_FEEDBACK_VIEW shader=%s binding=%u serial=%llu sampled_levels=%d,%d flags=%u sampled_layers=%d,%d flags=%u attachment_level=%d flags=%u attachment_layer=%d flags=%u\n",s->key,b->binding,(unsigned long long)t->serial,v->base_level,v->nlevels,slot->has_view&&v->set_levels,v->min_layer,v->nlayers,slot->has_view&&v->set_layers,a->base_level,G.state.rt.has_cview[rt]&&a->set_levels,a->min_layer,G.state.rt.has_cview[rt]&&a->set_layers);
               }
           }
           if(ranged){view_t *rv=&slot->view;unsigned rl=slot->has_view&&rv->set_levels?rv->base_level:0,rn=slot->has_view&&rv->set_levels?rv->nlevels:t->source.d.levels;
               texture_transition_levels(t,rl,rn,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
           else if(!alias&&!sampled_copy)texture_transition(t,storage_image_use(t->serial)?VK_IMAGE_LAYOUT_GENERAL:sampled_layout(t,s->h.stage));
           if(alias&&!t->shadow_view){fprintf(stderr,"[nvn-vulkan] missing comparison view shader=%s binding=%u slot=%d serial=%llu stage=%u\n",s->key,b->binding,b->slot,(unsigned long long)t->serial,s->h.stage);die("comparison image preparation",t->serial);}
           if(sampled_copy&&!t->sample_view)die("disjoint sample copy preparation",t->serial);
           VkImage selected_image=alias?t->shadow:sampled_copy?t->sample_copy:t->image;VkFormat selected_format=alias?t->shadow_format:t->format;
           ii[i].imageView=alias?t->shadow_view:sampled_copy?t->sample_view:t->view;ii[i].imageLayout=alias?t->shadow_layout:sampled_copy?t->sample_layout:ranged?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:t->layout;
           /* Size queries consume only the image from this descriptor. NVN
            * image handles contain no sampler; supply a valid unused sampler
            * when the translator declares a combined descriptor. */
           if(b->kind==11 && b->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)ii[i].sampler=fetch_sampler();
           if(slot->has_view||(s->image_type_mask&(1ull<<i))) {
               view_t no_view={0};const view_t *v=slot->has_view?&slot->view:&no_view;
               uint32_t level=v->set_levels?v->base_level:0,levels=v->set_levels?v->nlevels:t->source.d.levels;
               /* NVN image bindings address one selected mip. The registered
                * texture descriptor may retain the full sampled mip count;
                * storage operations and their size queries have no mip LOD. */
               if(b->kind==6 || b->kind==11)levels=1;
               uint32_t layer=v->set_layers?v->min_layer:0,layers=v->set_layers?v->nlayers:t->source.L.layers;
               int target=v->set_target?v->target:t->source.d.target;
               if(level>=t->source.d.levels || !levels || levels>t->source.d.levels-level ||
                  layer>=t->source.L.layers || !layers || layers>t->source.L.layers-layer ||
                  (v->set_format && v->format!=t->source.d.format) ||
                  (target!=NVN_TEXTURE_TARGET_1D && target!=NVN_TEXTURE_TARGET_1D_ARRAY && target!=NVN_TEXTURE_TARGET_2D && target!=NVN_TEXTURE_TARGET_3D && target!=NVN_TEXTURE_TARGET_2D_ARRAY && target!=NVN_TEXTURE_TARGET_CUBEMAP && target!=NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) ||
                  ((target==NVN_TEXTURE_TARGET_1D||target==NVN_TEXTURE_TARGET_2D) && layers!=1)) {
                   fprintf(stderr,"[nvn-vulkan] texture view id=%u flags=%u,%u,%u,%u levels=%d,%d layers=%d,%d format=%d/%d target=%d/%d\n",
                       tid,v->set_levels,v->set_layers,v->set_format,v->set_target,v->base_level,v->nlevels,v->min_layer,v->nlayers,
                       v->format,t->source.d.format,v->target,t->source.d.target);
                   fprintf(stderr,"[nvn-vulkan] texture source serial=%llu levels=%u layers=%u extent=%u,%u,%u\n",(unsigned long long)t->serial,t->source.d.levels,t->source.L.layers,t->width,t->height,t->depth);
                   die("texture view format/target/layer/range adaptation",tid);
               }
               int cube_compatible=t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP||t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY||
                   (!alias&&!sampled_copy&&t->source.d.target==NVN_TEXTURE_TARGET_2D_ARRAY&&t->width==t->height&&t->source.L.layers>=6&&t->width<=G.properties.limits.maxImageDimensionCube);
               if(s->image_type_mask&(1ull<<i))target=shader_image_target(&s->image_types[i],t,layers,cube_compatible);
               if((target==NVN_TEXTURE_TARGET_CUBEMAP||target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)&&layer%6)die("cube shader image base layer",layer);
               VkImageViewType view_type=texture_view_type(target);
               /* Existing base cube views select their type by face count. */
               int base_target=t->source.d.target;
               if(base_target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY&&t->source.L.layers==6)base_target=NVN_TEXTURE_TARGET_CUBEMAP;
               if(level || levels!=t->source.d.levels || layer || layers!=t->source.L.layers || view_type!=texture_view_type(base_target) || v->set_swizzle || v->depth_mode) {
                   VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=selected_image,.viewType=view_type,
                       .format=selected_format,.subresourceRange={aspect(selected_format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,level,levels,layer,layers}};
                   if(v->set_swizzle)vi.components=(VkComponentMapping){v->swizzle[0]+1,v->swizzle[1]+1,v->swizzle[2]+1,v->swizzle[3]+1};
                   if(v->depth_mode){if(!(aspect(selected_format)&VK_IMAGE_ASPECT_STENCIL_BIT))die("stencil view requires stencil texture",selected_format);vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;}
                   ii[i].imageView=submission_image_view(&vi);
                   if(target!=(v->set_target?v->target:t->source.d.target)&&!(s->typed_view_logged&(1ull<<i))){
                       s->typed_view_logged|=1ull<<i;
                       fprintf(stderr,"[nvn-vulkan] IMAGE_VIEW_TYPED shader=%s binding=%u source_target=%u target=%d view_type=%u layers=%u level=%u levels=%u aspect=%#x\n",s->key,b->binding,t->source.d.target,target,view_type,layers,level,levels,vi.subresourceRange.aspectMask);
                   }
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
               }else {ii[i].sampler=sampler_for_format(&G.sslot[sid],selected_format);
                   if(G.trace_frame&&s->h.stage==1&&!strcmp(s->key,"5c1ca4e7c3e4657c")){
                       samp_par *p=&G.sslot[sid].source;
                       fprintf(stderr,"[nvn-vulkan] TEXTURE_SAMPLER draw=%llu binding=%u slot=%d serial=%llu sampler=%u minf=%u magf=%u lod=%g,%g bias=%g compare=%u,%u view_levels=%d,%d set_levels=%u\n",(unsigned long long)G.draws,b->binding,b->slot,(unsigned long long)t->serial,sid,p->minf,p->magf,p->lod_min,p->lod_max,p->lod_bias,p->cmp_mode,p->cmp_func,slot->view.base_level,slot->view.nlevels,slot->has_view&&slot->view.set_levels);
                   }
               } }
           if(ranged&&ii[i].imageView==t->view)die("ranged sampling without a subresource view",t->serial);
           if(b->kind==6){t->cpu_owned=0;tex_dirty(t);}
           if(plan_safe){
               int custom_view=slot->has_view&&(slot->view.set_levels||slot->view.set_layers||slot->view.set_format||slot->view.set_target);
               if(b->kind!=4||alias||sampled_copy||ranged||custom_view||t->source.d.target!=NVN_TEXTURE_TARGET_2D||ii[i].imageView!=t->view)
                   plan_safe=0;
               else plan_resources[i].image=(BindingImage){.texture=texture_reference(t),.descriptor_id=tid,
                   .layout=ii[i].imageLayout,.sampler=ii[i].sampler};
           }
           w[i].pImageInfo=&ii[i];continue;
       } else die("Vulkan shader resource kind",b->kind);
       if(G.capture_feedback&&(b->kind==1||b->kind==7)){
           uint64_t bytes=bi[i].range;VBuffer *capture=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);all_barrier();
           VkBufferCopy copy={bi[i].offset,0,bytes};vkCmdCopyBuffer(S.command,bi[i].buffer,capture->b,1,&copy);
           char name[160];snprintf(name,sizeof(name),"feedback-effective-%04llu-%llu-%s-%s",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,s->h.stage?"ps":"vs",b->kind==1?"support":"c1");trace_capture_queue(capture,bytes,name);
       }
       w[i].pBufferInfo=&bi[i];
    }
    if(s->dyn_count){
        uint32_t k=0;for(uint32_t i=0;i<s->h.nb;i++)if((s->dyn_bindings>>i)&1)S.dyn_offset[s->h.stage][k++]=live_dyn[i];
        S.dyn_count[s->h.stage]=k;
    }
    uint32_t nb=s->h.nb;size_t half=(size_t)nb*sizeof(VkDescriptorBufferInfo);uint32_t slot=0;uint64_t hash=0;
    if(cacheable){
        hash=14695981039346656037ULL^(uint64_t)(uintptr_t)s;const uint8_t *k0=(const uint8_t *)bi,*k1=(const uint8_t *)ii;
        const uint64_t *q0=(const uint64_t *)k0,*q1=(const uint64_t *)k1;size_t words=half/8;
        for(size_t j=0;j<words;j++){hash=(hash^q0[j])*1099511628211ULL;hash^=hash>>29;}
        for(size_t j=0;j<words;j++){hash=(hash^q1[j])*1099511628211ULL;hash^=hash>>29;}
        for(slot=(uint32_t)hash&(DESC_TABLE-1);S.desc_table[slot];slot=(slot+1)&(DESC_TABLE-1)){
            const DescEntry *e=&S.desc_entries[S.desc_table[slot]-1];
            if(e->hash==hash&&e->shader==s&&!memcmp(S.desc_arena+e->off,bi,half)&&!memcmp(S.desc_arena+e->off+half,ii,half)){
                G.profile_desc_hits++;binding_plan_remember(s,e->set,plan_resources,plan_safe);return e->set;}
        }
        G.profile_desc_misses++;
        if(G.desc_diag)desc_miss_note(s,bi,ii,nb);
    }
    int skipped_binding=0;for(uint32_t i=0;i<nb;i++)if(!w[i].descriptorCount){skipped_binding=1;break;}
    uint64_t alloc_started=prof_now();set=descriptor_set_acquire(s,skipped_binding);G.profile_desc_alloc_ns+=prof_now()-alloc_started;
    uint32_t writes=0;for(uint32_t i=0;i<nb;i++)if(w[i].descriptorCount){w[writes]=w[i];w[writes++].dstSet=set;}
    uint64_t update_started=prof_now();vkUpdateDescriptorSets(G.device,writes,w,0,NULL);G.profile_desc_update_ns+=prof_now()-update_started;
    if(cacheable&&S.ndesc<DESC_ENTRIES&&S.desc_arena_used+2*half<=DESC_ARENA&&S.ndesc*2<DESC_TABLE){
        DescEntry *e=&S.desc_entries[S.ndesc];*e=(DescEntry){hash,s,set,nb,(uint32_t)S.desc_arena_used};
        memcpy(S.desc_arena+S.desc_arena_used,bi,half);memcpy(S.desc_arena+S.desc_arena_used+half,ii,half);S.desc_arena_used+=2*half;
        S.desc_table[slot]=++S.ndesc;
    }
    if(G.trace_frame&&G.trace_ui&&s->h.stage==1&&!strcmp(s->key,"62ddcb3faa41a92a")){
        for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==2||s->b[i].kind==9){uint64_t bytes=bi[i].range;
            if(bytes%4)die("UI effective uniform capture alignment",bytes);
            VBuffer *capture=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);all_barrier();VkBufferCopy copy={bi[i].offset,0,bytes};
            vkCmdCopyBuffer(S.command,bi[i].buffer,capture->b,1,&copy);char name[160];snprintf(name,sizeof(name),"ui-effective-%04llu-%llu-ps-ubo%d",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,s->b[i].slot);trace_capture_queue(capture,bytes,name);
        }
    }
    binding_plan_remember(s,set,plan_resources,plan_safe);return set;
}
static VShader *program_stage_optional_impl(int stage) {
    const prog_t *p=G.state.programs[stage];if(!p)return NULL;
    for(uint32_t i=0;i<p->nstage;i++) { const void *control=p->sd[i].control;if(!control)die("shader data control",i);
       if(G.shader_code_identity || G.shader_index_version==2) {
           const uint8_t *entry=shader_entry(control,stage,p->sd[i].data);
           if(entry)return shader_get(entry,stage);continue;
       }
       uint64_t mix=((uint64_t)(uintptr_t)control>>4^(uint64_t)stage*0x9e3779b97f4a7c15ULL)*0xbf58476d1ce4e5b9ULL;
       ControlCache *c=&G.control_cache[(mix>>40)&(CONTROL_CACHE-2)];
       if(!(c->valid&&c->control==control&&c->stage==(uint32_t)stage)){
           if(c[1].valid&&c[1].control==control&&c[1].stage==(uint32_t)stage)c++;
           else if(c->valid&&(!c[1].valid||(mix&1)))c++;
       }
       if(!G.disable_control_cache&&c->valid&&c->control==control&&c->stage==(uint32_t)stage){
           if(!memcmp(c->bytes,control,0x880)){G.profile_control_hits++;if(c->shader)return c->shader;continue;}
           G.profile_control_changed++;
       }else if(c->valid)G.profile_control_evict++;
       G.profile_control_misses++;
       const uint8_t *entry=shader_entry(control,stage,p->sd[i].data);
       VShader *found=entry?shader_get(entry,stage):NULL;
       c->control=control;c->stage=stage;c->valid=1;c->shader=found;memcpy(c->bytes,control,0x880);
       if(found)return found; }
    return NULL;
}
static VShader *program_stage_optional(int stage) {
    uint64_t started=prof_now();VShader *s=program_stage_optional_impl(stage);
    G.profile_lookup_ns+=prof_now()-started;return s;
}
static VShader *program_stage(int stage) {
    VShader *s=program_stage_optional(stage);if(!s){
        const prog_t *p=G.state.programs[stage];fprintf(stderr,"[nvn-vulkan] PROGRAM_MISSING stage=%d program=%p serial=%llu count=%u\n",stage,(const void *)p,(unsigned long long)(p?p->serial:0),p?p->nstage:0);
        if(p)for(unsigned i=0;i<p->nstage&&i<6;i++){uint32_t actual=0xffffffff;if(p->sd[i].control)memcpy(&actual,(const uint8_t *)p->sd[i].control+0x714,4);fprintf(stderr,"[nvn-vulkan] PROGRAM_STAGE index=%u actual=%u control=%p data=%llx\n",i,actual,p->sd[i].control,(unsigned long long)p->sd[i].data);}
        die("requested program stage missing",stage);
    }return s;
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
static int depth_input_identity_view(const tex_t *t,const view_t *v) {
    if(!v)return 1;
    if((v->set_levels&&(v->base_level||v->nlevels!=1))||
       (v->set_layers&&(v->min_layer||v->nlayers!=1))||
       (v->set_format&&v->format!=t->d.format)||
       (v->set_target&&v->target!=NVN_TEXTURE_TARGET_2D)||v->depth_mode)return 0;
    if(v->set_swizzle)for(unsigned i=0;i<4;i++)if(v->swizzle[i]!=NVN_TEXTURE_SWIZZLE_R+i)return 0;
    return 1;
}
static VShader *depth_input_select(VShader *vs,VShader *ps) {
    S.depth_input=0;
    if(!G.depth_order||!ps||strcmp(ps->key,"3749e309ce42949e")||!G.state.rt.depth||depth_read_only())return ps;
    RBinding *target=NULL;
    for(unsigned i=0;i<ps->h.nb;i++)if(ps->b[i].set==1&&ps->b[i].binding==3&&ps->b[i].kind==4)target=&ps->b[i];
    if(!target||target->slot<0||target->slot>=128)die("depth input calibrated shader binding",0);
    uint64_t handle=G.state.tex[1][target->slot];unsigned tid=handle&0xfffff;
    if(tid>=G.texture_slots||!G.tslot[tid].live||G.tslot[tid].texture.serial!=G.state.rt.depth->serial)return ps;
    const tex_t *depth=G.state.rt.depth;const TSlot *slot=&G.tslot[tid];
    const VkViewport *vp=&G.state.viewport;const VkRect2D *sc=&G.state.scissor;
    if(strcmp(vs->key,"4a2526c0e38b227d")||!G.fragment_stores||
       atomic_load(&G.window_origin)!=NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT||
       atomic_load(&G.depth_mode)!=NVN_DEPTH_MODE_NEAR_IS_ZERO||G.state.depth_clamp||
       vp->x!=0||vp->y!=0||vp->width!=1280||vp->height!=720||vp->minDepth!=0||vp->maxDepth!=1||
       sc->offset.x||sc->offset.y||sc->extent.width!=1280||sc->extent.height!=720||
       !G.state.ds.dt||!G.state.ds.dw||G.state.ds.df!=NVN_DEPTH_FUNC_LEQUAL||G.state.ds.st||
       G.state.polygon.offset_en||G.state.polygon_offset.factor||G.state.polygon_offset.units||G.state.polygon_offset.clamp||
       G.state.ms.cov2color_en||G.state.ms.raster_samples>1)
        die("depth input experiment outside calibrated draw state",G.draws);
    if(depth->d.width!=1280||depth->d.height!=720||depth->d.target!=NVN_TEXTURE_TARGET_2D||
       depth->d.levels!=1||depth->L.layers!=1||depth->d.samples>1||format(depth->d.format)!=VK_FORMAT_D24_UNORM_S8_UINT||
       !depth_input_identity_view(depth,G.state.rt.has_dview?&G.state.rt.dview:NULL)||
       !depth_input_identity_view(&slot->texture,slot->has_view?&slot->view:NULL))
        die("depth input experiment outside calibrated image/view",depth->serial);
    unsigned sid=(unsigned)((target->sampler>=0?G.state.samp[1][target->sampler]:handle)>>20)&0xfff;
    const samp_par *p=sid<LIMIT_OBJECTS&&G.sslot[sid].live?&G.sslot[sid].source:NULL;
    if(!p||p->minf!=NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR||p->magf!=NVN_MAG_FILTER_LINEAR||
       p->ws!=NVN_WRAP_MODE_CLAMP_TO_EDGE||p->wt!=NVN_WRAP_MODE_CLAMP_TO_EDGE||p->cmp_mode||
       p->aniso!=1||p->lod_min!=-1000||p->lod_max!=1000||p->lod_bias!=0)
        die("depth input experiment outside calibrated sampler",sid);
    if(G.diag_barrier||G.diag_loadstore||G.diag_skip_draws||G.diag_skip_class||G.diag_skip_xfer||
       G.diag_shape||G.diag_fixed_desc||G.diag_no_pass_deps||G.hz_diag||G.skip_ps[0])
        die("depth input experiment incompatible diagnostic mode",0);
    /* Ordinary sampled aliases and storage accesses must never borrow the
     * ordered input attachment's layout or semantics. */
    for(unsigned stage=0;stage<2;stage++){VShader *s=stage?ps:vs;
        for(unsigned i=0;i<s->h.nb;i++){RBinding *b=&s->b[i];if(b->kind!=4&&b->kind!=6&&b->kind!=11)continue;
            if(b->slot<0||b->slot>=(b->kind==4?128:32))die("depth input alias slot",b->slot);
            unsigned id=(b->kind==4?G.state.tex[stage][b->slot]:G.state.img[stage][b->slot])&0xfffff;
            if(id<G.texture_slots&&G.tslot[id].live&&G.tslot[id].texture.serial==depth->serial&&b!=target)
                die("depth input experiment additional image alias",b->binding);
        }
    }
    for(int i=0;i<G.state.rt.n;i++)if(G.state.rt.color[i]){
        const tex_t *c=G.state.rt.color[i];VkImageSubresourceRange r=attachment_range(c,G.state.rt.has_cview[i]?&G.state.rt.cview[i]:NULL);
        if((c->d.width>>r.baseMipLevel)<1280||(c->d.height>>r.baseMipLevel)<720)
            die("depth input experiment framebuffer intersection",c->serial);
    }
    pass_end();S.depth_input=1;depth_input_guard_prepare();
    fprintf(stderr,"[nvn-vulkan] DEPTH_INPUT_DRAW frame=%llu draw=%llu serial=%llu device_calibrated=1\n",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,(unsigned long long)depth->serial);
    return depth_input_shader(ps);
}
/* Submit the commands recorded so far so the GPU starts while the CPU keeps recording this segment.
 * Completion, writeback, events and counters are still published only after the segment's final fence. */
static void chunk_ts_write(int end) {
    if(!S.chunk_ts||S.nchunk_ts>=128)return;
    S.chunk_pass0[S.nchunk_ts]=G.pass_count;S.chunk_draw0[S.nchunk_ts]=G.draws;
    vkCmdWriteTimestamp(S.command,end?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,S.chunk_ts,S.nchunk_ts++);
}
static void sub_chunk_flush(void) {
    /* Pass queries belong to the submission, not a command buffer. Fresh query
     * prefixes are reset before first use and all chunks retire at the final
     * fence; profiling therefore need not disable normal early submission. */
    if(!S.chunk_draws||S.in_pass||S.active_occlusion!=UINT32_MAX||G.draws-S.chunk_mark<S.chunk_draws||S.chunk_index+1>=48)return;
    chunk_ts_write(1);CHECK(vkEndCommandBuffer(S.command));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&S.command};
    CHECK(vkQueueSubmit(G.queue,1,&si,VK_NULL_HANDLE));
    S.chunk_index++;
    if(S.chunk_index>=S.chunk_alloc){
        VkCommandBufferAllocateInfo a={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=S.command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        CHECK(vkAllocateCommandBuffers(G.device,&a,&S.chunk_cb[S.chunk_alloc]));S.chunk_alloc++;
    }
    S.command=S.chunk_cb[S.chunk_index];
    VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(S.command,&b));chunk_ts_write(0);
    nvn_vk_command_state_reset(&S.command_state);S.command_dirty=1;S.chunk_mark=G.draws;G.profile_chunks++;chunk_total++;
}
static p_scissor attachment_scissor(void) {
    return (p_scissor){G.state.scissor.offset.x,G.state.scissor.offset.y,
        (int)G.state.scissor.extent.width,(int)G.state.scissor.extent.height};
}
/* A load clear covers only renderArea. Unequal attachment extents therefore
 * keep the explicit whole-subresource clear; do not silently clip it. */
static int clear_can_fold(void) {
    if(!(G.attachment_flags&1)||S.flushing_clear||G.diag_shape||G.diag_loadstore)return 0;
    unsigned width=0,height=0;NvnAttachmentKey keys[9];unsigned count=0;
    for(int i=0;i<G.state.rt.n+(G.state.rt.depth?1:0);i++){
        int depth=i==G.state.rt.n;const tex_t *t=depth?G.state.rt.depth:G.state.rt.color[i];if(!t)continue;
        const view_t *v=depth?(G.state.rt.has_dview?&G.state.rt.dview:NULL):(G.state.rt.has_cview[i]?&G.state.rt.cview[i]:NULL);
        if(t->d.target==NVN_TEXTURE_TARGET_3D)return 0;
        NvnAttachmentKey key=nvn_attachment_key(t,v,0);if(key.level>=t->d.levels||key.level>=32)return 0;
        unsigned w=t->d.width>>key.level,h=t->d.height>>key.level;if(!w)w=1;if(!h)h=1;
        if(count&&(width!=w||height!=h))return 0;
        for(unsigned j=0;j<count;j++)if(nvn_attachment_equal(key,keys[j]))return 0;
        keys[count++]=key;width=w;height=h;
    }
    return count!=0;
}
static int attachment_store_dead(const tex_t *t,const view_t *v,unsigned plane) {
    if(!(G.attachment_flags&2)||!G.attachment_index_valid||G.trace_frame||G.diag_loadstore||S.flushing_clear)return 0;
    return nvn_store_dead(G.attachment_ops,G.attachment_count,G.attachment_pos,G.state.rt,
        attachment_scissor(),G.state.scissor_set,nvn_attachment_key(t,v,plane));
}
static VkRenderPass render_begin(void) {
    p_set_rt *rt=&G.state.rt;if(rt->n<0 || rt->n>8 || (!rt->n&&!rt->depth))die("render targets",rt->n);
    int read_only=S.depth_sampled&&depth_read_only();
    /* OP_SET_RT and every transfer/layout barrier already end the pass.
     * Keep ordinary consecutive draws in the same attachment configuration.
     * Feedback passes retain the original explicit pass boundaries. */
    if(S.in_pass && !G.disable_pass_reuse && !S.feedback_flags && !S.pass_feedback_flags && !S.depth_input && !S.pass_depth_input && read_only==S.pass_depth_read_only){
        for(int i=0;i<rt->n+(rt->depth?1:0);i++){
            const tex_t *source=i==rt->n?rt->depth:rt->color[i];if(!source)continue;
            VTexture *t=texture_get(source,0);t->cpu_owned=0;{const view_t *av=i==rt->n?(rt->has_dview?&rt->dview:NULL):(rt->has_cview[i]?&rt->cview[i]:NULL);tex_attach_dirty(t,av&&av->set_levels?av->base_level:0);}
        }
        return S.passes[S.active_pass];
    }
    pass_end();G.pass_side_effects=0;
    if(S.chunk_draws)sub_chunk_flush();
    VkAttachmentDescription attachments[9];VkAttachmentReference color[8],depth,input;VkImageView views[9];VkClearValue clears[9]={0};uint32_t n=0,width=0,height=0;
    uint64_t clear_bpp=0,discard_bpp=0;
    VTexture *cov_targets[9]={0};VkImageSubresourceRange cov_ranges[9];uint32_t cov_w[9],cov_h[9];
    S.pass_discard=0;
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
        t->cpu_owned=0;tex_attach_dirty(t,range.baseMipLevel);
        VkImageLayout attachment_layout=is_depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if(is_depth&&S.depth_input)attachment_layout=VK_IMAGE_LAYOUT_GENERAL;
        else if(is_depth&&S.depth_sampled&&depth_read_only())attachment_layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        else if(S.feedback_flags&(is_depth?VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT:VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT))attachment_layout=VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        VkImageLayout layout;
        if(!is_depth&&texture_range_ok(t)&&range.levelCount<t->source.d.levels){texture_transition_levels(t,range.baseMipLevel,range.levelCount,attachment_layout);layout=attachment_layout;}
        else{texture_transition(t,attachment_layout);layout=t->layout;}
        attachments[n]=(VkAttachmentDescription){.format=t->format,.samples=VK_SAMPLE_COUNT_1_BIT,
           .loadOp=G.diag_loadstore>=2?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,
           .storeOp=G.diag_loadstore>=1?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE,
           .stencilLoadOp=G.diag_loadstore>=2?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,
           .stencilStoreOp=G.diag_loadstore>=1?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE,
           .initialLayout=layout,.finalLayout=layout};
        unsigned clear_bit=is_depth?256u:1u<<i;
        clears[n]=S.clear_values[is_depth?8:i];
        if(S.pending_clear&clear_bit){attachments[n].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;G.attachment_stats.clear_loads++;
            clear_bpp+=is_depth?(t->format==VK_FORMAT_D16_UNORM?2:4):t->bpp;}
        if(is_depth&&(S.pending_clear&512u)){attachments[n].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;G.attachment_stats.clear_loads++;
            clear_bpp++;}
        if(attachment_store_dead(source,v,is_depth?1:0)){
            attachments[n].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;S.pass_discard|=clear_bit;G.attachment_stats.discard_stores++;
            discard_bpp+=is_depth?(t->format==VK_FORMAT_D16_UNORM?2:4):t->bpp;}
        if(is_depth&&(aspect(t->format)&VK_IMAGE_ASPECT_STENCIL_BIT)&&attachment_store_dead(source,v,2)){
            attachments[n].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;S.pass_discard|=512u;G.attachment_stats.discard_stores++;
            discard_bpp++;}
        if(is_depth){dbg_dload=attachments[n].loadOp;dbg_dstore=attachments[n].storeOp;}
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=t->format,.subresourceRange=range};
        views[n]=submission_image_view(&view);
        cov_targets[n]=t;cov_ranges[n]=range;cov_w[n]=tw;cov_h[n]=th;
        if(is_depth)depth=(VkAttachmentReference){n,layout};else color[i]=(VkAttachmentReference){n,layout};n++;
    }
    if(!n)die("render targets have no bound attachment",rt->n);
    G.attachment_stats.load_bytes_avoided+=(uint64_t)width*height*clear_bpp;
    G.attachment_stats.store_bytes_avoided+=(uint64_t)width*height*discard_bpp;
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=rt->n,.pColorAttachments=color,
        .pDepthStencilAttachment=rt->depth?&depth:NULL};
    VkInputAttachmentAspectReference input_aspect={.subpass=0,.inputAttachmentIndex=0,.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT};
    VkRenderPassInputAttachmentAspectCreateInfo input_info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,
        .aspectReferenceCount=1,.pAspectReferences=&input_aspect};
    if(S.depth_input){input=depth;sub.inputAttachmentCount=1;sub.pInputAttachments=&input;
        sub.flags=VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_ARM;}
    VkSubpassDependency deps[3]={
      {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       .dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
      {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
      {.srcSubpass=0,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
       .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
       .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT|VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT}};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=n,.pAttachments=attachments,
       .subpassCount=1,.pSubpasses=&sub,.dependencyCount=S.depth_input?2u:(G.diag_no_pass_deps||(G.precise_sync&&!S.feedback_flags))?0u:S.feedback_flags?3u:2u,.pDependencies=deps,
       .pNext=S.depth_input?&input_info:NULL};
    memset(&S.pass_key,0,sizeof(S.pass_key));
    S.pass_key.count=n;S.pass_key.color_count=rt->n;S.pass_key.has_depth=rt->depth!=NULL;S.pass_key.dependency_count=ri.dependencyCount;
    S.pass_key.input_count=sub.inputAttachmentCount;S.pass_key.subpass_flags=sub.flags;
    if(S.depth_input){S.pass_key.input=input;S.pass_key.input_aspect=input_aspect.aspectMask;}
    memcpy(S.pass_key.attachments,attachments,n*sizeof(*attachments));
    memcpy(S.pass_key.color,color,rt->n*sizeof(*color));if(rt->depth)S.pass_key.depth=depth;
    memcpy(S.pass_key.dependencies,deps,ri.dependencyCount*sizeof(*deps));
    cov_prepare_pass(attachments,clears,cov_targets,cov_ranges,cov_w,cov_h,n,width,height);
    FramebufferKey key;memset(&key,0,sizeof key);key.pass=S.pass_key;key.width=width;key.height=height;memcpy(key.views,views,n*sizeof(*views));
    int index=0;
    if(!G.disable_resource_cache){
        for(;index<S.npass;index++)if(!memcmp(&S.framebuffer_keys[index],&key,sizeof key))break;
    }else index=S.npass;
    if(index==S.npass){
        if(S.npass-S.retained_passes==LIMIT_WRITES)die("render pass limit",S.npass);S.npass++;
        S.passes[index]=render_pass_cached(&S.pass_key,&ri,&S.pass_owned[index]);
        VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=S.passes[index],.attachmentCount=n,
           .pAttachments=views,.width=width,.height=height,.layers=1};CHECK(vkCreateFramebuffer(G.device,&fi,NULL,&S.framebuffers[index]));
        S.framebuffer_keys[index]=key;G.profile_framebuffer_misses++;G.resource_passes++;
    }else {G.profile_framebuffer_hits++;G.resource_hits++;}
    if(G.dump_frame&&G.present_count+1==G.dump_frame&&(rt->color[0]||rt->depth)){if(dbg_dump_frame_seen!=G.dump_frame){dbg_dump_frame_seen=G.dump_frame;dbg_dump_idx=0;}
        const tex_t *dsrc=rt->color[0]?rt->color[0]:rt->depth;VkImageSubresourceRange dr=rt->color[0]?attachment_range(rt->color[0],rt->has_cview[0]?&rt->cview[0]:NULL):attachment_range(rt->depth,rt->has_dview?&rt->dview:NULL);dbg_dump_src=dsrc;dbg_dump_level=dr.baseMipLevel;dbg_dump_layer=dr.baseArrayLayer;dbg_dump_w=width;dbg_dump_h=height;}
    S.active_pass=index;G.pass_draws0=G.draws;{const tex_t *f0=rt->n>0?rt->color[0]:rt->depth;snprintf(G.pass_desc,sizeof G.pass_desc,"%ux%u c%d d%d s%llu",f0?f0->d.width:0,f0?f0->d.height:0,rt->n,rt->depth!=NULL,(unsigned long long)(f0?f0->serial:0));}
    VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=S.passes[index],.framebuffer=S.framebuffers[index],
       .renderArea={{0,0},{(G.diag_shape&4)&&width>16?16:width,(G.diag_shape&4)&&height>16?16:height}},.clearValueCount=n,.pClearValues=clears};
    if(S.pass_ts&&S.npt<500){uint32_t q=S.npt*2;query_reserve(S.pass_ts,&S.pass_reset_count,q,2,1024);
        if(S.pass_stats)query_reserve(S.pass_stats,&S.pass_stats_reset_count,S.npt,1,512);
        vkCmdWriteTimestamp(S.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,S.pass_ts,q);
        S.pt[S.npt].q0=q;S.pt[S.npt].w=width;S.pt[S.npt].h=height;S.pt[S.npt].n=rt->n;
        S.pt[S.npt].key=(rt->color[0]?rt->color[0]->serial:0)*1315423911ULL^(rt->depth?rt->depth->serial:0);S.pt_draws0=G.draws;S.pt_open=1;}
    if(G.precise_sync)hz_pass_sync();
    vkCmdBeginRenderPass(S.command,&begin,VK_SUBPASS_CONTENTS_INLINE);S.pending_clear=0;S.in_pass=1;G.pass_count++;if(G.draw_timing)dt_mark_pass(width,height,rt->n,rt->depth!=NULL);
    if(S.pass_stats){if(S.pt_open)vkCmdBeginQuery(S.command,S.pass_stats,S.npt,0);else{G.pass_stats_omitted++;G.pass_stats_window_omitted++;}}
    S.pass_feedback_flags=S.feedback_flags;S.pass_depth_read_only=read_only;S.pass_depth_input=S.depth_input;
    return S.passes[index];
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
    all_barrier();VkBufferCopy copy={offset,0,span};vkCmdCopyBuffer(S.command,pool->v.b,capture->b,1,&copy);
    char gpu_name[160];snprintf(gpu_name,sizeof(gpu_name),"mesh-%04llu-%llu-%s-gpu",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,name);trace_capture_queue(capture,bytes,gpu_name);
    S.trace_captures[S.ntrace_captures-1].offset=prefix;
}
static void command_bind_profile(unsigned kind,int issued) {
    if(issued)G.profile_cmd_issued[kind]++;else G.profile_cmd_skipped[kind]++;
}
static void draw(const p_draw *a,const p_draw_idx *idx,int instances,int base_instance) {
    if(!a&&!idx)die("draw parameters",0);
    int upper_left=atomic_load(&G.window_origin)==NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
    if(G.state.ms.samples>1 || G.state.ms.a2c || G.state.ms.cov_mod || G.state.ms.sl_en)die("multisample semantics",G.state.ms.samples);
    if(G.state.color.logic_op!=NVN_LOGIC_OP_COPY || G.state.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS)die("logic/alpha test adaptation",G.state.color.logic_op);
    if(G.state.polygon.mode!=NVN_POLYGON_MODE_FILL)die("polygon mode",G.state.polygon.mode);
    NvnVkDepthBias depth_bias;
    int bias_result=nvn_vk_depth_bias(G.state.polygon.offset_en,G.state.polygon_offset.factor,
        G.state.polygon_offset.units,G.state.polygon_offset.clamp,G.enabled_features.depthBiasClamp,&depth_bias);
    if(bias_result!=NVN_VK_DEPTH_BIAS_OK){
        fprintf(stderr,"[nvn-vulkan] DEPTH_BIAS_REJECT enables=%u factor=%g units=%g clamp=%g clamp_enabled=%u\n",
            G.state.polygon.offset_en,G.state.polygon_offset.factor,G.state.polygon_offset.units,G.state.polygon_offset.clamp,G.enabled_features.depthBiasClamp);
        die("polygon offset depth bias capability/parameters",bias_result);
    }
    /* A zero-color pass may still run fragment discard, depth export or
     * storage side effects. Only a program with no fragment stage omits it. */
    VShader *vs=program_stage(0),*ps=program_stage_optional(1);
    if(ps&&frame_dump_dir()&&((!strcmp(ps->key,"3749e309ce42949e")&&!strcmp(vs->key,"4a2526c0e38b227d"))||(!strcmp(ps->key,"73adabd418bb541e")&&!strcmp(vs->key,"72f3a02ea695e6be")))){
        char request[1200];snprintf(request,sizeof(request),"%s/%s",getenv("DM_FRAME_DIR"),!strcmp(ps->key,"3749e309ce42949e")?"feedback-request":"feedback-73ad-request");
        /* Empty/zero requests capture the first match. A positive integer
         * skips that many matching draws, enabling a later failing draw to be
         * reproduced without changing its real uniforms or depth writes. */
        int capture=0;FILE *requested=fopen(request,"r");
        if(requested){unsigned skip=0;int parsed=fscanf(requested,"%u",&skip);fclose(requested);
            if(parsed==1&&skip){if(skip>100000)die("feedback capture skip bound",skip);
                FILE *remaining=fopen(request,"w");if(!remaining||fprintf(remaining,"%u\n",skip-1)<0||fclose(remaining))die("feedback capture skip output",0);
            }else capture=1;
        }
        if(capture){
            if(unlink(request))die("feedback trace request consume",0);G.capture_feedback=G.trace_frame=1;snprintf(G.trace_shader,sizeof G.trace_shader,"%s",ps->key);G.ntrace_textures=0;
            fprintf(stderr,"[nvn-vulkan] FEEDBACK_DRAW frame=%llu draw=%llu vs=%s ps=%s origin=%d viewport=%g,%g,%g,%g depth_range=%g,%g clamp=%u depth_mode=%d\n",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,vs->key,ps->key,atomic_load(&G.window_origin),G.state.viewport.x,G.state.viewport.y,G.state.viewport.width,G.state.viewport.height,G.state.viewport.minDepth,G.state.viewport.maxDepth,G.state.depth_clamp,atomic_load(&G.depth_mode));
            fprintf(stderr,"[nvn-vulkan] FEEDBACK_POLYGON offset_enable=%u factor=%g units=%g clamp=%g\n",G.state.polygon.offset_en,G.state.polygon_offset.factor,G.state.polygon_offset.units,G.state.polygon_offset.clamp);
            for(unsigned stage=0;stage<2;stage++){VShader *s=stage?ps:vs;
                for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==2||s->b[i].kind==9){unsigned slot=s->b[i].slot;char name[48];snprintf(name,sizeof(name),"feedback-%s-ubo%u",stage?"ps":"vs",slot);Range r=G.state.uniform[stage][slot];if(r.addr&&r.size)trace_mesh_range(name,r.addr,r.size);}}
            uint8_t streams[16]={0};for(unsigned i=0;i<vs->h.na;i++){unsigned location=vs->attr[i][0];if(location>=16)die("feedback diagnostic attribute",location);va_st attr=G.state.attributes[location];if(attr.stream>=16)die("feedback diagnostic stream",attr.stream);streams[attr.stream]=1;
                fprintf(stderr,"[nvn-vulkan] FEEDBACK_VERTEX location=%u stream=%u format=%u offset=%u stride=%u divisor=%u\n",location,attr.stream,attr.fmt,attr.offset,G.state.streams[attr.stream].stride,G.state.streams[attr.stream].divisor);}
            for(unsigned i=0;i<16;i++)if(streams[i]&&G.state.vbuf[i].size){char name[32];snprintf(name,sizeof(name),"feedback-vbuf%u",i);if(G.state.vbuf[i].size<=16*1024*1024)trace_mesh_range(name,G.state.vbuf[i].address,G.state.vbuf[i].size);else fprintf(stderr,"[nvn-vulkan] FEEDBACK_BUFFER_SKIPPED stream=%u bytes=%llu\n",i,(unsigned long long)G.state.vbuf[i].size);}
            if(idx){trace_mesh_range("feedback-indices",idx->ibuf,(uint64_t)idx->count*(idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?2:4));fprintf(stderr,"[nvn-vulkan] FEEDBACK_INDEX count=%d instances=%d base_vertex=%d base_instance=%u type=%u primitive=%u\n",idx->count,idx->instances,idx->base_vertex,idx->base_instance,idx->itype,idx->prim);}
            else fprintf(stderr,"[nvn-vulkan] FEEDBACK_ARRAY first=%d count=%d instances=%d base_instance=%d primitive=%d\n",a->first,a->count,instances,base_instance,a->prim);
            /* Capture every sampled texture before binding can stop at the
             * unsupported depth descriptor partway through the binding list. */
            for(unsigned i=0;i<ps->h.nb;i++)if(ps->b[i].kind==4){
                unsigned tid=(unsigned)G.state.tex[1][ps->b[i].slot]&0xfffff;
                if(tid<G.texture_slots&&G.tslot[tid].live)trace_texture(slot_texture(&G.tslot[tid],1),ps,&ps->b[i]);
            }
        }
    }
    int trace_postprocess=G.trace_frame&&!G.trace_ui&&!G.capture_feedback&&ps&&G.trace_shader[0]&&
        (!strcmp(ps->key,G.trace_shader)||!strcmp(ps->key,"68ea18a1b9bbe5f3"));
    /* The proven ClipDistance[0] == Position.z plane is supplied by Vulkan's
     * native zero-to-one clipping only with depth clamping disabled. */
    if(vs->requires_native_depth_clip&&(G.state.depth_clamp||atomic_load(&G.depth_mode)!=NVN_DEPTH_MODE_NEAR_IS_ZERO))die("redundant clip requires unclamped zero-to-one depth",G.state.depth_clamp);
    if(G.pass_timing&&ps&&G.state.rt.n==1&&G.state.rt.color[0]&&G.state.rt.color[0]->d.width==1280&&G.state.rt.color[0]->d.height==720){
        uint32_t k=0;while(k<G.nhot&&strcmp(G.hot[k].key,ps->key))k++;
        if(k==G.nhot&&k<64){strcpy(G.hot[k].key,ps->key);G.hot[k].draws=0;G.nhot++;}
        if(k<G.nhot)G.hot[k].draws++;}
    ps=depth_input_select(vs,ps);
    if(G.skip_ps[0]&&ps&&!strcmp(ps->key,G.skip_ps))return;
    S.read_snapshots_allowed=1;
    for(unsigned stage=0;stage<2;stage++){VShader *s=stage?ps:vs;if(!s)continue;
        for(unsigned i=0;i<s->h.nb;i++){RBinding *b=&s->b[i];if(((b->kind==3||b->kind==9)&&b->flags)||b->kind==6||b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)S.read_snapshots_allowed=0;}
    }
    if(!S.read_snapshots_allowed)G.pass_side_effects=1;
    G.draw_store=!S.read_snapshots_allowed||S.depth_input;
    if(G.profile_detail){snprintf(G.last_ps,sizeof G.last_ps,"%s",ps?ps->key:"none");G.last_count=a?a->count:idx->count;}
    if(G.precise_sync&&G.draw_store){pass_end();hz_add(1u<<HZ_GV|1u<<HZ_GF,HZ_ATT_STAGES|HZ_SHADER_STAGES,HZ_ATT_ACCESS|VK_ACCESS_SHADER_WRITE_BIT);}
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
    if(trace_postprocess){
        for(unsigned stage=0;stage<2;stage++){VShader *s=stage?ps:vs;
            for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==2||s->b[i].kind==9){unsigned slot=s->b[i].slot;char name[48];snprintf(name,sizeof(name),"post-%s-ubo%u",stage?"ps":"vs",slot);Range r=G.state.uniform[stage][slot];if(r.addr&&r.size)trace_mesh_range(name,r.addr,r.size);}}
        for(unsigned i=0;i<16;i++)if(G.state.vbuf[i].size){char name[32];snprintf(name,sizeof(name),"post-vbuf%u",i);trace_mesh_range(name,G.state.vbuf[i].address,G.state.vbuf[i].size);}
        if(idx){trace_mesh_range("post-indices",idx->ibuf,(uint64_t)idx->count*(idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?2:4));
            fprintf(stderr,"[nvn-vulkan] POST_INDEX draw=%llu type=%u base_vertex=%d base_instance=%u count=%d primitive=%u\n",(unsigned long long)G.draws,idx->itype,idx->base_vertex,idx->base_instance,idx->count,idx->prim);}
        blend_st b=G.state.blend[0];fprintf(stderr,"[nvn-vulkan] POST_BLEND draw=%llu shader=%s enable=%u src=%u,%u dst=%u,%u equation=%u,%u channels=%#x\n",(unsigned long long)G.draws,ps->key,G.state.color.blend_en,b.src_rgb,b.src_a,b.dst_rgb,b.dst_a,b.eq_rgb,b.eq_a,G.state.mask.mask);
    }
    if(G.state.rt.n&&!ps)die("color draw program fragment stage missing",1);
    if(G.draws<9 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_TRACE draw=%llu vs=%s ps=%s count=%d instances=%d rt=%llu viewport=%g,%g,%g,%g scissor=%d,%d,%u,%u cull=%u front=%u depth=%u,%u,%u blend=%u mask=%#x\n",
        (unsigned long long)G.draws,vs->key,ps?ps->key:"none",a?a->count:idx->count,a?instances:idx->instances,
        (unsigned long long)(G.state.rt.n&&G.state.rt.color[0]?G.state.rt.color[0]->serial:0),G.state.viewport.x,G.state.viewport.y,G.state.viewport.width,G.state.viewport.height,
        G.state.scissor.offset.x,G.state.scissor.offset.y,G.state.scissor.extent.width,G.state.scissor.extent.height,
        G.state.polygon.cull,G.state.polygon.front,G.state.ds.dt,G.state.ds.dw,G.state.ds.df,G.state.color.blend_en,G.state.mask.mask);
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_DEPTH_STATE draw=%llu range=%g,%g stencil=%u func=%u,%u masks=%#x,%#x write=%#x,%#x ref=%#x,%#x ops=%u,%u,%u/%u,%u,%u\n",
        (unsigned long long)G.draws,G.state.viewport.minDepth,G.state.viewport.maxDepth,G.state.ds.st,G.state.ds.sf_f,G.state.ds.sf_b,
        G.state.stencil_compare[0],G.state.stencil_compare[1],G.state.stencil_write[0],G.state.stencil_write[1],G.state.stencil_ref[0],G.state.stencil_ref[1],
        G.state.ds.fail_f,G.state.ds.zfail_f,G.state.ds.zpass_f,G.state.ds.fail_b,G.state.ds.zfail_b,G.state.ds.zpass_b);
    S.feedback_flags=0;S.depth_sampled=0;S.nimage_uses=0;collect_image_uses(vs);collect_image_uses(ps);
    /* A sampled/storage view can observe a clear before the attachment pass,
     * including another mip of an image tracked with whole-image layouts. */
    for(unsigned u=0;S.pending_clear&&u<S.nimage_uses;u++){
        for(int i=0;i<8;i++)if((S.pending_clear&(1u<<i))&&G.state.rt.color[i]&&G.state.rt.color[i]->serial==S.image_uses[u].serial){pending_clears_flush();break;}
        if((S.pending_clear&768u)&&G.state.rt.depth&&G.state.rt.depth->serial==S.image_uses[u].serial)pending_clears_flush();
    }
    G.xition_vertex=0;for(unsigned i=0;i<vs->h.nb;i++)if(vs->b[i].kind==4||vs->b[i].kind==6)G.xition_vertex=1;
    uint64_t bind_started=prof_now();
    prepare_sampling_copies(vs);prepare_sampling_copies(ps);prepare_comparison_images(vs);prepare_comparison_images(ps);
    G.profile_prep_ns+=prof_now()-bind_started;
    VkDescriptorSet descriptor_sets[2]={bind_shader(vs),ps?bind_shader(ps):VK_NULL_HANDLE};
    uint32_t draw_dyn[2*LIMIT_BINDINGS];uint32_t draw_ndyn=S.dyn_count[vs->h.stage];
    memcpy(draw_dyn,S.dyn_offset[vs->h.stage],draw_ndyn*sizeof(*draw_dyn));
    if(ps){
        memcpy(draw_dyn+draw_ndyn,S.dyn_offset[ps->h.stage],S.dyn_count[ps->h.stage]*sizeof(*draw_dyn));
        draw_ndyn+=S.dyn_count[ps->h.stage];
    }
    if(G.diag_fixed_desc){static VkDescriptorSet last_sets[2];static VShader *last_vs,*last_ps;static uint64_t last_epoch;
        if(last_epoch==S.epoch&&last_vs==vs&&last_ps==ps){descriptor_sets[0]=last_sets[0];descriptor_sets[1]=last_sets[1];}
        else{last_epoch=S.epoch;last_vs=vs;last_ps=ps;last_sets[0]=descriptor_sets[0];last_sets[1]=descriptor_sets[1];}}
    G.profile_bind_ns+=prof_now()-bind_started;
    if(G.profile_detail){int n=0;G.last_uses[0]=0;for(unsigned u=0;u<S.nimage_uses&&n<90;u++)n+=snprintf(G.last_uses+n,sizeof G.last_uses-n,"%llu%s ",(unsigned long long)S.image_uses[u].serial,(S.image_uses[u].roles&2)?"w":"");}
    G.xition_vertex=0;
    if(S.active_occlusion==UINT32_MAX)query_reserve(S.occlusion,&S.occlusion_reset_count,S.nocc,1,LIMIT_WRITES);
    uint64_t render_started=prof_now();render_begin();G.profile_render_ns+=prof_now()-render_started;
    VkVertexInputAttributeDescription attributes[32];VkVertexInputBindingDescription bindings[16];uint32_t nbindings=0,nattributes=0,locations=0;uint8_t used[16]={0};
    NvnVkVertexAdapt vertex_plan={0};int has_default=0;
    for(unsigned l=0;l<16;l++)if(vs->vertex_inputs[l].type)locations|=1u<<l;
    for(uint32_t i=0;i<vs->h.na;i++) { unsigned location=vs->attr[i][0];if(location>=16)die("vertex attribute count",location);
        va_st v=G.state.attributes[location];if(v.stream>=16)die("vertex stream",v.stream);
        if(adapted_shader_dir()&&!vs->vertex_layout_logged)fprintf(stderr,"[nvn-vulkan] VERTEX_LAYOUT shader=%s location=%u shader_kind=%u format=%u kind=%u stream=%u offset=%u\n",vs->key,location,vs->attr[i][1],v.fmt,vertex_scalar_kind(v.fmt),v.stream,v.offset);
        if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] VERTEX_INPUT draw=%llu location=%u shader_type=%u stream=%u format=%u offset=%u stride=%u divisor=%u address=%#llx size=%llu\n",(unsigned long long)G.draws,location,vs->attr[i][1],v.stream,v.fmt,v.offset,G.state.streams[v.stream].stride,G.state.streams[v.stream].divisor,(unsigned long long)G.state.vbuf[v.stream].address,(unsigned long long)G.state.vbuf[v.stream].size);
        if(v.fmt==NVN_FORMAT_NONE){has_default=1;continue;}
        NvnVkVertexFormat f=nvn_vk_vertex_format(v.fmt);if(!f.native)die("unsupported NVN vertex format",v.fmt);
        NvnVkVertexInput input=vs->vertex_inputs[location];
        if(input.kind!=f.kind)die("vertex format/shader numeric type mismatch",location);
        if(f.convert){
            unsigned components=(v.fmt>=0x51&&v.fmt<=0x68)?(v.fmt-0x51)/6+1:0;
            VkFormat native=nvn_vk_vertex_scaled_native(v.fmt,components);
            VkFormat head=nvn_vk_vertex_scaled_native(v.fmt,2),tail=nvn_vk_vertex_scaled_native(v.fmt,1);
            int native_ok=native&&(format_properties(native).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
            int split_ok=components==3&&head&&tail&&
                (format_properties(head).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)&&
                (format_properties(tail).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
            if((native_ok&&(!G.force_vertex_split||!f.tail))||split_ok){f.native=native;f.head=head;f.tail=components==3?tail:VK_FORMAT_UNDEFINED;f.convert=0;}
        }
        int split=f.tail&&(G.force_vertex_split||!(format_properties(f.native).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT));
        VkFormat head=split?f.head:f.native;
        if(!(format_properties(head).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)||
           (split&&!(format_properties(f.tail).bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)))die("vertex format has no supported fetch",v.fmt);
        if(v.offset>G.properties.limits.maxVertexInputAttributeOffset||G.state.streams[v.stream].stride>G.properties.limits.maxVertexInputBindingStride)die("vertex input device limits",location);
        NVNbufferRange range=G.state.vbuf[v.stream];uint32_t stride=G.state.streams[v.stream].stride,divisor=G.state.streams[v.stream].divisor;
        if((range.address+v.offset)%f.alignment||(stride&&stride%f.alignment))die("vertex fetch component alignment",location);
        uint64_t last=0;int reads=0;
        if(a){if(a->count<0||a->first<0)die("draw arrays bounds",a->count);reads=a->count!=0;if(!divisor&&reads)last=(uint64_t)a->first+(uint64_t)a->count-1;}
        else {if(idx->count<0||idx->instances<0)die("indexed draw counts",idx->count);reads=idx->count&&idx->instances;if(divisor&&reads)last=(uint64_t)idx->base_instance+(uint64_t)idx->instances-1;}
        /* Indexed per-vertex extents may be GPU-authored. Never scan stale CPU
         * indices to guess their bounds; only array and instance extents here. */
        if(reads&&!nvn_vk_vertex_extent(range.size,v.offset,f.bytes,stride,last))die("vertex fetch outside bound range",location);
        if(nattributes>=32)die("vertex attribute limit",nattributes);
        attributes[nattributes++]=(VkVertexInputAttributeDescription){location,v.stream,head,v.offset};used[v.stream]=1;
        vertex_plan.convert[location]=f.convert;
        if(split&&input.components>=3){
            unsigned tail=0;while(tail<32&&(locations&(1u<<tail)))tail++;
            if(tail>=32||tail>=G.properties.limits.maxVertexInputAttributes||nattributes>=32)die("RGB vertex fallback attribute limit",tail);
            uint64_t offset=(uint64_t)v.offset+f.tail_offset;if(offset>G.properties.limits.maxVertexInputAttributeOffset)die("RGB vertex tail offset limit",offset);
            locations|=1u<<tail;vertex_plan.split|=1u<<location;vertex_plan.tail[location]=tail;
            attributes[nattributes++]=(VkVertexInputAttributeDescription){tail,v.stream,f.tail,(uint32_t)offset};
        }
    }
    if(has_default){
        unsigned binding=0;while(binding<16&&used[binding])binding++;if(binding==16)die("default vertex attribute binding limit",16);
        if(!G.default_vertex.b){G.default_vertex=buffer_make(32,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);const float value[4]={0,0,0,1};const uint32_t integer[4]={0,0,0,1};memcpy(G.default_vertex.map,value,sizeof value);memcpy(G.default_vertex.map+16,integer,sizeof integer);}
        for(uint32_t i=0;i<vs->h.na;i++)if(G.state.attributes[vs->attr[i][0]].fmt==NVN_FORMAT_NONE){
            unsigned kind=vs->attr[i][1];if(kind>2)die("default vertex attribute scalar type",kind);
            if(nattributes>=32)die("default vertex attribute limit",nattributes);
            attributes[nattributes++]=(VkVertexInputAttributeDescription){vs->attr[i][0],binding,kind==2?VK_FORMAT_R32G32B32A32_UINT:kind==1?VK_FORMAT_R32G32B32A32_SINT:VK_FORMAT_R32G32B32A32_SFLOAT,kind?16:0};
        }
        bindings[nbindings++]=(VkVertexInputBindingDescription){binding,0,VK_VERTEX_INPUT_RATE_VERTEX};
        command_bind_profile(NVN_VK_CMD_VERTEX,nvn_vk_cmd_vertex(&S.command_state,G.enable_command_cache&&!G.trace_frame,S.command,binding,G.default_vertex.b,0));
    }
    if(nattributes>G.properties.limits.maxVertexInputAttributes)die("vertex attribute count device limit",nattributes);
    vs->vertex_layout_logged=1;
    for(unsigned i=0;i<16;i++)if(used[i]) { vs_st v=G.state.streams[i];if(v.divisor>1)die("vertex divisor",v.divisor);
        bindings[nbindings++]=(VkVertexInputBindingDescription){i,v.stride,v.divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};
        NVNbufferRange r=G.state.vbuf[i];if(!r.address || !r.size)die("unbound vertex stream",i);uint64_t offset=0;VPool *p=pool_read(r.address,r.size,&offset);
        command_bind_profile(NVN_VK_CMD_VERTEX,nvn_vk_cmd_vertex(&S.command_state,G.enable_command_cache&&!G.trace_frame,S.command,i,p->v.b,offset)); }
    VkPipelineInputAssemblyStateCreateInfo ai={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=topology(a?a->prim:idx->prim)};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.depthClampEnable=G.state.depth_clamp,
       .cullMode=G.state.polygon.cull==NVN_FACE_FRONT?VK_CULL_MODE_FRONT_BIT:G.state.polygon.cull==NVN_FACE_BACK?VK_CULL_MODE_BACK_BIT:
          G.state.polygon.cull==NVN_FACE_FRONT_AND_BACK?VK_CULL_MODE_FRONT_AND_BACK:VK_CULL_MODE_NONE,
       // An upper-left NVN window retains NDC y upwards; a negative Vulkan
       // viewport restores that mapping. Facing follows the same inversion.
        .frontFace=(G.state.polygon.front==NVN_FRONT_FACE_CCW)==upper_left?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE,.lineWidth=1,
        .depthBiasEnable=depth_bias.enabled,.depthBiasConstantFactor=depth_bias.constant,
        .depthBiasSlopeFactor=depth_bias.slope,.depthBiasClamp=depth_bias.clamp};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=G.state.ds.dt,
        .depthWriteEnable=G.state.ds.dw,.depthCompareOp=(VkCompareOp)(G.state.ds.df-1),.stencilTestEnable=G.state.ds.st,
        .flags=S.depth_input?VK_PIPELINE_DEPTH_STENCIL_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_ARM:0};
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
    int32_t draw_parameters[3]={a?a->first:idx->base_vertex,a?base_instance:idx->base_instance,0};
    PsoKey key;memset(&key,0,sizeof(key));
    key.shader[0]=vs->identity;key.shader[1]=ps?ps->identity:0;
    memcpy(key.storage_bias[0],S.storage_bias[0],sizeof key.storage_bias[0]);if(ps)memcpy(key.storage_bias[1],S.storage_bias[1],sizeof key.storage_bias[1]);
    key.null_bindings[0]=S.null_bindings[0];if(ps)key.null_bindings[1]=S.null_bindings[1];
    for(unsigned f=0;f<3;f++)if(vs->draw_parameter_mask&(1u<<f))key.draw_parameters[f]=draw_parameters[f];
    key.flags=S.feedback_flags;key.topology=ai.topology;key.cull=raster.cullMode;key.front=raster.frontFace;
    key.depth_bias=depth_bias;
    key.vertex_split=vertex_plan.split;
    for(unsigned l=0;l<16;l++){if(vertex_plan.convert[l]==1)key.vertex_unsigned|=1u<<l;if(vertex_plan.convert[l]==2)key.vertex_signed|=1u<<l;}
    key.attribute_count=nattributes;key.binding_count=nbindings;key.color_count=cb.attachmentCount;
    memcpy(key.attributes,attributes,nattributes*sizeof(*attributes));memcpy(key.bindings,bindings,nbindings*sizeof(*bindings));
    key.shader_variant[1]=ps?ps->depth_input:0;
    key.depth_test=ds.depthTestEnable;key.depth_write=ds.depthWriteEnable;key.depth_compare=ds.depthCompareOp;key.stencil_test=ds.stencilTestEnable;
    key.depth_flags=ds.flags;
    key.depth_clamp=raster.depthClampEnable;
    key.stencil[0]=ds.front;key.stencil[1]=ds.back;memcpy(key.blend,blends,cb.attachmentCount*sizeof(*blends));key.pass=S.pass_key;
    /* Load/store actions do not affect Vulkan render-pass compatibility.
     * Keep the real pass descriptions intact, but avoid a new graphics PSO
     * for each clear/discard combination. */
    for(unsigned i=0;i<key.pass.count;i++){
        key.pass.attachments[i].loadOp=key.pass.attachments[i].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        key.pass.attachments[i].storeOp=key.pass.attachments[i].stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
    }
    uint64_t hash=0;Pso pso,*cached=pso_lookup(&key,&hash);int pso_pending=0;
    if(cached)pso=*cached;
    else pso=pipeline_obtain(&key,hash,vs,ps,&pso_pending);
    int cache_commands=G.enable_command_cache&&!G.trace_frame;
    /* 管线还在后台编译且本帧等待预算用完：只跳过实际绘制，其余状态照常 | Pipeline still compiling and this frame's wait budget is spent: only the actual draw is skipped, all other state proceeds as usual */
    if(!pso_pending){
    command_bind_profile(NVN_VK_CMD_PIPELINE,nvn_vk_cmd_pipeline(&S.command_state,cache_commands,S.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pso.pipeline));
    command_bind_profile(NVN_VK_CMD_DESCRIPTORS,nvn_vk_cmd_descriptors(&S.command_state,cache_commands,S.command,
        VK_PIPELINE_BIND_POINT_GRAPHICS,pso.layout,ps?2:1,descriptor_sets,draw_ndyn,draw_dyn));
    }
    if(G.state.viewport.width<=0 || G.state.viewport.height<=0)die("viewport bounds",0);
    VkViewport viewport=G.state.viewport;if(upper_left){viewport.y+=viewport.height;viewport.height=-viewport.height;}
    command_bind_profile(NVN_VK_CMD_VIEWPORT,nvn_vk_cmd_viewport(&S.command_state,cache_commands,S.command,&viewport));
    if(G.profile_detail)dsmix_note();
    command_bind_profile(NVN_VK_CMD_SCISSOR,nvn_vk_cmd_scissor(&S.command_state,cache_commands,S.command,(G.diag_shape&1)?&(VkRect2D){{0,0},{1,1}}:&G.state.scissor));
    command_bind_profile(NVN_VK_CMD_BLEND,nvn_vk_cmd_blend(&S.command_state,cache_commands,S.command,G.state.blend_color));
    /* Precise sample counts add across drawing commands. Keep one query open
     * through consecutive draws, closing it at pass/counter boundaries. This
     * preserves every NVN counter value without serializing each draw on tile
     * GPUs. Frame traces retain the original per-draw query diagnostics. */
    int skip_occlusion=G.diag_no_occlusion;
    if(!skip_occlusion&&(G.query_skip_mask&1)&&G.state.rt.n==1&&!G.state.rt.depth&&ps&&
       !strncmp(ps->key,"ce3ad3",6)){skip_occlusion=1;G.profile_occlusion_skipped_ui++;}
    if(!skip_occlusion&&(G.query_skip_mask&2)&&G.state.rt.n==0&&G.state.rt.depth&&
       G.state.rt.depth->d.width==1024&&G.state.rt.depth->d.height==1024&&G.state.rt.depth->d.levels<=1){
        skip_occlusion=1;G.profile_occlusion_skipped_shadow++;
    }
    if(S.active_occlusion==UINT32_MAX&&!skip_occlusion){
        if(!G.precise_occlusion || S.nocc==LIMIT_WRITES)die("precise occlusion query support/count",S.nocc);
        S.active_occlusion=S.nocc++;G.profile_occlusion_queries++;
        vkCmdBeginQuery(S.command,S.occlusion,S.active_occlusion,VK_QUERY_CONTROL_PRECISE_BIT);
    }
    uint32_t query=S.active_occlusion;
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_QUERY draw=%llu query=%u\n",(unsigned long long)G.draws,query);
    int skip_class=0;
    if(G.diag_skip_class){const tex_t *first=G.state.rt.n>0?G.state.rt.color[0]:G.state.rt.depth;unsigned w=first?first->d.width:0,h=first?first->d.height:0;
        int cls=(w==2100&&h==2100)?1:(w==960&&h==540)?(G.state.rt.n==0?2:G.state.rt.n==1?4:8):16;skip_class=(G.diag_skip_class&cls)!=0;}
    if(G.diag_skip_draws||skip_class||pso_pending){}
    else if(a) {if(a->count<0 || a->first<0||instances<0||base_instance<0)die("draw arrays bounds",a->count);vkCmdDraw(S.command,(G.diag_shape&2)&&a->count>3?3:a->count,instances,a->first,base_instance);}
    else { if(idx->count<0 || idx->instances<0 || (idx->itype!=NVN_INDEX_TYPE_UNSIGNED_SHORT && idx->itype!=NVN_INDEX_TYPE_UNSIGNED_INT))die("index type/bounds",idx->itype);
        uint64_t offset=0,size=(uint64_t)idx->count*(idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?2:4);VPool *p=pool_read(idx->ibuf,size,&offset);
        command_bind_profile(NVN_VK_CMD_INDEX,nvn_vk_cmd_index(&S.command_state,cache_commands,S.command,p->v.b,offset,idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32));
        vkCmdDrawIndexed(S.command,(G.diag_shape&2)&&idx->count>3?3:idx->count,idx->instances,0,idx->base_vertex,idx->base_instance); }
    if(G.draw_timing&&!(G.diag_skip_draws||skip_class))dt_mark_draw(vs->key,ps?ps->key:NULL,(uint32_t)(a?a->count:idx->count),(uint32_t)(a?instances:idx->instances));
    if(G.disable_query_batch||G.trace_frame)occlusion_end();
    if(G.trace_frame&&ps&&((G.trace_ui&&!strcmp(ps->key,"62ddcb3faa41a92a"))||trace_postprocess)&&G.state.rt.n==1){
        VTexture *t=texture_get(G.state.rt.color[0],0);if(t->bpp!=4||aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)die("UI result capture format",t->format);
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);uint64_t bytes=(uint64_t)t->width*t->height*4;
        VBuffer *capture=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
        vkCmdCopyImageToBuffer(S.command,t->image,t->layout,capture->b,1,&copy);char name[160];snprintf(name,sizeof(name),"%s-result-%04llu-%llu-%ux%u",trace_postprocess?"post":"ui",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,t->width,t->height);trace_capture_queue(capture,bytes,name);
        fprintf(stderr,"[nvn-vulkan] GPU_OUTPUT_CAPTURE name=%s draw=%llu shader=%s serial=%llu format=%u extent=%u,%u\n",name,(unsigned long long)G.draws,ps->key,(unsigned long long)t->serial,t->format,t->width,t->height);
    }
    ghost_note(a,idx,instances,base_instance,vs,ps);
    G.capture_feedback=0;G.draws++;aligned_writeback();
    /* Inspect the GPU guard before any following draw, guest publication or
     * presentation. Keep this conservative experiment synchronous. */
    if(S.depth_input){all_barrier();mtl_sub_commit();mtl_sub_begin();}
}
#include "nvn_vk_ui_ghost.inc"
static void dispatch(const p_dispatch *p) {
    S.read_snapshots_allowed=0;
    pass_end();if(p->x<0 || p->y<0 || p->z<0)die("dispatch dimensions",p->x);
    uint32_t groups[3]={(uint32_t)p->x,(uint32_t)p->y,(uint32_t)p->z};
    for(unsigned axis=0;axis<3;axis++)if(groups[axis]>G.properties.limits.maxComputeWorkGroupCount[axis])die("dispatch device group limit",axis);
    if(memcmp(G.state.dispatch,groups,sizeof groups)){memcpy(G.state.dispatch,groups,sizeof groups);G.ssbo_revision[5]++;G.binding_revision[5]++;}
    VShader *s=program_stage(5);
    S.nimage_uses=0;collect_image_uses(s);prepare_comparison_images(s);
    VkDescriptorSet set=bind_shader(s);
    PsoKey key;memset(&key,0,sizeof(key));key.compute=1;key.shader[0]=s->identity;
    memcpy(key.storage_bias[0],S.storage_bias[5],sizeof key.storage_bias[0]);
    key.null_bindings[0]=S.null_bindings[5];
    uint64_t hash=0;Pso pso,*cached=pso_lookup(&key,&hash);
    if(cached)pso=*cached;
    else pso=pipeline_obtain(&key,hash,s,NULL,NULL);
    int cache_commands=G.enable_command_cache&&!G.trace_frame;
    command_bind_profile(NVN_VK_CMD_PIPELINE,nvn_vk_cmd_pipeline(&S.command_state,cache_commands,S.command,VK_PIPELINE_BIND_POINT_COMPUTE,pso.pipeline));
    command_bind_profile(NVN_VK_CMD_DESCRIPTORS,nvn_vk_cmd_descriptors(&S.command_state,cache_commands,S.command,
        VK_PIPELINE_BIND_POINT_COMPUTE,pso.layout,1,&set,S.dyn_count[s->h.stage],S.dyn_offset[s->h.stage]));
    if(G.precise_sync)hz_sync(HZ_CS,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_UNIFORM_READ_BIT);
    vkCmdDispatch(S.command,p->x,p->y,p->z);
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
    VTexture *t=texture_get(p->tex,bc);size_t row=0,image=0,size=0;
    unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
    if(bc) {
        int bw,bh;int block=nvn_fmt_bpb(p->tex->d.format,&bw,&bh);
        if(!nvn_copy_pitch(p->r.width,p->r.height,p->r.depth,bw,bh,block,G.state.copy_stride,G.state.copy_image_stride,&row,&image,&size) ||
           (uint64_t)p->r.xoffset+p->r.width>width || (uint64_t)p->r.yoffset+p->r.height>height)
            die("compressed buffer-to-texture range/stride",row);
        for(int i=0;i<S.nwrites;++i)if(overlap(p->src,size,S.writes[i].addr,S.writes[i].size))
            die("compressed decode requires completed GPU source write",p->src);
        if(!pool_find(p->src,size,NULL))die("compressed decode source range",p->src);
        /* No queued GPU write overlaps this range (checked above), so the
         * compressed source bytes are authoritative in guest CPU memory. */
        const uint8_t *compressed=(const void *)(uintptr_t)p->src;
        if(t->bc_block&&!nvn_vk_bc_region(width,height,p->r.xoffset,p->r.yoffset,p->r.width,p->r.height))
            die("native BC buffer-to-texture block alignment",p->tex->serial);
        uint64_t block_row=((uint64_t)p->r.width+bw-1)/bw*block,block_rows=((uint64_t)p->r.height+bh-1)/bh;
        uint64_t image_bytes=t->bc_block?block_row*block_rows:(uint64_t)p->r.width*p->r.height*t->bpp;
        VBuffer *staging=temporary(image_bytes*p->r.depth,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if(t->bc_block){
            for(int z=0;z<p->r.depth;z++)for(uint64_t y=0;y<block_rows;y++)
                memcpy(staging->map+z*image_bytes+y*block_row,compressed+(size_t)z*image+y*row,(size_t)block_row);
        }else for(int z=0;z<p->r.depth;++z)if(nvn_bc_decode(p->tex->d.format,compressed+(size_t)z*image,row,p->r.width,p->r.height,staging->map+z*image_bytes,(size_t)p->r.width*t->bpp))
            die("buffer-to-texture BC decode",p->tex->d.format);
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c={.imageSubresource={aspect(t->format),level,volume?0:layer,volume?1:p->r.depth},
            .imageOffset={p->r.xoffset,p->r.yoffset,volume?p->r.zoffset:0},.imageExtent={p->r.width,p->r.height,volume?p->r.depth:1}};
        vkCmdCopyBufferToImage(S.command,staging->b,t->image,t->layout,1,&c);t->cpu_owned=0;tex_dirty(t);return;
    }
    if(!nvn_copy_pitch(p->r.width,p->r.height,p->r.depth,1,1,t->bpp,G.state.copy_stride,G.state.copy_image_stride,&row,&image,&size))
       die("buffer-to-texture pitch",G.state.copy_image_stride);
    if(row%t->bpp || row/t->bpp>UINT32_MAX || image%t->bpp || (uint64_t)p->r.xoffset+p->r.width>width || (uint64_t)p->r.yoffset+p->r.height>height)
       die("buffer-to-texture extent/stride",row);
    uint64_t offset=0;VPool *src=pool_read(p->src,size,&offset);all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c={.bufferOffset=offset,.bufferRowLength=(uint32_t)(row/t->bpp),.imageSubresource={aspect(t->format),level,volume?0:layer,volume?1:p->r.depth},
       .imageOffset={p->r.xoffset,p->r.yoffset,volume?p->r.zoffset:0},.imageExtent={p->r.width,p->r.height,volume?p->r.depth:1}};
    if(G.state.copy_image_stride) {
        /* NVN's byte image stride need not be divisible by the row stride.
         * Emit one slice/layer instead of rounding VkBufferImageCopy height. */
        c.imageSubresource.layerCount=1;c.imageExtent.depth=1;
        for(int z=0;z<p->r.depth;z++) {
            c.bufferOffset=offset+(size_t)z*image;
            c.imageSubresource.baseArrayLayer=volume?0:layer+(unsigned)z;
            c.imageOffset.z=volume?p->r.zoffset+z:0;
            vkCmdCopyBufferToImage(S.command,src->v.b,t->image,t->layout,1,&c);
        }
    }else vkCmdCopyBufferToImage(S.command,src->v.b,t->image,t->layout,1,&c);
    t->cpu_owned=0;tex_dirty(t);
}
static void clear_color(const p_clear_color *p) {
    if(p->mask!=15 || p->index<0 || p->index>=G.state.rt.n || !G.state.rt.color[p->index])die("clear-color target/mask/view",p->mask);
    VTexture *t=texture_get(G.state.rt.color[p->index],0);VkRect2D s=G.state.scissor;
    VkImageSubresourceRange range=attachment_range(&t->source,G.state.rt.has_cview[p->index]?&G.state.rt.cview[p->index]:NULL);
    unsigned w=t->width>>range.baseMipLevel,h=t->height>>range.baseMipLevel;if(!w)w=1;if(!h)h=1;
    if(G.state.scissor_set&&(!s.extent.width||!s.extent.height))return;
    int full=nvn_clear_full(&t->source,G.state.rt.has_cview[p->index]?&G.state.rt.cview[p->index]:NULL,attachment_scissor(),G.state.scissor_set);
    if(full&&clear_can_fold()){
        /* A previous pass whose store was omitted must end before the clear
         * that justified that omission. Its replacement must get a new store. */
        if(S.in_pass&&S.pass_discard)pass_end();
        if(S.in_pass){VkClearAttachment a={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=p->index};memcpy(a.clearValue.color.float32,p->rgba,16);
            VkClearRect r={{{0,0},{w,h}},0,1};vkCmdClearAttachments(S.command,1,&a,1,&r);G.attachment_stats.inline_clears++;
        }else{S.pending_clear|=1u<<p->index;memcpy(S.clear_values[p->index].color.float32,p->rgba,16);}
        t->cpu_owned=0;tex_dirty(t);return;
    }
    pending_clears_flush();
    if(s.offset.x || s.offset.y || (s.extent.width && s.extent.width!=w) || (s.extent.height && s.extent.height!=h)) {
        if(s.offset.x<0 || s.offset.y<0)die("clear scissor negative",p->index);
        if((unsigned)s.offset.x>=w || (unsigned)s.offset.y>=h)return;
        if(s.extent.width>w-s.offset.x)s.extent.width=w-s.offset.x;if(s.extent.height>h-s.offset.y)s.extent.height=h-s.offset.y;
        render_begin();VkClearAttachment a={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=p->index};memcpy(a.clearValue.color.float32,p->rgba,16);
        VkClearRect r={s,0,1};vkCmdClearAttachments(S.command,1,&a,1,&r);t->cpu_owned=0;tex_dirty(t);return;
    }
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearColorValue color;memcpy(color.float32,p->rgba,sizeof(p->rgba));
    vkCmdClearColorImage(S.command,t->image,t->layout,&color,1,&range);G.attachment_stats.transfer_clears++;t->cpu_owned=0;tex_dirty(t);
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
    if(G.state.scissor_set&&(!s.extent.width||!s.extent.height))return;
    int full=nvn_clear_full(&t->source,G.state.rt.has_dview?&G.state.rt.dview:NULL,attachment_scissor(),G.state.scissor_set);
    if(full&&clear_can_fold()){
        if(S.in_pass&&(S.pass_discard||S.pass_depth_read_only))pass_end();
        if(S.in_pass){VkClearAttachment a={.aspectMask=flags,.clearValue.depthStencil={p->depth,(uint32_t)p->stencil}};
            VkClearRect r={{{0,0},{w,h}},0,1};vkCmdClearAttachments(S.command,1,&a,1,&r);G.attachment_stats.inline_clears++;
        }else{
            if(flags&VK_IMAGE_ASPECT_DEPTH_BIT){S.pending_clear|=256u;S.clear_values[8].depthStencil.depth=p->depth;}
            if(flags&VK_IMAGE_ASPECT_STENCIL_BIT){S.pending_clear|=512u;S.clear_values[8].depthStencil.stencil=(uint32_t)p->stencil;}
        }
        t->cpu_owned=0;tex_dirty(t);return;
    }
    pending_clears_flush();
    if(s.offset.x || s.offset.y || (s.extent.width && s.extent.width!=w) || (s.extent.height && s.extent.height!=h)){
        if(s.offset.x<0 || s.offset.y<0)die("depth clear negative scissor",0);
        if((unsigned)s.offset.x>=w || (unsigned)s.offset.y>=h)return;
        if(s.extent.width>w-s.offset.x)s.extent.width=w-s.offset.x;if(s.extent.height>h-s.offset.y)s.extent.height=h-s.offset.y;
        render_begin();VkClearAttachment a={.aspectMask=flags,.clearValue.depthStencil={p->depth,(uint32_t)p->stencil}};
        VkClearRect r={s,0,1};vkCmdClearAttachments(S.command,1,&a,1,&r);t->cpu_owned=0;tex_dirty(t);return;
    }
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearDepthStencilValue value={p->depth,(uint32_t)p->stencil};
    vkCmdClearDepthStencilImage(S.command,t->image,t->layout,&value,1,&range);G.attachment_stats.transfer_clears++;t->cpu_owned=0;tex_dirty(t);
}
static void pending_clears_flush(void) {
    if(!S.pending_clear)return;
    unsigned mask=S.pending_clear;S.pending_clear=0;S.flushing_clear=1;
    for(int i=0;i<8;i++)if(mask&(1u<<i)){
        p_clear_color c={.index=i,.mask=15};memcpy(c.rgba,S.clear_values[i].color.float32,16);
        /* The pending clear captured full coverage, regardless of subsequent
         * scissor changes recorded before this observation boundary. */
        VkRect2D saved=G.state.scissor;int set=G.state.scissor_set;G.state.scissor=(VkRect2D){0};G.state.scissor_set=0;
        clear_color(&c);G.state.scissor=saved;G.state.scissor_set=set;
    }
    if(mask&768u){p_clear_ds c={S.clear_values[8].depthStencil.depth,(mask&256u)!=0,(int)S.clear_values[8].depthStencil.stencil,(mask&512u)?255:0};
        VkRect2D saved=G.state.scissor;int set=G.state.scissor_set;G.state.scissor=(VkRect2D){0};G.state.scissor_set=0;
        clear_ds(&c);G.state.scissor=saved;G.state.scissor_set=set;
    }
    S.flushing_clear=0;
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
    if(!p->is_uint && nvn==p->tex->d.format &&
       (nvn==NVN_FORMAT_R11G11B10F || nvn==NVN_FORMAT_R16F || nvn==NVN_FORMAT_RG16F || nvn==NVN_FORMAT_RGBA16F) &&
       !p->r.xoffset && !p->r.yoffset && (unsigned)p->r.width==w && (unsigned)p->r.height==h &&
       (!volume || (!p->r.zoffset && (unsigned)p->r.depth==z))) {
        /* Vulkan performs the target float packing, including R11G11B10,
         * for complete mip/layer clears. Partial clears retain the checked
         * staging path below. */
        VkClearColorValue value;memcpy(value.float32,p->c.f,sizeof value.float32);
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,level,1,volume?0:layer+p->r.zoffset,volume?1:p->r.depth};
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(S.command,t->image,t->layout,&value,1,&range);t->cpu_owned=0;tex_dirty(t);return;
    }
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
    vkCmdCopyBufferToImage(S.command,staging->b,t->image,t->layout,1,&c);
    t->cpu_owned=0;tex_dirty(t); /* GPU now owns the image; sampled access must preserve it. */
}
static VkImageSubresourceLayers copy_layers(const tex_t *t,VkFormat gpu_format,const view_t *v,const NVNcopyRegion *r) {
    if(v&&copy_view_log())fprintf(stderr,"[nvn-vulkan] COPY_VIEW serial=%llu target=%d format=%d view_format=%u:%d view_target=%u:%d levels=%u:%d,%d layers=%u:%d,%d region=%d,%d,%d,%d,%d,%d\n",(unsigned long long)t->serial,t->d.target,t->d.format,v->set_format,v->format,v->set_target,v->target,v->set_levels,v->base_level,v->nlevels,v->set_layers,v->min_layer,v->nlayers,r->xoffset,r->yoffset,r->zoffset,r->width,r->height,r->depth);
    unsigned level=v&&v->set_levels?v->base_level:0,base=v&&v->set_layers?v->min_layer:0;
    int volume=t->d.target==NVN_TEXTURE_TARGET_3D;
    if(level>=t->d.levels || (v&&v->set_levels&&v->nlevels<1) || (v&&v->set_format&&v->format!=t->d.format) ||
       (v&&v->set_target&&v->target!=t->d.target) || r->xoffset<0 || r->yoffset<0 || r->zoffset<0 || r->width<1 || r->height<1 || r->depth<1)
        die("texture copy view/region",t->serial);
    unsigned w=t->d.width>>level,h=(t->d.target==NVN_TEXTURE_TARGET_1D || t->d.target==NVN_TEXTURE_TARGET_1D_ARRAY)?1:t->d.height>>level;
    unsigned z=volume?t->d.depth>>level:t->L.layers;if(!w)w=1;if(!h)h=1;if(!z)z=1;
    if((uint64_t)r->xoffset+r->width>w || (uint64_t)r->yoffset+r->height>h || (uint64_t)(volume?0:base)+r->zoffset+r->depth>z)
        die("texture copy extent",t->serial);
    /* Use the resident representation: BC can be native or CPU-decoded. */
    return (VkImageSubresourceLayers){aspect(gpu_format),level,volume?0:base+r->zoffset,volume?1:r->depth};
}
#include "nvn_vk_depth_copy.h"
static void copy_t2t(const p_copy_t2t *p) {
    static unsigned copies;
    if(copies++<8||G.trace_frame)fprintf(stderr,"[nvn-vulkan] COPY_REPLAY frame=%llu src=%llu dst=%llu src_mem=%p dst_mem=%p src_extent=%u,%u dst_extent=%u,%u sr=%d,%d,%d,%d,%d,%d dr=%d,%d,%d,%d,%d,%d flags=%#x src_view=%u:%d,%d,%d,%d dst_view=%u:%d,%d,%d,%d\n",
        (unsigned long long)G.present_count+1,(unsigned long long)p->src->serial,(unsigned long long)p->dst->serial,p->src->mem,p->dst->mem,p->src->d.width,p->src->d.height,p->dst->d.width,p->dst->d.height,
        p->sr.xoffset,p->sr.yoffset,p->sr.zoffset,p->sr.width,p->sr.height,p->sr.depth,p->dr.xoffset,p->dr.yoffset,p->dr.zoffset,p->dr.width,p->dr.height,p->dr.depth,p->flags,
        p->has_sv,p->sv.base_level,p->sv.nlevels,p->sv.min_layer,p->sv.nlayers,p->has_dv,p->dv.base_level,p->dv.nlevels,p->dv.min_layer,p->dv.nlayers);
    if(p->flags&~(NVN_COPY_FLAGS_ENGINE_2D_BIT|NVN_COPY_FLAGS_LINEAR_FILTER_BIT))die("texture copy flags",p->flags);
    if(p->sr.width<0 || p->sr.height<0 || p->sr.depth<0 || p->dr.width<0 || p->dr.height<0 || p->dr.depth<0)die("texture copy negative extent",0);
    if(!p->sr.width || !p->sr.height || !p->sr.depth || !p->dr.width || !p->dr.height || !p->dr.depth) {
        fprintf(stderr,"[nvn-vulkan] COPY_EMPTY src=%d,%d,%d dst=%d,%d,%d\n",p->sr.width,p->sr.height,p->sr.depth,p->dr.width,p->dr.height,p->dr.depth);return;
    }
    uint32_t decoded_format;int decoded_bpp;
    int bc_destination=nvn_bc_unpacked(p->dst->d.format,&decoded_format,&decoded_bpp);
    /* A partial BC update must retain the other compressed blocks already
     * present in guest storage, even when the destination is first used here. */
    VTexture *s=texture_get(p->src,1),*d=texture_get(p->dst,bc_destination);
    int sv=p->src->d.target==NVN_TEXTURE_TARGET_3D,dv=p->dst->d.target==NVN_TEXTURE_TARGET_3D;
    int same_format=s->format==d->format && p->src->d.format==p->dst->d.format;
    if(s==d || sv!=dv)die("texture copy alias/dimension",p->dst->serial);
    VkImageSubresourceLayers sl=copy_layers(p->src,s->format,p->has_sv?&p->sv:NULL,&p->sr),dl=copy_layers(p->dst,d->format,p->has_dv?&p->dv:NULL,&p->dr);
    if(sl.aspectMask!=dl.aspectMask)die("texture copy aspect mismatch",p->dst->serial);
    if(s->bc_block||d->bc_block){
        unsigned sw=s->width>>sl.mipLevel,sh=s->height>>sl.mipLevel,dw=d->width>>dl.mipLevel,dh=d->height>>dl.mipLevel;
        if(!sw)sw=1;if(!sh)sh=1;if(!dw)dw=1;if(!dh)dh=1;
        if(!s->bc_block||!d->bc_block||!same_format||
           p->sr.width!=p->dr.width||p->sr.height!=p->dr.height||p->sr.depth!=p->dr.depth||
           !nvn_vk_bc_region(sw,sh,p->sr.xoffset,p->sr.yoffset,p->sr.width,p->sr.height)||
           !nvn_vk_bc_region(dw,dh,p->dr.xoffset,p->dr.yoffset,p->dr.width,p->dr.height))
            die("native BC copy requires matching block-aligned regions; disable BC passthrough",p->dst->serial);
    }
    /* Copies are normal per-frame work. Keep their resource details for startup
     * and requested frame traces without growing the on-device log per copy. */
    if(G.present_count<2 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] COPY_T2T src=%llu dst=%llu region=%d,%d,%d -> %d,%d,%d flags=%#x src_level=%u dst_level=%u src_layer=%u dst_layer=%u\n",(unsigned long long)p->src->serial,(unsigned long long)p->dst->serial,p->sr.width,p->sr.height,p->sr.depth,p->dr.width,p->dr.height,p->dr.depth,p->flags,sl.mipLevel,dl.mipLevel,sl.baseArrayLayer,dl.baseArrayLayer);
    VkOffset3D so={p->sr.xoffset,p->sr.yoffset,sv?p->sr.zoffset:0},doff={p->dr.xoffset,p->dr.yoffset,dv?p->dr.zoffset:0};
    int depth=(sl.aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT)!=0;
    G.profile_t2t_copies++;G.profile_t2t_texels+=(uint64_t)p->dr.width*p->dr.height*p->dr.depth;
    if(same_format&&p->sr.width==p->dr.width&&p->sr.height==p->dr.height&&p->sr.depth==p->dr.depth) {
        texture_transition(s,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);texture_transition(d,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);all_barrier();
        VkImageCopy c={.srcSubresource=sl,.srcOffset=so,.dstSubresource=dl,.dstOffset=doff,.extent={p->sr.width,p->sr.height,sv?p->sr.depth:1}};
        VkImageCopy copies[2]={c,c};unsigned count=1;
        if(sl.aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT){
            copies[0].srcSubresource.aspectMask=copies[0].dstSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            copies[1].srcSubresource.aspectMask=copies[1].dstSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;count=2;
        }
        vkCmdCopyImage(S.command,s->image,s->layout,d->image,d->layout,count,copies);
        if(depth)DepthCopy.plain_copies++;
    }else {
        G.profile_t2t_scaled++;
        VkFormatProperties fp=format_properties(s->format),dp=format_properties(d->format);
        if(!(p->flags&NVN_COPY_FLAGS_ENGINE_2D_BIT) || (!sv&&p->sr.depth!=p->dr.depth))die("texture scaled copy flags/layers",p->flags);
        VkFilter filter=p->flags&NVN_COPY_FLAGS_LINEAR_FILTER_BIT?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        if(depth&&filter!=VK_FILTER_NEAREST)die("depth copy requires nearest filtering",p->flags);
        if(!same_format && !((p->src->d.format==NVN_FORMAT_R11G11B10F || p->src->d.format==NVN_FORMAT_RGBA16F) &&
                            (p->dst->d.format==NVN_FORMAT_RGBA8 || p->dst->d.format==NVN_FORMAT_RGBA8_SRGB || p->dst->d.format==NVN_FORMAT_RGB10A2)))
            die("texture 2D format conversion needs numeric class validation",p->dst->d.format);
        if(filter==VK_FILTER_LINEAR&&!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))die("texture blit linear feature",s->format);
        int blit=(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)&&(dp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT);
        const char *force=depth?getenv("DM_VK_DEPTH_COPY_DRAW"):NULL;
        int drawn=depth&&(!blit||(force&&!strcmp(force,"1")));
        if(drawn){
            VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
            if(sv||dv||(fp.optimalTilingFeatures&needed)!=needed)die("drawn depth copy format/dimension",s->format);
            depth_copy_log(s->format,1);depth_copy_draw(s,d,sl,dl,&p->sr,&p->dr);
        }else{
            if(!blit)die("texture scaled copy blit unavailable",s->format);
            texture_transition(s,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);texture_transition(d,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);all_barrier();
            VkImageBlit c={.srcSubresource=sl,.srcOffsets={so,{so.x+p->sr.width,so.y+p->sr.height,so.z+(sv?p->sr.depth:1)}},
                .dstSubresource=dl,.dstOffsets={doff,{doff.x+p->dr.width,doff.y+p->dr.height,doff.z+(dv?p->dr.depth:1)}}};
            VkImageBlit copies[2]={c,c};unsigned count=1;
            if(sl.aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT){
                copies[0].srcSubresource.aspectMask=copies[0].dstSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
                copies[1].srcSubresource.aspectMask=copies[1].dstSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;count=2;
            }
            vkCmdBlitImage(S.command,s->image,s->layout,d->image,d->layout,count,copies,filter);
            if(depth){depth_copy_log(s->format,0);DepthCopy.blits++;}
        }
    }
    d->cpu_owned=0;tex_dirty(d);all_barrier();
}
static void zcull_data(const p_zcull *p,int restore) {
    if(p->address%32 || p->size<sizeof(NvnVkZCullRecord) || !pool_find(p->address,p->size,NULL))
        die("ZCull storage bounds/alignment",p->address);
    if(!G.state.rt.depth)die("ZCull without depth attachment",restore);
    VTexture *depth=texture_get(G.state.rt.depth,0);
    if(!(aspect(depth->format)&VK_IMAGE_ASPECT_DEPTH_BIT))die("ZCull non-depth attachment",depth->format);
    VkImageSubresourceRange range=attachment_range(&depth->source,G.state.rt.has_dview?&G.state.rt.dview:NULL);
    unsigned width=depth->width>>range.baseMipLevel,height=depth->height>>range.baseMipLevel;
    NvnVkZCullRecord expected=nvn_vk_zcull_record(depth->source.d.format,width?width:1,height?height:1,
        depth->source.d.samples>1?depth->source.d.samples:1,range.baseMipLevel,range.layerCount);
    all_barrier();
    uint64_t offset=0;
    if(restore) {
        /* Upload CPU-owned bytes, preserve queued GPU writes, then wait for the
         * actual fence before inspecting the saved record. This also handles
         * save/copy/restore in one command handle or across submissions. */
        pool_read(p->address,sizeof(expected),&offset);S.submission_cpu_readback=1;
        mtl_sub_commit();mtl_sub_begin();
        VPool *pool=pool_find(p->address,sizeof(expected),&offset);
        if(!pool || !pool->v.map)die("ZCull restore pool lifetime",p->address);
        NvnVkZCullRecord saved;memcpy(&saved,pool->v.map+offset,sizeof(saved));
        if(!nvn_vk_zcull_valid(&saved,&expected))die("ZCull restore incompatible/corrupt state",p->address);
        /* No external hierarchy is imported into Vulkan. Each image keeps its
         * own driver-managed HiZ; never roll back or clear real depth pixels. */
    } else {
        VPool *pool=pool_find(p->address,sizeof(expected),&offset);pool_materialize(pool);
        vkCmdUpdateBuffer(S.command,pool->v.b,offset,sizeof(expected),&expected);
        mark_write(p->address,sizeof(expected));
    }
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] ZCULL_%s address=%llx bytes=%zu extent=%ux%u level=%u layers=%u conservative=1\n",
        restore?"RESTORE":"SAVE",(unsigned long long)p->address,sizeof(expected),width,height,range.baseMipLevel,range.layerCount);
}
static void payload_check(const op_hdr *h,size_t bytes) { if(h->size<8+bytes)die("command payload truncated",h->op); }
static int command_memory_readable(CommandMemoryCursor *cursor,const uint8_t *pointer,uint64_t bytes) {
    uint64_t address=(uint64_t)(uintptr_t)pointer;
    if(command_cursor_contains(cursor,S.epoch,address,bytes)){G.profile_command_lease_hits++;return 1;}
    G.profile_command_resolves++;VPool *pool=pool_find(address,bytes,NULL);
    if(!pool){cursor->valid=0;return 0;}
    *cursor=(CommandMemoryCursor){(uint64_t)(uintptr_t)pool->guest,pool->size,pool->serial,S.epoch,1};
    return 1;
}
static void execute(const uint8_t *p,unsigned depth) {
    if(depth>32)die("nested command recursion",depth);
    CommandMemoryCursor cursor={0};
    for(unsigned guard=0;guard<1000000;guard++) {
        if(!command_memory_readable(&cursor,p,8))die("command memory outside pools",(long long)(uintptr_t)p);
        op_hdr h;memcpy(&h,p,sizeof(h));if(h.size<8 || h.size%8 || !command_memory_readable(&cursor,p,h.size))die("command record size",h.size);
        G.profile_command_records++;G.profile_command_bytes+=h.size;
        const uint8_t *a=p+8;
        if(G.attachment_index_valid){
            if(G.attachment_pos>=G.attachment_count||G.attachment_ops[G.attachment_pos]!=p)G.attachment_index_valid=0;
            else G.attachment_pos++;
        }
        if(S.pending_clear && !nvn_attachment_state_op(h.op) && h.op!=OP_SET_RT &&
           h.op!=OP_CLEAR_COLOR && h.op!=OP_CLEAR_DS && h.op!=OP_DRAW && h.op!=OP_DRAW_IDX)pending_clears_flush();
        switch(h.op) {
        case OP_END:return;
        case OP_BEGIN:break; /* Structural record; GPU binding state persists across command handles. */
        case OP_JUMP:payload_check(&h,8);memcpy(&p,a,8);continue;
        case OP_CALL:{const uint8_t *next;payload_check(&h,8);memcpy(&next,a,8);execute(next,depth+1);break;}
        case OP_SIGNAL_EVENT:{Event e;payload_check(&h,16);memcpy(&e.addr,a,8);memcpy(&e.mode,a+8,4);memcpy(&e.value,a+12,4);
            if(G.event_trace)fprintf(stderr,"[nvn-event] SIGNAL_REPLAY frame=%llu record=%p address=%p mode=%u value=%u\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)e.addr,e.mode,e.value);
            mtl_sub_event(e.addr,e.mode,e.value);break;}
        case OP_REPORT_COUNTER:{uint64_t type,address;payload_check(&h,16);memcpy(&type,a,8);memcpy(&address,a+8,8);mtl_sub_counter(address,type);break;}
        case OP_RESET_COUNTER:{uint64_t type;payload_check(&h,8);memcpy(&type,a,8);
            if(type!=NVN_COUNTER_TYPE_SAMPLES_PASSED)die("GPU counter reset type implementation pending",type);
            occlusion_end();counter_op(0,0,0);break;}
        case OP_SAVE_ZCULL:case OP_RESTORE_ZCULL:
            payload_check(&h,sizeof(p_zcull));zcull_data((const p_zcull *)a,h.op==OP_RESTORE_ZCULL);break;
        case OP_DEBUG:{payload_check(&h,sizeof(p_debug));const p_debug *d=(const void *)a;
            if(d->action<1 || d->action>3 || !d->length || d->length>4096)die("debug record fields",d->action);
            payload_check(&h,sizeof(*d)+d->length);const char *label=(const char *)(d+1);
            if(label[d->length-1])die("debug record string bounds",d->length);
            if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] DEBUG_GROUP action=%u domain=%#x text=%s\n",d->action,d->domain,label);break;}
        case OP_COPY_B2B:{uint64_t src,dst,len;payload_check(&h,24);memcpy(&src,a,8);memcpy(&dst,a+8,8);memcpy(&len,a+16,8);mtl_sub_copy(src,dst,len);break;}
        case OP_UPDATE_UB:{uint64_t dst,len;payload_check(&h,16);memcpy(&dst,a,8);memcpy(&len,a+8,8);if(len>h.size-24)die("uniform update payload",len);
            G.profile_uniform_updates++;
            all_barrier();uint64_t off=0;VPool *pool=pool_find(dst,len,&off);if(!pool || off%4 || len%4 || len>65536)die("uniform update bounds/alignment",len);
            pool_materialize(pool);vkCmdUpdateBuffer(S.command,pool->v.b,off,len,a+16);mark_write(dst,len);S.write_cpu[S.nwrites-1]=a+16;break;}
        case OP_SET_RT:payload_check(&h,sizeof(p_set_rt));G.setrt_total++;
            if(!memcmp(&G.state.rt,a,sizeof(p_set_rt))){G.setrt_same++;if(!G.disable_setrt_skip)break;}
            pending_clears_flush();pass_end();memcpy(&G.state.rt,a,sizeof(p_set_rt));break;
        case OP_CLEAR_COLOR:payload_check(&h,sizeof(p_clear_color));clear_color((const p_clear_color *)a);break;
        case OP_CLEAR_DS:payload_check(&h,sizeof(p_clear_ds));clear_ds((const p_clear_ds *)a);break;
        case OP_VIEWPORTS:{payload_check(&h,sizeof(p_viewports));const p_viewports *v=(const void *)a;if(v->first || v->n!=1)die("multiple viewports",v->n);
            /* NVN SetViewports records only x/y/w/h; depth range has its own
             * command and survives viewport changes. */
            G.state.viewport.x=v->v[0][0];G.state.viewport.y=v->v[0][1];
            G.state.viewport.width=v->v[0][2];G.state.viewport.height=v->v[0][3];break;}
        case OP_SCISSOR:{payload_check(&h,sizeof(p_scissor));const p_scissor *s=(const void *)a;if(s->x<0||s->y<0||s->w<0||s->h<0)die("scissor bounds",s->x);
            G.state.scissor=(VkRect2D){{s->x,s->y},{s->w,s->h}};G.state.scissor_set=1;break;}
        case OP_DEPTH_RANGE:{payload_check(&h,sizeof(p_depth_range));const p_depth_range *d=(const void *)a;G.state.viewport.minDepth=d->n;G.state.viewport.maxDepth=d->f;break;}
        case OP_BIND_PROGRAM:{payload_check(&h,sizeof(p_bind_program));const p_bind_program *b=(const void *)a;
            if(G.present_count<3)fprintf(stderr,"[nvn-vulkan] BIND_PROGRAM pointer=%p mask=%#x count=%u serial=%llu\n",(const void *)b->prog,b->stages,b->prog?b->prog->nstage:0,(unsigned long long)(b->prog?b->prog->serial:0));
            if(b->stages&~63)die("program stage mask",b->stages);
            for(unsigned stage=0;stage<6;stage++)if(b->stages&(1u<<stage))G.state.programs[stage]=b->prog;
            break;}
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
            Range *dest=h.op==OP_BIND_UBO?G.state.uniform[b->stage]:G.state.storage[b->stage];Range next={b->addr,b->size};
            if(memcmp(&dest[b->index],&next,sizeof(next))){dest[b->index]=next;G.binding_revision[b->stage]++;if(h.op==OP_BIND_SSBO||b->stage==5)G.ssbo_revision[b->stage]++;}break;}
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
            size_t bytes=(size_t)b->n*sizeof(uint64_t);if(bytes&&memcmp(dest+b->first,b->h,bytes)){memcpy(dest+b->first,b->h,bytes);G.binding_revision[b->stage]++;G.texbind_revision[b->stage]++;}break;}
        case OP_DRAW:payload_check(&h,sizeof(p_draw));draw((const p_draw *)a,NULL,1,0);break;
        case OP_DRAW_IDX:payload_check(&h,sizeof(p_draw_idx));draw(NULL,(const p_draw_idx *)a,0,0);break;
        case OP_DRAW_INSTANCED:{payload_check(&h,sizeof(p_draw_instanced));const p_draw_instanced *d=(const void *)a;draw(&d->draw,NULL,d->instances,d->base_instance);break;}
        case OP_DISPATCH:payload_check(&h,sizeof(p_dispatch));dispatch((const p_dispatch *)a);break;
        case OP_BARRIER:{payload_check(&h,4);int32_t bits;memcpy(&bits,a,4);G.barrier_bits[bits&255]++;G.last_barrier_bits=bits;
            /* A pass with no shader-written memory has nothing for a barrier to order: attachment hazards are covered by
             * the render-pass external dependencies and layout transitions at the pass boundaries. */
            if(G.barrier_pass_elide&&S.in_pass&&!G.pass_side_effects){G.barrier_kept[bits&255]++;break;}
            if(!S.in_pass)G.barrier_nopass++;else G.barrier_sidefx++;
            all_barrier_soft();break;} /* Full barrier is stronger than every supported NVN barrier bit. */
        case OP_POLY_OFFSET:{payload_check(&h,sizeof(p_poly_offset));const p_poly_offset *v=(const void *)a;
            if(!isfinite(v->factor)||!isfinite(v->units)||!isfinite(v->clamp))die("non-finite polygon offset",0);
            G.state.polygon_offset=*v;
            break;}
        case OP_DEPTH_CLAMP:{payload_check(&h,sizeof(uint32_t));uint32_t enabled;memcpy(&enabled,a,4);
            if(enabled>1||(enabled&&!G.depth_clamp_available))die("depth clamp feature/state unavailable",enabled);
            G.state.depth_clamp=enabled;break;}
        case OP_WAIT_EVENT:{uint64_t address;uint32_t mode,value;payload_check(&h,16);
            memcpy(&address,a,8);memcpy(&mode,a+8,4);memcpy(&value,a+12,4);
            if(!pool_find(address,4,NULL) || address%4 || mode>1)die("event wait storage/mode",address);
            uint32_t before_cut=atomic_load((_Atomic uint32_t *)(uintptr_t)address),projected=before_cut;unsigned prior_same=0,pending_cpu_writes=0,pending_counter_reports=0;uint64_t pending_write_bytes=0;
            if(G.event_trace){
                for(int i=0;i<S.nwrites;i++){pending_write_bytes+=S.writes[i].size;pending_cpu_writes+=S.write_cpu[i]!=NULL;}
                for(int i=0;i<S.ncounter;i++)pending_counter_reports+=S.counter_ops[i].kind>=2;
                fprintf(stderr,"[nvn-event] WAIT_CUT_BEGIN frame=%llu record=%p address=%p mode=%u value=%u observed_before_cut=%u pending_signal_count=%d pending_write_ranges=%d pending_write_bytes=%llu pending_cpu_writes=%u pending_counter_ops=%d pending_counter_reports=%u pending_readback=%u\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)(uintptr_t)address,mode,value,before_cut,S.nevents,S.nwrites,(unsigned long long)pending_write_bytes,pending_cpu_writes,S.ncounter,pending_counter_reports,S.submission_cpu_readback);
                for(int i=0;i<S.nevents;i++)if((uint64_t)(uintptr_t)S.events[i].addr==address){
                    Event *event=&S.events[i];prior_same++;if(event->mode)projected+=event->value;else projected=event->value;
                    fprintf(stderr,"[nvn-event] WAIT_PRIOR_SIGNAL frame=%llu wait_record=%p address=%p index=%d mode=%u value=%u projected=%u\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)(uintptr_t)address,i,event->mode,event->value,projected);
                }
                int projected_ready=mode==0?projected==value:(int32_t)(projected-value)>=0;
                fprintf(stderr,"[nvn-event] WAIT_PROJECTED frame=%llu record=%p address=%p prior_same=%u projected=%u predicate_ready=%u\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)(uintptr_t)address,prior_same,projected,projected_ready);
            }
            /* The GPU queue is in order: a wait whose predicate is already satisfied by the current value plus the
             * signals recorded earlier in this replay needs neither a submission nor a CPU poll. The event values are
             * still published in recorded order when the segment completes. */
            if(!G.disable_wait_elision){
                uint32_t value_now=before_cut;
                for(int i=0;i<S.nevents;i++)if((uint64_t)(uintptr_t)S.events[i].addr==address){Event *event=&S.events[i];if(event->mode)value_now+=event->value;else value_now=event->value;}
                if(mode==0?value_now==value:(int32_t)(value_now-value)>=0){
                    if(G.event_trace)fprintf(stderr,"[nvn-event] WAIT_ELIDED address=%p mode=%u value=%u projected=%u\n",(void *)(uintptr_t)address,mode,value,value_now);
                    all_barrier_soft();G.profile_wait_elided++;break;
                }
            }
            /* Submit preceding work and publish only its actual GPU results.
             * This also handles a signal earlier in the same command handle. */
            S.submission_event_wait=1;
            mtl_sub_commit();
            uint32_t after_cut=atomic_load((_Atomic uint32_t *)(uintptr_t)address);
            uint64_t start=dm_android_nvn_clock_ns();
            uint64_t work_wait=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_EVENT);
            uint64_t polls=0;
            for(;;){uint32_t observed=atomic_load((_Atomic uint32_t *)(uintptr_t)address);
                if(mode==0 ? observed==value : (int32_t)(observed-value)>=0)break;
                if(G.event_trace)polls++;
                if(dm_android_nvn_clock_ns()-start>30000000000ULL)die("event wait exceeded diagnostic deadline",address);
                struct timespec pause={0,100000};nanosleep(&pause,NULL);
            }
            uint64_t poll_ns=dm_android_nvn_clock_ns()-start;uint32_t after_wait=atomic_load((_Atomic uint32_t *)(uintptr_t)address);
            dm_android_nvn_work_wait_end(work_wait);
            if(G.event_trace)fprintf(stderr,"[nvn-event] WAIT_CUT_END frame=%llu record=%p address=%p mode=%u value=%u before_cut=%u after_publish=%u after_wait=%u prior_same_address=%u poll_ns=%llu polls=%llu\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)(uintptr_t)address,mode,value,before_cut,after_cut,after_wait,prior_same,(unsigned long long)poll_ns,(unsigned long long)polls);
            mtl_sub_begin();break;}
        case OP_WAIT_SYNC:case OP_FENCE_SYNC:{payload_check(&h,sizeof(p_sync));const p_sync *v=(const void *)a;
            if(!v->sync || v->sync->magic!=MAGIC('S','Y','N','C'))die("sync lifetime",0);
            if(h.op==OP_WAIT_SYNC){
                if(!v->sync->fenced || v->sync->fence>G.fence+1)die("wait on unsubmitted sync",v->sync->fence);
                /* Earlier submits finish synchronously. A fence recorded in this
                 * submit is ordered by a full GPU memory/execution barrier. */
                all_barrier_soft();
            }else{
                if(v->condition<0 || v->condition>1 || v->flags&~1)die("fence condition/flags",v->flags);
                all_barrier_soft();dm_nvn_sync_publish(v->sync,G.fence+1);
                /* CPU completion remains false until vkWaitForFences finishes. */
            }break;}
        case OP_COPY_B2T:payload_check(&h,sizeof(p_copy_b2t));copy_b2t((const p_copy_b2t *)a);break;
        case OP_CLEAR_TEX:payload_check(&h,sizeof(p_clear_tex));clear_tex((const p_clear_tex *)a);break;
        case OP_COPY_T2T:payload_check(&h,sizeof(p_copy_t2t));copy_t2t((const p_copy_t2t *)a);break;
        case OP_COPY_STRIDE:payload_check(&h,sizeof(p_copy_stride));G.state.copy_stride=((const p_copy_stride *)a)->stride;break;
        case OP_COPY_IMAGE_STRIDE:payload_check(&h,sizeof(p_copy_stride));G.state.copy_image_stride=((const p_copy_stride *)a)->stride;break;
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
             if(!*current){*current=((const p_set_pool *)a)->pool;for(unsigned stage=0;stage<6;stage++){G.binding_revision[stage]++;G.ssbo_revision[stage]++;G.texbind_revision[stage]++;}}}break;
        default:die("unimplemented NVN command opcode",h.op);
        }
        p+=h.size;
    }
    die("command iteration limit",depth);
}
void mtl_exec(const uint8_t *handle) {
    if(!G.active)die("execute without submission",0);
    if(replay_cpu_depth++==0)replay_cpu_started=dm_android_nvn_thread_cpu_ns();
    execute(handle,0);
    if(--replay_cpu_depth==0){G.profile_replay_cpu_ns+=dm_android_nvn_thread_cpu_ns()-replay_cpu_started;G.profile_replay_calls++;}
}
void mtl_exec_rest(const uint64_t *handles,int n) { (void)handles;(void)n; } /* Optimization-only lookahead; replay order is unchanged. */
/* Upstream sdx_walk ordering, with Vulkan pool bounds and a bounded index.
 * Allocation/capacity failures disable the optimization, never discard data. */
static void attachment_index_walk(const uint8_t *p,unsigned depth) {
    if(depth>32){G.attachment_index_valid=0;return;}
    CommandMemoryCursor cursor={0};
    for(unsigned guard=0;guard<1000000&&G.attachment_index_valid;guard++){
        if(!command_memory_readable(&cursor,p,8)){G.attachment_index_valid=0;return;}
        op_hdr h;memcpy(&h,p,8);
        if(h.size<8||h.size%8||!command_memory_readable(&cursor,p,h.size)){G.attachment_index_valid=0;return;}
        if(G.attachment_count==G.attachment_capacity){
            size_t capacity=G.attachment_capacity?G.attachment_capacity*2:4096;
            if(capacity>262144){G.attachment_index_valid=0;return;}
            const uint8_t **ops=realloc(G.attachment_ops,capacity*sizeof(*ops));
            if(!ops){G.attachment_index_valid=0;return;}G.attachment_ops=ops;G.attachment_capacity=capacity;
        }
        G.attachment_ops[G.attachment_count++]=p;
        if(h.op==OP_END)return;
        if(h.op==OP_JUMP||h.op==OP_CALL){
            if(h.size<16){G.attachment_index_valid=0;return;}
            const uint8_t *next;memcpy(&next,p+8,8);
            if(h.op==OP_JUMP){p=next;continue;}attachment_index_walk(next,depth+1);
        }
        p+=h.size;
    }
    G.attachment_index_valid=0;
}
void mtl_sub_index(const uint64_t *handles,int n) {
    G.attachment_count=G.attachment_pos=0;G.attachment_index_valid=(G.attachment_flags&2)!=0;
    for(int i=0;i<n&&G.attachment_index_valid;i++)attachment_index_walk((const uint8_t *)(uintptr_t)handles[i],0);
}

static void swapchain_destroy_locked(void);
void dm_vk_set_window(void *native) {
    mtl_init();pthread_mutex_lock(&G.lock);async_drain_locked();CHECK(cpu_work_queue_idle(G.queue));
    /* Focus/resume notifications may publish the same BufferQueue again.
     * Retain its Vulkan surface instead of disconnecting and reconnecting the
     * producer. Invalidate only the swapchain if this window changed size. */
    if(native && G.window==native){
        if(G.swapchain && ((uint32_t)ANativeWindow_getWidth(G.window)!=G.swap_extent.width ||
                          (uint32_t)ANativeWindow_getHeight(G.window)!=G.swap_extent.height)){
            swapchain_destroy_locked();
        }
        pthread_mutex_unlock(&G.lock);return;
    }
    swapchain_destroy_locked();if(G.surface)vkDestroySurfaceKHR(G.instance,G.surface,NULL);
    if(G.window)ANativeWindow_release(G.window);G.window=native;G.swapchain=VK_NULL_HANDLE;G.surface=VK_NULL_HANDLE;G.nswap=0;
    if(G.window){ANativeWindow_acquire(G.window);VkAndroidSurfaceCreateInfoKHR i={.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,.window=G.window};
        PFN_vkCreateAndroidSurfaceKHR create=(PFN_vkCreateAndroidSurfaceKHR)dm_vk_get(G.instance,"vkCreateAndroidSurfaceKHR");
        if(!create)die("Android Vulkan surface entry",0);CHECK(create(G.instance,&i,NULL,&G.surface)); }
    pthread_mutex_unlock(&G.lock);
}
static int present_srgb(VkFormat format) {
    return format==VK_FORMAT_R8G8B8A8_SRGB||format==VK_FORMAT_B8G8R8A8_SRGB;
}
/* DM_WSI_DIAG: log what the window-system layer reports, once a second and whenever it changes. */
static void wsi_diag_note(int is_present,VkResult result) {
    static unsigned long long acq_sub,pres_sub,frames;static int last_tr=-1;static VkExtent2D last_ext;
    if(is_present){if(result==VK_SUBOPTIMAL_KHR)pres_sub++;frames++;}else if(result==VK_SUBOPTIMAL_KHR)acq_sub++;
    if(!is_present||!G.surface)return;
    VkSurfaceCapabilitiesKHR c;if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(G.physical,G.surface,&c)!=VK_SUCCESS)return;
    int changed=(int)c.currentTransform!=last_tr||c.currentExtent.width!=last_ext.width||c.currentExtent.height!=last_ext.height;
    if(changed||frames%60==0){
        fprintf(stderr,"[nvn-vulkan] WSI_DIAG frame=%llu cur_transform=0x%x supported=0x%x cur_extent=%ux%u min_ext=%ux%u max_ext=%ux%u swap_extent=%ux%u images=%u acq_suboptimal=%llu present_suboptimal=%llu%s\n",
            (unsigned long long)G.present_count,c.currentTransform,c.supportedTransforms,c.currentExtent.width,c.currentExtent.height,
            c.minImageExtent.width,c.minImageExtent.height,c.maxImageExtent.width,c.maxImageExtent.height,G.swap_extent.width,G.swap_extent.height,G.nswap,acq_sub,pres_sub,changed?" CHANGED":"");
        last_tr=(int)c.currentTransform;last_ext=c.currentExtent;}
}
static void swapchain_make(VkFormat source_format) {
    if(G.swapchain)return;if(!G.surface)die("present without Android Surface",0);
    VkBool32 supported=0;CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(G.physical,G.family,G.surface,&supported));if(!supported)die("queue surface support",0);
    VkSurfaceCapabilitiesKHR caps;CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(G.physical,G.surface,&caps));
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))die("swapchain transfer usage",caps.supportedUsageFlags);
    uint32_t n=0;CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(G.physical,G.surface,&n,NULL));if(!n)die("surface formats",0);
    VkSurfaceFormatKHR *formats=calloc(n,sizeof(*formats));if(!formats)die("surface format allocation",n);
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(G.physical,G.surface,&n,formats));VkSurfaceFormatKHR chosen=formats[0];int selected=0;
    /* Preserve the guest's encoded scanout values. A blit from sRGB to
     * UNORM decodes to linear; Android's nonlinear display then looks dark. */
    if(n==1&&formats[0].format==VK_FORMAT_UNDEFINED){chosen.format=present_srgb(source_format)?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;selected=1;}
    for(int fallback=0;fallback<2&&!selected;fallback++)for(uint32_t k=0;k<n;k++){
        VkFormat f=formats[k].format;
        if((f==VK_FORMAT_R8G8B8A8_UNORM||f==VK_FORMAT_B8G8R8A8_UNORM||present_srgb(f))&&
           formats[k].colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR&&present_srgb(f)==(present_srgb(source_format)^fallback)){
            chosen=formats[k];selected=1;break;
        }
    }
    free(formats);if(!selected)die("Android scanout surface needs supported 8-bit nonlinear format",chosen.format);
    fprintf(stderr,"[nvn-vulkan] PRESENT_ENCODING source_format=%u swap_format=%u colorspace=%u bridge=%d\n",source_format,chosen.format,chosen.colorSpace,present_srgb(source_format)!=present_srgb(chosen.format));
    G.swap_format=chosen.format;G.swap_extent=caps.currentExtent;
    if(G.swap_extent.width==UINT32_MAX)G.swap_extent=(VkExtent2D){(uint32_t)ANativeWindow_getWidth(G.window),(uint32_t)ANativeWindow_getHeight(G.window)};
    uint32_t count=caps.minImageCount+1;if(caps.maxImageCount && count>caps.maxImageCount)count=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;if(!(caps.supportedCompositeAlpha&alpha))alpha=VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if(!(caps.supportedCompositeAlpha&alpha))die("surface composite alpha",caps.supportedCompositeAlpha);
    VkSwapchainCreateInfoKHR i={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=G.surface,.minImageCount=count,
      .imageFormat=chosen.format,.imageColorSpace=chosen.colorSpace,.imageExtent=G.swap_extent,.imageArrayLayers=1,
      .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR?VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:caps.currentTransform,
      .compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};CHECK(vkCreateSwapchainKHR(G.device,&i,NULL,&G.swapchain));
    CHECK(vkGetSwapchainImagesKHR(G.device,G.swapchain,&G.nswap,NULL));if(!G.nswap)die("swapchain has no images",0);
    free(G.swap_images);free(G.swap_layouts);
    G.swap_images=calloc(G.nswap,sizeof(*G.swap_images));G.swap_layouts=calloc(G.nswap,sizeof(*G.swap_layouts));
    if(!G.swap_images || !G.swap_layouts)die("swapchain image allocation",G.nswap);
    CHECK(vkGetSwapchainImagesKHR(G.device,G.swapchain,&G.nswap,G.swap_images));
    if(G.nswap>16)die("swapchain image count",G.nswap);
    VkSemaphoreCreateInfo sem_info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(uint32_t k=0;k<G.nswap;k++)CHECK(vkCreateSemaphore(G.device,&sem_info,NULL,&G.swap_finished[k]));
}
static void swapchain_destroy_locked(void) {
    if(G.present_encoding_bridge.image)vkDestroyImage(G.device,G.present_encoding_bridge.image,NULL);
    memory_release(G.present_encoding_bridge.memory,G.present_encoding_bridge.allocation_size,DM_VK_MEMORY_PRESENT);
    memset(&G.present_encoding_bridge,0,sizeof G.present_encoding_bridge);
    for(uint32_t k=0;k<G.nswap&&k<16;k++)if(G.swap_finished[k]){vkDestroySemaphore(G.device,G.swap_finished[k],NULL);G.swap_finished[k]=VK_NULL_HANDLE;}
    if(G.swapchain)vkDestroySwapchainKHR(G.device,G.swapchain,NULL);G.swapchain=VK_NULL_HANDLE;G.nswap=0;
}
static VTexture *present_encoded_source(VTexture *source) {
    if(present_srgb(source->format)==present_srgb(G.swap_format))return source;
    int rgba=source->format==VK_FORMAT_R8G8B8A8_SRGB||source->format==VK_FORMAT_R8G8B8A8_UNORM;
    int bgra=source->format==VK_FORMAT_B8G8R8A8_SRGB||source->format==VK_FORMAT_B8G8R8A8_UNORM;
    if(!rgba&&!bgra)die("scanout encoding bridge format",source->format);
    VkFormat format=rgba?(present_srgb(G.swap_format)?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM):
        (present_srgb(G.swap_format)?VK_FORMAT_B8G8R8A8_SRGB:VK_FORMAT_B8G8R8A8_UNORM);
    VTexture *t=&G.present_encoding_bridge;
    if(t->image&&(t->width!=source->width||t->height!=source->height||t->format!=format)){
        CHECK(cpu_work_queue_idle(G.queue));vkDestroyImage(G.device,t->image,NULL);memory_release(t->memory,t->allocation_size,DM_VK_MEMORY_PRESENT);memset(t,0,sizeof(*t));
    }
    if(!t->image){
        t->width=source->width;t->height=source->height;t->depth=1;t->format=format;t->source.d.levels=1;t->source.L.layers=1;
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,
            .extent={t->width,t->height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
            .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        CHECK(vkCreateImage(G.device,&ci,NULL,&t->image));VkMemoryRequirements r;vkGetImageMemoryRequirements(G.device,t->image,&r);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        t->allocation_size=r.size;t->memory=memory_allocate(&ai,DM_VK_MEMORY_PRESENT);CHECK(vkBindImageMemory(G.device,t->image,t->memory,0));
    }
    /* Copying compatible formats preserves bits; subsequent blitting between
     * matching encodings scales those pixels without changing scanout gamma. */
    texture_transition(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={t->width,t->height,1}};
    vkCmdCopyImage(S.command,source->image,source->layout,t->image,t->layout,1,&copy);texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);return t;
}
void dm_vk_set_layer_crop(int x,int y,int width,int height) {
    if(x<0||y<0||width<0||height<0||width>INT32_MAX-x||height>INT32_MAX-y)die("vi layer crop arguments",width);
    pthread_mutex_lock(&G.lock);G.vi_crop[0]=x;G.vi_crop[1]=y;G.vi_crop[2]=width;G.vi_crop[3]=height;pthread_mutex_unlock(&G.lock);
    fprintf(stderr,"[nvn-vulkan] VI_LAYER_CROP %d,%d %dx%d\n",x,y,width,height);
}
uint64_t mtl_present(const tex_t *source,double period,const int crop[4]) {
    if(present_cpu_depth++==0)present_cpu_started=dm_android_nvn_thread_cpu_ns();
    uint64_t present_started=dm_android_nvn_clock_ns();G.seg_index=0;
    (void)period;if(!G.window)die("original game present requires Surface",0);sub_begin_ctx(&G.submission_slot2);
    VTexture *t=texture_get(source,0);if(aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)die("present depth texture",t->format);
    swapchain_make(t->format);
    if(G.present_count<2 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] PRESENT_FORMAT frame=%llu source=%llu source_mem=%p source_format=%u swap_format=%u source_extent=%u,%u swap_extent=%u,%u\n",(unsigned long long)G.present_count+1,(unsigned long long)source->serial,source->mem,t->format,G.swap_format,t->width,t->height,G.swap_extent.width,G.swap_extent.height);
    const char *capture_root=getenv("DM_FRAME_DIR");VBuffer capture={0};
    int capture_frame=capture_root && (G.trace_frame || (getenv("DM_CAPTURE_FRAMES") &&
        (G.present_count<4 || (G.present_count%120==0 && G.present_count<1000))));
    if(capture_frame && t->bpp==4){capture=buffer_create_kind((uint64_t)t->width*t->height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,1,DM_VK_MEMORY_READBACK);
        texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);VkBufferImageCopy c={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
        vkCmdCopyImageToBuffer(S.command,t->image,t->layout,capture.b,1,&c);}
    VkSemaphoreCreateInfo sem_info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore available;
    CHECK(vkCreateSemaphore(G.device,&sem_info,NULL,&available));
    uint64_t acquire_wait=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_ACQUIRE);
    uint32_t index=0;VkResult acquired=vkAcquireNextImageKHR(G.device,G.swapchain,UINT64_MAX,available,VK_NULL_HANDLE,&index);
    dm_android_nvn_work_wait_end(acquire_wait);G.pres_acq_ns+=dm_android_nvn_clock_ns()-present_started;
    if(acquired!=VK_SUCCESS && acquired!=VK_SUBOPTIMAL_KHR)die("swapchain acquisition",acquired);
    if(G.wsi_diag)wsi_diag_note(0,acquired);
    VkSemaphore finished=G.swap_finished[index];
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=G.swap_layouts[index],
       .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
       .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=G.swap_images[index],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    int x=0,y=0,w=t->width,h=t->height;if(crop && crop[2]>0 && crop[3]>0){x=crop[0];y=crop[1];w=crop[2];h=crop[3];}
    if(G.vi_crop[2]>0 && G.vi_crop[3]>0) {
        int right=x+w,bottom=y+h;
        if(G.vi_crop[0]>x)x=G.vi_crop[0];if(G.vi_crop[1]>y)y=G.vi_crop[1];
        if(G.vi_crop[0]+G.vi_crop[2]<right)right=G.vi_crop[0]+G.vi_crop[2];
        if(G.vi_crop[1]+G.vi_crop[3]<bottom)bottom=G.vi_crop[1]+G.vi_crop[3];
        w=right-x;h=bottom-y;
    }
    if(x<0||y<0||w<=0||h<=0||(uint64_t)x+w>t->width||(uint64_t)y+h>t->height)die("present crop bounds",w);
    int flip_y=atomic_load(&G.window_origin)==NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{x,flip_y?y+h:y,0},{x+w,flip_y?y:y+h,1}},
      .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int)G.swap_extent.width,(int)G.swap_extent.height,1}}};
    if(G.present_count<2||G.trace_frame)fprintf(stderr,"[nvn-vulkan] PRESENT_ORIENTATION origin=%d flip_y=%d crop=%d,%d,%d,%d\n",atomic_load(&G.window_origin),flip_y,x,y,w,h);
    VTexture *encoded=present_encoded_source(t);
    VkFormatProperties src_format=format_properties(encoded->format),dst_format=format_properties(G.swap_format);
    if(!(src_format.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(dst_format.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))die("present blit formats",t->format);
    VkFilter present_filter=src_format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    /* Upscaling 1280x720 to the panel by about 2x: cubic keeps text and edges sharper than bilinear. */
    if(G.present_cubic&&(src_format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT))present_filter=VK_FILTER_CUBIC_EXT;
    vkCmdBlitImage(S.command,encoded->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,G.swap_images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,present_filter);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
    G.swap_layouts[index]=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;CHECK(vkEndCommandBuffer(S.command));
    VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,
      .pWaitSemaphores=&available,.pWaitDstStageMask=&wait_stage,.commandBufferCount=1,.pCommandBuffers=&S.command,
      .signalSemaphoreCount=1,.pSignalSemaphores=&finished};
    VkFence fence=S.gpu_fence;
    if(S.depth_input_guard_active)die("unchecked depth input guard reached presentation",S.epoch);
    CHECK(vkQueueSubmit(G.queue,1,&si,fence));S.ticket=++G.fence;S.phase=SUBMISSION_INFLIGHT;VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,
      .pWaitSemaphores=&finished,.swapchainCount=1,.pSwapchains=&G.swapchain,.pImageIndices=&index};
    uint64_t present_wait=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_PRESENT);
    VkResult presented=vkQueuePresentKHR(G.queue,&pi);dm_android_nvn_work_wait_end(present_wait);
    if(presented!=VK_SUCCESS && presented!=VK_SUBOPTIMAL_KHR)die("queue present",presented);
    if(G.wsi_diag)wsi_diag_note(1,presented);
    int async_present=G.async_enabled&&!capture.b&&!G.trace_frame&&!S.ntrace_captures;
    if(async_present)S.present_available=available;
    else{
    if(G.slot_publication)async_drain_locked();
    uint64_t pres_t0=dm_android_nvn_clock_ns();CHECK(cpu_work_wait_fences(G.device,1,&fence,VK_TRUE,UINT64_MAX));uint64_t pres_t1=dm_android_nvn_clock_ns();G.pres_fence_ns+=pres_t1-pres_t0;G.pres_idle_ns+=dm_android_nvn_clock_ns()-pres_t1;
    submission_gpu_done(&S);
    pass_timing_collect(&S);
    trace_capture_complete(&S);
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
    vkDestroySemaphore(G.device,available,NULL);
    resource_cache_retire(&S);
    temporaries_retire(&S);
    }
    G.profile_frames++;
    {   /* 卡顿归因 | hitch attribution: counter deltas since the previous present, printed only for frames well above the recent mean */
        static uint64_t h_cpu,h_faults,h_mp_ns,h_majflt,h_inblock,h_vcsw,h_icsw;
        uint64_t now_h=dm_android_nvn_clock_ns(),cpu_h=dm_android_nvn_thread_cpu_ns(),fa_h=0,pr_h=0,mc_h=0,mpg_h=0,mn_h=0;
        nvn_dirty_stats(&fa_h,&pr_h);nvn_dirty_mp_stats(&mc_h,&mpg_h,&mn_h);
        struct rusage ru_h;getrusage(RUSAGE_SELF,&ru_h);
        if(G.hitch_last_ns){
            double dt=(now_h-G.hitch_last_ns)/1e6;uint64_t pso_sync=PW.sync_creates,pso_wait=PW.wait_ns;
            if(G.hitch_ema_ms>0&&dt>40.0&&dt>2.0*G.hitch_ema_ms&&G.hitch_count<600){
                G.hitch_count++;
                fprintf(stderr,"[nvn-vulkan] HITCH frame=%llu dt_ms=%.1f mean_ms=%.1f pipeline_ms=%.1f pso_sync=%llu pso_wait_ms=%.1f shader_load_ms=%.1f tex_decode_ms=%.1f gpu_wait_ms=%.1f replay_cpu_ms=%.1f faults=%llu mprotect_ms=%.1f majflt=%llu inblock=%llu vol_csw=%llu invol_csw=%llu\n",(unsigned long long)G.present_count+1,dt,G.hitch_ema_ms,HITCH_DELTA(G.profile_pipeline_ns,G.hitch_pipe)/1e6,(unsigned long long)(pso_sync-G.hitch_pso_sync),(pso_wait-G.hitch_pso_wait)/1e6,HITCH_DELTA(G.profile_shader_ns,G.hitch_shader)/1e6,HITCH_DELTA(G.profile_decode_ns,G.hitch_decode)/1e6,HITCH_DELTA(G.profile_wait_ns,G.hitch_wait)/1e6,
                    (cpu_h-h_cpu)/1e6,(unsigned long long)(fa_h-h_faults),(mn_h-h_mp_ns)/1e6,(unsigned long long)(ru_h.ru_majflt-h_majflt),(unsigned long long)(ru_h.ru_inblock-h_inblock),(unsigned long long)(ru_h.ru_nvcsw-h_vcsw),(unsigned long long)(ru_h.ru_nivcsw-h_icsw));
            }
            if(dt>G.hitch_max_ns/1e6)G.hitch_max_ns=(uint64_t)(dt*1e6);
            G.hitch_ema_ms=G.hitch_ema_ms>0?G.hitch_ema_ms+((dt<3*G.hitch_ema_ms?dt:3*G.hitch_ema_ms)-G.hitch_ema_ms)/32:dt;
            G.hitch_pso_sync=pso_sync;G.hitch_pso_wait=pso_wait;
        }
        h_cpu=cpu_h;h_faults=fa_h;h_mp_ns=mn_h;h_majflt=(uint64_t)ru_h.ru_majflt;h_inblock=(uint64_t)ru_h.ru_inblock;h_vcsw=(uint64_t)ru_h.ru_nvcsw;h_icsw=(uint64_t)ru_h.ru_nivcsw;
        G.hitch_last_ns=now_h;G.hitch_pipe=G.profile_pipeline_ns;G.hitch_wait=G.profile_wait_ns;G.hitch_decode=G.profile_decode_ns;G.hitch_shader=G.profile_shader_ns;
    }
    G.profile_present_ns+=dm_android_nvn_clock_ns()-present_started;
    if(--present_cpu_depth==0){G.profile_present_cpu_ns+=dm_android_nvn_thread_cpu_ns()-present_cpu_started;G.profile_present_calls++;}
    if(G.trace_frame || G.profile_frames>=60 || G.present_count<2){uint64_t now=dm_android_nvn_clock_ns();
        double elapsed_ms=(now-G.profile_start_ns)/1e6;
        double fps=elapsed_ms>0?1000.0*G.profile_frames/elapsed_ms:0;
        fprintf(stderr,"[nvn-vulkan] RENDER_PROFILE frame=%llu frames=%llu fps=%.3f elapsed_ms=%.3f pipeline_ms=%.3f gpu_wait_ms=%.3f upload_decode_ms=%.3f submits=%llu draws=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,fps,elapsed_ms,G.profile_pipeline_ns/1e6,G.profile_wait_ns/1e6,G.profile_decode_ns/1e6,(unsigned long long)G.profile_submits,(unsigned long long)(G.draws-G.profile_draws));
        if(PW.async)fprintf(stderr,"[nvn-vulkan] PSO_ASYNC frame=%llu skipped_draws_total=%llu budget_us=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)PW.skips,(unsigned long long)(PW.async_budget_ns/1000));
        fprintf(stderr,"[nvn-vulkan] RENDER_TEXTURE_COPY_PROFILE frame=%llu frames=%llu calls=%llu scaled=%llu destination_texels=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,(unsigned long long)G.profile_t2t_copies,(unsigned long long)G.profile_t2t_scaled,(unsigned long long)G.profile_t2t_texels);
        G.profile_t2t_copies=G.profile_t2t_scaled=G.profile_t2t_texels=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_SUBMIT_CLASS_PROFILE frame=%llu total_submits=%llu observed_eligible_candidates=%llu candidate_fence_wait_ms=%.3f pool_writeback_bytes=%llu events=%llu counter_reports=%llu write_submits=%llu event_submits=%llu report_submits=%llu trace_submits=%llu cpu_readback_submits=%llu event_wait_submits=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_submits,(unsigned long long)G.profile_submit_candidates,G.profile_candidate_wait_ns/1e6,(unsigned long long)G.profile_writeback_bytes,(unsigned long long)G.profile_events,(unsigned long long)G.profile_reports,(unsigned long long)G.profile_write_submits,(unsigned long long)G.profile_event_submits,(unsigned long long)G.profile_report_submits,(unsigned long long)G.profile_trace_submits,(unsigned long long)G.profile_cpu_readback_submits,(unsigned long long)G.profile_event_wait_submits);
        fprintf(stderr,"[nvn-vulkan] RENDER_CPU_PROFILE frame=%llu lookup_ms=%.3f pool_ms=%.3f bind_ms=%.3f renderpass_ms=%.3f present_ms=%.3f desc_alloc_ms=%.3f desc_update_ms=%.3f prep_ms=%.3f\n",(unsigned long long)G.present_count+1,G.profile_lookup_ns/1e6,G.profile_pool_ns/1e6,G.profile_bind_ns/1e6,G.profile_render_ns/1e6,G.profile_present_ns/1e6,G.profile_desc_alloc_ns/1e6,G.profile_desc_update_ns/1e6,G.profile_prep_ns/1e6);
        fprintf(stderr,"[nvn-vulkan] RENDER_WRITEBACK_PROFILE frame=%llu enabled=%u raw_ranges=%llu copied_ranges=%llu raw_bytes=%llu copied_bytes=%llu plan_thread_cpu_ms=%.3f\n",(unsigned long long)G.present_count+1,G.enable_writeback_coalesce,(unsigned long long)G.profile_writeback_raw_ranges,(unsigned long long)G.profile_writeback_copy_ranges,(unsigned long long)G.profile_writeback_raw_bytes,(unsigned long long)G.profile_writeback_bytes,G.profile_writeback_plan_cpu_ns/1e6);
        fprintf(stderr,"[nvn-vulkan] RENDER_PSO_PROFILE frame=%llu cached=%u hits=%llu misses=%llu\n",(unsigned long long)G.present_count+1,G.npso,(unsigned long long)G.profile_pso_hits,(unsigned long long)G.profile_pso_misses);
        if(G.profile_frames>=60)fprintf(stderr,"[nvn-vulkan] RENDER_PSO_HASH_PROFILE frame=%llu frames=%llu key_bytes=%zu hash_mode=word4_mix last_cache=%d lookups=%llu hash_calls=%llu hash_ms=%.3f lookup_ms=%.3f last_hits=%llu last_misses=%llu table_hits=%llu misses=%llu table_probes=%llu max_probe=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,sizeof(PsoKey),G.enable_pso_last,(unsigned long long)G.profile_pso_lookups,(unsigned long long)G.profile_pso_hash_calls,G.profile_pso_hash_ns/1e6,G.profile_pso_lookup_ns/1e6,(unsigned long long)G.profile_pso_last_hits,(unsigned long long)G.profile_pso_last_misses,(unsigned long long)G.profile_pso_table_hits,(unsigned long long)G.profile_pso_misses,(unsigned long long)G.profile_pso_probes,(unsigned long long)G.profile_pso_max_probe);
        if(G.profile_frames>=60)fprintf(stderr,"[nvn-vulkan] RENDER_PHASE_CPU_PROFILE frame=%llu frames=%llu clock=thread_cpu writeback_prepare=%d replay_inclusive_ms=%.3f begin_ms=%.3f commit_prepare_ms=%.3f commit_wait_ms=%.3f retire_ms=%.3f writeback_subset_ms=%.3f present_body_inclusive_ms=%.3f replay_calls=%llu begin_calls=%llu commit_calls=%llu writeback_calls=%llu present_calls=%llu wb_global_faults=%llu wb_global_preps=%llu wb_global_mp_calls=%llu wb_global_mp_pages=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,G.enable_writeback_prepare,G.profile_replay_cpu_ns/1e6,G.profile_begin_cpu_ns/1e6,G.profile_commit_prepare_cpu_ns/1e6,G.profile_commit_wait_cpu_ns/1e6,G.profile_retire_cpu_ns/1e6,G.profile_writeback_cpu_ns/1e6,G.profile_present_cpu_ns/1e6,(unsigned long long)G.profile_replay_calls,(unsigned long long)G.profile_begin_calls,(unsigned long long)G.profile_commit_calls,(unsigned long long)G.profile_writeback_calls,(unsigned long long)G.profile_present_calls,(unsigned long long)G.profile_writeback_global_faults,(unsigned long long)G.profile_writeback_global_preps,(unsigned long long)G.profile_writeback_global_mp_calls,(unsigned long long)G.profile_writeback_global_mp_pages);
        if(G.profile_frames>=60){
            unsigned contexts=(G.submission_slot.phase!=SUBMISSION_UNUSED)+(G.submission_slot2.phase!=SUBMISSION_UNUSED);for(int i=1;i<G.render_slots_n;i++)contexts+=G.extra_render_slots[i-1].phase!=SUBMISSION_UNUSED;
            fprintf(stderr,"[nvn-vulkan] RENDER_SLOT_PROFILE frame=%llu slots=%d conflicts=%llu\n",(unsigned long long)G.present_count+1,G.render_slots_n,(unsigned long long)G.slot_conflicts);conflict_ledger_dump();
            if(G.slot_publication)fprintf(stderr,"[nvn-vulkan] SLOT_PUBLICATION_PROFILE frame=%llu overlap_publications=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_overlap_publications);
            fprintf(stderr,"[nvn-vulkan] RENDER_SUBMISSION_PROFILE frame=%llu contexts=%u epoch=%llu creates=%llu reuses=%llu publications=%llu context_struct_bytes=%zu descriptor_arena_bytes=%u descriptor_entry_bytes=%zu async=%d slot_wait_ms=%.3f drain_wait_ms=%.3f completion_fence_wait_ms=%.3f completion_waits=%llu\n",
                (unsigned long long)G.present_count+1,contexts,(unsigned long long)S.epoch,
                (unsigned long long)G.profile_submission_creates,(unsigned long long)G.profile_submission_reuses,
                (unsigned long long)G.profile_submission_publications,sizeof(VkSubmission),(unsigned)DESC_ARENA,
                DESC_ENTRIES*sizeof(DescEntry),G.async_enabled,G.profile_slot_wait_ns/1e6,
                G.profile_drain_wait_ns/1e6,G.profile_completion_wait_ns/1e6,(unsigned long long)G.profile_completion_waits);
        }
        if(G.profile_frames>=60)fprintf(stderr,"[nvn-vulkan] RENDER_COMMAND_PROFILE frame=%llu records=%llu bytes=%llu lease_hits=%llu pool_resolves=%llu epoch_bound=1\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_command_records,(unsigned long long)G.profile_command_bytes,(unsigned long long)G.profile_command_lease_hits,(unsigned long long)G.profile_command_resolves);
        if(G.profile_frames>=60)command_state_profile("RENDER_COMMAND_STATE_PROFILE",G.present_count+1,G.profile_frames);
        dirty_query_profile("RENDER_DIRTY_QUERY_PROFILE",G.present_count+1,G.profile_frames,1);
        memset(G.profile_cmd_issued,0,sizeof G.profile_cmd_issued);memset(G.profile_cmd_skipped,0,sizeof G.profile_cmd_skipped);
        G.profile_command_records=G.profile_command_bytes=G.profile_command_lease_hits=G.profile_command_resolves=0;
        G.profile_submission_creates=G.profile_submission_reuses=G.profile_submission_publications=0;
        G.profile_pso_hits=G.profile_pso_misses=0;
        G.profile_pso_lookups=G.profile_pso_hash_calls=G.profile_pso_hash_ns=G.profile_pso_lookup_ns=0;
        G.profile_pso_last_hits=G.profile_pso_last_misses=G.profile_pso_table_hits=G.profile_pso_probes=G.profile_pso_max_probe=0;
        G.profile_slot_wait_ns=G.profile_drain_wait_ns=G.profile_completion_wait_ns=G.profile_completion_waits=0;
        G.profile_replay_cpu_ns=G.profile_begin_cpu_ns=G.profile_commit_prepare_cpu_ns=G.profile_commit_wait_cpu_ns=G.profile_retire_cpu_ns=G.profile_writeback_cpu_ns=G.profile_present_cpu_ns=0;
        G.profile_replay_calls=G.profile_begin_calls=G.profile_commit_calls=G.profile_writeback_calls=G.profile_present_calls=0;
        G.profile_writeback_global_faults=G.profile_writeback_global_preps=G.profile_writeback_global_mp_calls=G.profile_writeback_global_mp_pages=0;
        fprintf(stderr,"[nvn-vulkan] CONTROL_CACHE_REASON evict=%llu changed=%llu""\n",(unsigned long long)G.profile_control_evict,(unsigned long long)G.profile_control_changed);G.profile_control_evict=G.profile_control_changed=0;
        fprintf(stderr,"[nvn-vulkan] SHADER_STAMP_PROFILE frame=%llu stamp_hits=%llu disabled=%d\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_shader_stamp_hits,G.disable_shader_stamp);G.profile_shader_stamp_hits=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_CACHE_PROFILE frame=%llu desc_hits=%llu desc_misses=%llu control_hits=%llu control_misses=%llu barriers_skipped=%llu sample_copy_hits=%llu sample_copy_misses=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_desc_hits,(unsigned long long)G.profile_desc_misses,(unsigned long long)G.profile_control_hits,(unsigned long long)G.profile_control_misses,(unsigned long long)G.profile_barriers_skipped,(unsigned long long)G.profile_sample_copy_hits,(unsigned long long)G.profile_sample_copy_misses);
        G.profile_desc_hits=G.profile_desc_misses=G.profile_control_hits=G.profile_control_misses=G.profile_barriers_skipped=G.profile_sample_copy_hits=G.profile_sample_copy_misses=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_BINDING_PLAN_PROFILE frame=%llu hits=%llu misses=%llu fallbacks=%llu prepared_buffers=%llu prepared_images=%llu enabled=%u\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_plan_hits,(unsigned long long)G.profile_plan_misses,(unsigned long long)G.profile_plan_fallbacks,(unsigned long long)G.profile_plan_buffers,(unsigned long long)G.profile_plan_images,!G.disable_binding_plans);
        binding_plan_profile(G.present_count+1);
        G.profile_plan_hits=G.profile_plan_misses=G.profile_plan_fallbacks=G.profile_plan_buffers=G.profile_plan_images=0;
        memset(G.profile_plan_miss_reason,0,sizeof G.profile_plan_miss_reason);
        fprintf(stderr,"[nvn-vulkan] RENDER_RESOURCE_CACHE_PROFILE frame=%llu enabled=%d sequence=%llu views_created=%llu framebuffers_created=%llu framebuffer_hits=%llu flushes=%llu kept_submissions=%llu retained_passes=%d retained_views=%d\n",(unsigned long long)G.present_count+1,atomic_load(&resource_applied),(unsigned long long)resource_control_sequence,(unsigned long long)G.resource_views,(unsigned long long)G.resource_passes,(unsigned long long)G.resource_hits,(unsigned long long)G.resource_flushes,(unsigned long long)G.resource_kept,G.submission_slot.npass+G.submission_slot2.npass,G.submission_slot.nview+G.submission_slot2.nview);
        fprintf(stderr,"[nvn-vulkan] RENDER_IMAGE_USAGE_PROFILE frame=%llu declared_policy=%d diagnostic=%d storage_images=%llu omitted_storage=%llu promotions=%llu retained_old_images=%llu\n",(unsigned long long)G.present_count+1,G.image_usage_by_flags,G.diag_img_usage,(unsigned long long)G.image_usage_storage,(unsigned long long)G.image_usage_omitted,(unsigned long long)G.image_promotions,(unsigned long long)G.image_retired_live);
        fprintf(stderr,"[nvn-vulkan] RENDER_VIEW_CACHE_PROFILE frame=%llu view_hits=%llu view_misses=%llu framebuffer_hits=%llu framebuffer_misses=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_view_hits,(unsigned long long)G.profile_view_misses,(unsigned long long)G.profile_framebuffer_hits,(unsigned long long)G.profile_framebuffer_misses);
        G.profile_view_hits=G.profile_view_misses=G.profile_framebuffer_hits=G.profile_framebuffer_misses=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_POOL_UPLOAD_PROFILE frame=%llu uploaded_bytes=%llu reused_ranges=%llu reused_bytes=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_pool_uploaded_bytes,(unsigned long long)G.profile_pool_reused_ranges,(unsigned long long)G.profile_pool_reused_bytes);
        fprintf(stderr,"[nvn-vulkan] RENDER_POOL_OWNERSHIP_PROFILE frame=%llu enabled=%d bypassed_ranges=%llu bypassed_bytes=%llu\n",(unsigned long long)G.present_count+1,G.pool_owned_bypass,(unsigned long long)G.profile_owned_reads,(unsigned long long)G.profile_owned_bytes);
        G.profile_owned_reads=G.profile_owned_bytes=0;
        if(G.pool_range_stats){
            static const uint64_t bounds[]={64,256,1024,4096,16384,65536,262144,1048576,UINT64_MAX};
            for(unsigned i=0;i<9;i++)fprintf(stderr,"[nvn-vulkan] POOL_READ_RANGE_PROFILE frame=%llu max_bytes=%llu reads=%llu hits=%llu protect_calls=%llu read_bytes=%llu protect_bytes=%llu uniforms=%llu\n",
                (unsigned long long)G.present_count+1,(unsigned long long)bounds[i],(unsigned long long)G.pool_ranges[i].reads,(unsigned long long)G.pool_ranges[i].hits,
                (unsigned long long)G.pool_ranges[i].protects,(unsigned long long)G.pool_ranges[i].read_bytes,(unsigned long long)G.pool_ranges[i].protect_bytes,(unsigned long long)G.pool_ranges[i].uniforms);
            memset(G.pool_ranges,0,sizeof G.pool_ranges);
        }
        G.profile_pool_uploaded_bytes=G.profile_pool_reused_ranges=G.profile_pool_reused_bytes=0;
        if(G.pass_timing){for(uint32_t a=0;a<G.npacc;a++)for(uint32_t b=a+1;b<G.npacc;b++)if(G.pacc[b].ms>G.pacc[a].ms){PassAcc tmp=G.pacc[a];G.pacc[a]=G.pacc[b];G.pacc[b]=tmp;}
            double total=0;for(uint32_t a=0;a<G.npacc;a++)total+=G.pacc[a].ms;fprintf(stderr,"[nvn-vulkan] PASS_TIME_TOTAL frame=%llu groups=%u gpu_ms=%.3f\n",(unsigned long long)G.present_count+1,G.npacc,total);
            for(uint32_t a=0;a<G.npacc&&a<12;a++)fprintf(stderr,"[nvn-vulkan] PASS_TIME rank=%u rt_key=%llx size=%ux%u colors=%u passes=%u draws=%llu ms=%.3f\n",a,(unsigned long long)G.pacc[a].key,G.pacc[a].w,G.pacc[a].h,G.pacc[a].n,G.pacc[a].passes,(unsigned long long)G.pacc[a].draws,G.pacc[a].ms);
            if(G.pass_stats){
                for(uint32_t a=0;a<G.npacc;a++)fprintf(stderr,"[nvn-vulkan] PASS_STATS frame=%llu frames=%llu rt_key=%llx size=%ux%u colors=%u passes=%u draws=%llu ms=%.3f ia_vertices=%llu ia_primitives=%llu vs_invocations=%llu fs_invocations=%llu\n",
                    (unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,(unsigned long long)G.pacc[a].key,
                    G.pacc[a].w,G.pacc[a].h,G.pacc[a].n,G.pacc[a].passes,(unsigned long long)G.pacc[a].draws,G.pacc[a].ms,
                    (unsigned long long)G.pacc[a].stats[0],(unsigned long long)G.pacc[a].stats[1],(unsigned long long)G.pacc[a].stats[2],(unsigned long long)G.pacc[a].stats[3]);
                fprintf(stderr,"[nvn-vulkan] PASS_STATS_COVERAGE frame=%llu omitted_passes=%llu groups=%u group_limit=%d\n",
                    (unsigned long long)G.present_count+1,(unsigned long long)G.pass_stats_window_omitted,G.npacc,G.npacc==128);
                G.pass_stats_window_omitted=0;
            }
            for(uint32_t a=0;a<G.nhot;a++)for(uint32_t b=a+1;b<G.nhot;b++)if(G.hot[b].draws>G.hot[a].draws){char tk[17];uint32_t td=G.hot[a].draws;memcpy(tk,G.hot[a].key,17);memcpy(G.hot[a].key,G.hot[b].key,17);G.hot[a].draws=G.hot[b].draws;memcpy(G.hot[b].key,tk,17);G.hot[b].draws=td;}
            for(uint32_t a=0;a<G.nhot&&a<12;a++)fprintf(stderr,"[nvn-vulkan] HOT_PS rank=%u key=%s draws=%u\n",a,G.hot[a].key,G.hot[a].draws);
            G.nhot=0;G.npacc=0;}
        draw_timing_report(G.present_count+1,G.profile_frames);if(G.desc_diag)desc_miss_report();
        fprintf(stderr,"[nvn-vulkan] RENDER_RESOURCE_PROFILE frame=%llu driver_tables=%llu driver_table_reuses=%llu buffer_allocs=%llu buffer_bytes=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_driver_tables,(unsigned long long)G.profile_driver_table_reuses,(unsigned long long)G.profile_buffer_allocs,(unsigned long long)G.profile_buffer_bytes);
        G.profile_driver_tables=G.profile_driver_table_reuses=G.profile_buffer_allocs=G.profile_buffer_bytes=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_TEMP_PROFILE frame=%llu reuses=%llu cached=%u cached_bytes=%llu clear_ms=%.3f allocate_ms=%.3f aligned_bytes=%llu comparison_bytes=%llu driver_bytes=%llu other_bytes=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_temp_reuses,G.nretired,(unsigned long long)G.retired_bytes,G.profile_temp_clear_ns/1e6,G.profile_buffer_allocate_ns/1e6,(unsigned long long)G.profile_temp_bytes[TEMP_ALIGNED],(unsigned long long)G.profile_temp_bytes[TEMP_COMPARISON],(unsigned long long)G.profile_temp_bytes[TEMP_DRIVER],(unsigned long long)G.profile_temp_bytes[TEMP_OTHER]);
        G.profile_temp_reuses=G.profile_temp_clear_ns=G.profile_buffer_allocate_ns=0;memset(G.profile_temp_bytes,0,sizeof G.profile_temp_bytes);
        fprintf(stderr,"[nvn-vulkan] RENDER_STORAGE_BIAS_PROFILE frame=%llu bindings=%llu avoided_snapshot_bytes=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_storage_bias_bindings,(unsigned long long)G.profile_storage_bias_bytes);
        G.profile_storage_bias_bindings=G.profile_storage_bias_bytes=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_QUERY_PROFILE frame=%llu queries=%llu skipped_ui=%llu skipped_shadow=%llu mask=%u\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_occlusion_queries,
            (unsigned long long)G.profile_occlusion_skipped_ui,(unsigned long long)G.profile_occlusion_skipped_shadow,G.query_skip_mask);
        G.profile_occlusion_queries=0;
        G.profile_occlusion_skipped_ui=G.profile_occlusion_skipped_shadow=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_QUERY_RESET_PROFILE frame=%llu frames=%llu enabled=%d calls=%llu slots=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,G.enable_query_reset_range,(unsigned long long)G.profile_query_reset_calls,(unsigned long long)G.profile_query_reset_slots);
        G.profile_query_reset_calls=G.profile_query_reset_slots=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_SYNC_PROFILE frame=%llu passes=%llu all_barriers=%llu uniform_updates=%llu aligned_cpu_snapshots=%llu aligned_gpu_copies=%llu aligned_snapshot_reuses=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)(G.pass_count-G.profile_passes),(unsigned long long)G.profile_barriers,(unsigned long long)G.profile_uniform_updates,(unsigned long long)G.profile_cpu_snapshots,(unsigned long long)G.profile_gpu_copies,(unsigned long long)G.profile_snapshot_reuses);
        fprintf(stderr,"[nvn-vulkan] PRESENT_PARTS acquire_ms=%.1f fence_ms=%.1f idle_ms=%.1f\n",G.pres_acq_ns/1e6,G.pres_fence_ns/1e6,G.pres_idle_ns/1e6);G.pres_acq_ns=G.pres_fence_ns=G.pres_idle_ns=0;
        for(int q=0;q<4;q++){fprintf(stderr,"[nvn-vulkan] SEG%d record_ms=%.1f wait_ms=%.1f last_chunk_draws=%llu chunks=%llu gpu_busy_ms=%.1f gpu_gap_ms=%.1f gpu_span_ms=%.1f\n",q,G.seg_record_ns[q]/1e6,G.seg_wait_ns[q]/1e6,(unsigned long long)G.seg_draws[q],(unsigned long long)G.seg_chunks[q],G.gts_busy[q]/1e6,G.gts_gap[q]/1e6,G.gts_span[q]/1e6);G.gts_busy[q]=G.gts_gap[q]=G.gts_span[q]=G.gts_chunks[q]=0;G.seg_record_ns[q]=G.seg_wait_ns[q]=G.seg_draws[q]=G.seg_chunks[q]=0;}
        if(G.precise_sync||G.barrier_stats)fprintf(stderr,"[nvn-vulkan] HZ_SYNC barriers=%llu chunks=%llu wait_elided=%llu\n",(unsigned long long)G.profile_hz_barriers,(unsigned long long)G.profile_chunks,(unsigned long long)G.profile_wait_elided);G.profile_chunks=0;G.profile_wait_elided=0;fprintf(stderr,"[nvn-vulkan] HZ_KINDS pass_frag=%llu pass_wide=%llu xfer=%llu cs=%llu tr_shaderread=%llu tr_attach=%llu tr_transfer=%llu tr_other=%llu\n",(unsigned long long)G.hz_count[0],(unsigned long long)G.hz_count[1],(unsigned long long)G.hz_count[2],(unsigned long long)G.hz_count[3],(unsigned long long)G.hz_count[4],(unsigned long long)G.hz_count[5],(unsigned long long)G.hz_count[6],(unsigned long long)G.hz_count[7]);memset(G.hz_count,0,sizeof G.hz_count);G.profile_hz_barriers=0;
        if(G.barrier_stats){fprintf(stderr,"[nvn-vulkan] BARRIER_BITS nopass=%u sidefx=%u",G.barrier_nopass,G.barrier_sidefx);G.barrier_nopass=G.barrier_sidefx=0;for(int i=0;i<256;i++)if(G.barrier_bits[i]){fprintf(stderr," %#x=%u(kept %u)",i,G.barrier_bits[i],G.barrier_kept[i]);G.barrier_bits[i]=G.barrier_kept[i]=0;}fprintf(stderr,"\n");fprintf(stderr,"[nvn-vulkan] BARRIER_SITES");for(int i=0;i<4096;i++)if(G.barrier_sites[i])fprintf(stderr," L%d=%u",i,G.barrier_sites[i]);fprintf(stderr,"\n");}
        if(G.barrier_stats){fprintf(stderr,"[nvn-vulkan] SETRT total=%llu same=%llu\n",(unsigned long long)G.setrt_total,(unsigned long long)G.setrt_same);G.setrt_total=G.setrt_same=0;}
        if(G.barrier_stats){fprintf(stderr,"[nvn-vulkan] PASS_END_SITES");for(int i=0;i<4096;i++)if(G.pass_end_sites[i])fprintf(stderr," L%d=%u",i,G.pass_end_sites[i]);fprintf(stderr,"\n");}
        memset(G.pass_end_sites,0,sizeof G.pass_end_sites);
        memset(G.barrier_sites,0,sizeof G.barrier_sites);
        G.profile_passes=G.pass_count;G.profile_barriers=G.profile_uniform_updates=G.profile_cpu_snapshots=G.profile_gpu_copies=G.profile_snapshot_reuses=0;
        G.profile_lookup_ns=G.profile_pool_ns=G.profile_bind_ns=G.profile_render_ns=G.profile_present_ns=G.profile_desc_alloc_ns=G.profile_desc_update_ns=G.profile_prep_ns=0;
        G.profile_submit_candidates=G.profile_candidate_wait_ns=G.profile_writeback_bytes=G.profile_events=G.profile_reports=0;
        G.profile_writeback_raw_bytes=G.profile_writeback_raw_ranges=G.profile_writeback_copy_ranges=G.profile_writeback_plan_cpu_ns=0;
        G.profile_write_submits=G.profile_event_submits=G.profile_report_submits=G.profile_trace_submits=G.profile_cpu_readback_submits=G.profile_event_wait_submits=0;
        G.profile_start_ns=now;G.profile_pipeline_ns=G.profile_wait_ns=G.profile_decode_ns=G.profile_submits=G.profile_frames=0;G.profile_draws=G.draws;}
    G.trace_frame=G.trace_ui=0;
    if(capture_root){char request[1200];snprintf(request,sizeof request,"%s/trace-ui-request",capture_root);
        if(!access(request,F_OK)){if(unlink(request))die("UI trace request consume",0);G.trace_frame=G.trace_ui=1;G.trace_meshes=G.ntrace_textures=0;
            fprintf(stderr,"[nvn-vulkan] TRACE_NEXT_FRAME frame=%llu ui=1\n",(unsigned long long)G.present_count+2);}
        snprintf(request,sizeof request,"%s/trace-request",capture_root);
        if(!access(request,F_OK)){if(unlink(request))die("trace request consume",0);G.trace_frame=1;G.trace_meshes=G.ntrace_textures=0;
            G.trace_shader[0]=0;char selector[1200];snprintf(selector,sizeof selector,"%s/texture-trace-shader",capture_root);FILE *selection=fopen(selector,"rb");
            if(selection){char key[18]={0};size_t bytes=fread(key,1,sizeof(key),selection);fclose(selection);
                if((bytes!=16 && !(bytes==17&&key[16]=='\n')) || strspn(key,"0123456789abcdef")!=16)die("texture trace shader selector",bytes);
                memcpy(G.trace_shader,key,16);G.trace_shader[16]=0;
            }
            fprintf(stderr,"[nvn-vulkan] TRACE_NEXT_FRAME frame=%llu\n",(unsigned long long)G.present_count+2);}}
    pipeline_cache_checkpoint(0);
    pipeline_frame(G.present_count+1);attachment_ab_frame(G.present_count+1);
    atomic_store(&G.recording_active,0);
    if(async_present){G.active=0;async_enqueue_locked(&S,1);uint64_t pticket=++G.present_count;pthread_mutex_unlock(&G.lock);return pticket;}
    if(S.nwrites || S.nevents || S.ncounter)die("present has unpublished guest effects",S.epoch);
    submission_publish(&S);submission_released(&S);
    uint64_t ticket=++G.present_count;pthread_mutex_unlock(&G.lock);return ticket;
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
    mtl_sub_begin();S.submission_cpu_readback=1;VTexture *t=texture_get(source,1);unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
    if(region->xoffset<0||region->yoffset<0||region->width<0||region->height<0||
      (uint64_t)region->xoffset+region->width>width||(uint64_t)region->yoffset+region->height>height)die("CPU texture read bounds",region->width);
    size_t pitch=(size_t)region->width*t->bpp;if(row<pitch || (img && img<row*(size_t)region->height))die("CPU texture read stride",row);
    int packed_stencil=(aspect(t->format)&VK_IMAGE_ASPECT_STENCIL_BIT)!=0;
    uint64_t texels=(uint64_t)region->width*region->height,stencil_offset=texels*4;
    VBuffer readback=buffer_create_kind(packed_stencil?(stencil_offset+texels+3)&~3ull:pitch*region->height,VK_BUFFER_USAGE_TRANSFER_DST_BIT,1,DM_VK_MEMORY_READBACK);
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy={.imageSubresource={packed_stencil?VK_IMAGE_ASPECT_DEPTH_BIT:aspect(t->format),level,layer,1},
        .imageOffset={region->xoffset,region->yoffset,region->zoffset},.imageExtent={region->width,region->height,1}};
    VkBufferImageCopy copies[2]={copy,copy};unsigned count=1;
    if(packed_stencil){copies[1].bufferOffset=stencil_offset;copies[1].imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;count=2;}
    vkCmdCopyImageToBuffer(S.command,t->image,t->layout,readback.b,count,copies);mtl_sub_commit();
    for(int y=0;y<region->height;y++){
        if(!packed_stencil){memcpy(dst+y*row,readback.map+y*pitch,pitch);continue;}
        /* Vulkan transfers one tightly packed aspect at a time. Restore NVN's
         * D24S8 or D32FS8 texel layout without exposing undefined padding. */
        for(int x=0;x<region->width;x++){
            size_t pixel=(size_t)y*region->width+x;uint32_t z;memcpy(&z,readback.map+pixel*4,4);
            uint8_t stencil=readback.map[stencil_offset+pixel];uint8_t *out=dst+y*row+x*t->bpp;
            if(t->format==VK_FORMAT_D24_UNORM_S8_UINT){z=(z&0xffffffu)|((uint32_t)stencil<<24);memcpy(out,&z,4);}
            else{memcpy(out,&z,4);out[4]=stencil;memset(out+5,0,3);}
        }
    }
    buffer_free(&readback);
}
void dm_vk_write_texels(const tex_t *source,const view_t *view,const NVNcopyRegion *r,const void *data,ptrdiff_t row,ptrdiff_t img) {
    if(!source || !r || !data)die("CPU texture write arguments",0);
    unsigned level=view&&view->set_levels?view->base_level:0,layer=view&&view->set_layers?view->min_layer:0;
    int bw=0,bh=0,bytes=nvn_fmt_bpb(source->d.format,&bw,&bh);
    int volume=source->d.target==NVN_TEXTURE_TARGET_3D;
    if((source->d.target!=NVN_TEXTURE_TARGET_2D&&!volume) || bw!=1 || bh!=1 || !bytes ||
       source->d.samples>1 || (view&&view->set_format&&view->format!=source->d.format) ||
       (view&&view->set_target&&view->target!=source->d.target) ||
       (source->d.format>=NVN_FORMAT_DEPTH16&&source->d.format<=NVN_FORMAT_DEPTH32F_STENCIL8))
        {fprintf(stderr,"[nvn-vulkan] CPU_TEXTURE_WRITE_UNSUPPORTED target=%d format=%d samples=%d block=%dx%d extent=%ux%ux%u region=%d,%d,%d,%d,%d,%d\n",source->d.target,source->d.format,source->d.samples,bw,bh,source->d.width,source->d.height,source->d.depth,r->xoffset,r->yoffset,r->zoffset,r->width,r->height,r->depth);die("CPU texture write target/format requires adaptation",source->d.format);}
    if(level>=source->d.levels || layer>=source->L.layers || r->xoffset<0 || r->yoffset<0 ||
       r->zoffset<0 || r->depth<0 || (!volume&&(r->zoffset||r->depth!=1)) || r->width<0 || r->height<0)die("CPU texture write region",level);
    uint32_t width=source->d.width>>level,height=source->d.height>>level;if(!width)width=1;if(!height)height=1;
    uint32_t depth=volume?source->d.depth>>level:1;if(!depth)depth=1;
    if((uint64_t)r->xoffset+r->width>width || (uint64_t)r->yoffset+r->height>height || (uint64_t)r->zoffset+r->depth>depth)die("CPU texture write bounds",r->width);
    if(!r->width || !r->height || !r->depth)return;
    size_t pitch=(size_t)r->width*bytes;if(!row)row=pitch;
    if(row<0 || (size_t)row<pitch || img<0 || (img&&(uint64_t)img<(uint64_t)row*r->height))die("CPU texture write stride",row);
    if(!img)img=row*r->height;
    uint32_t full_row,rows,slices,bpb;uint64_t base;unsigned th,td;
    if(nvn_deswizzle_geom(&source->d,level,layer,&full_row,&rows,&slices,&base,&th,&td,&bpb))die("CPU texture write layout",source->serial);
    mtl_sub_begin();
    if(!pool_find((uint64_t)(uintptr_t)source->mem,source->L.size,NULL))die("CPU texture write storage",source->serial);
    /* Update only the addressed block-linear bytes. GPU-owned pixels outside
     * the region are retained in the image and never re-uploaded from RAM. */
    for(int z=0;z<r->depth;z++)for(int y=0;y<r->height;y++)for(size_t x=0;x<pitch;x++) {
        uint64_t offset=base+nvn_bl_offset((uint32_t)((size_t)r->xoffset*bytes+x),r->yoffset+y,r->zoffset+z,full_row,rows,th,td);
        if(offset>=source->L.size)die("CPU texture write storage bounds",offset);
        source->mem[offset]=((const uint8_t *)data)[(size_t)z*img+(size_t)y*row+x];
    }
    VTexture *t=texture_get(source,1);VBuffer *staging=temporary(pitch*r->height*r->depth,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for(int z=0;z<r->depth;z++)for(int y=0;y<r->height;y++)memcpy(staging->map+((size_t)z*r->height+y)*pitch,(const uint8_t *)data+(size_t)z*img+(size_t)y*row,pitch);
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1},
        .imageOffset={r->xoffset,r->yoffset,r->zoffset},.imageExtent={r->width,r->height,r->depth}};
    vkCmdCopyBufferToImage(S.command,staging->b,t->image,t->layout,1,&copy);tex_dirty(t);
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] CPU_TEXTURE_WRITE serial=%llu format=%d mip=%u region=%d,%d,%d,%d\n",
        (unsigned long long)source->serial,source->d.format,level,r->xoffset,r->yoffset,r->width,r->height);
    mtl_sub_commit();
}
#ifdef DM_VK_COPY_TEST
/* Separate aspect copies: a combined depth/stencil mask is not legal in a
 * VkBufferImageCopy. This probe checks actual GPU pixels after ZCull restore. */
int dm_vk_copy_test_depth(const tex_t *source,float expected_depth,unsigned expected_stencil) {
    if(G.active)die("depth probe during submission",0);
    mtl_sub_begin();S.submission_cpu_readback=1;VTexture *t=texture_get(source,1);
    if(t->format!=VK_FORMAT_D24_UNORM_S8_UINT)die("depth probe format",t->format);
    uint64_t pixels=(uint64_t)t->width*t->height;
    VBuffer readback=buffer_create_kind(pixels*5,VK_BUFFER_USAGE_TRANSFER_DST_BIT,1,DM_VK_MEMORY_READBACK);
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c[2]={
        {.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.imageExtent={t->width,t->height,1}},
        {.bufferOffset=pixels*4,.imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1},.imageExtent={t->width,t->height,1}}};
    vkCmdCopyImageToBuffer(S.command,t->image,t->layout,readback.b,2,c);mtl_sub_commit();
    uint32_t expected=(uint32_t)llround((double)expected_depth*16777215.0);int ok=1;
    for(uint64_t i=0;i<pixels;i++){
        uint32_t depth;memcpy(&depth,readback.map+i*4,4);depth&=0xffffff;
        if((depth>expected?depth-expected:expected-depth)>1 || readback.map[pixels*4+i]!=expected_stencil)ok=0;
    }
    buffer_free(&readback);return ok;
}
void dm_vk_copy_test_pool_bytes(uint64_t *logical,uint64_t *resident) {
    *logical=*resident=0;for(int i=0;i<G.npool;i++){*logical+=G.pools[i].size;*resident+=G.pools[i].v.size;}
}
/* Independent device probe only: inspect expanded BC pixels without promising
 * compressed guest-format CPU readback or introducing a lossy BC encoder. */
void dm_vk_copy_test_read(const tex_t *source,unsigned level,unsigned layer,uint8_t *dst) {
    if(G.active || level>=source->d.levels || layer>=source->L.layers)die("copy probe subresource",level);
    mtl_sub_begin();S.submission_cpu_readback=1;VTexture *t=texture_get(source,1);
    unsigned w=t->width>>level,h=t->height>>level;if(!w)w=1;if(!h)h=1;
    uint64_t row=t->bc_block?((uint64_t)w+3)/4*t->bc_block:(uint64_t)w*t->bpp;
    uint64_t bytes=t->bc_block?row*((h+3)/4):row*h;
    VBuffer readback=buffer_create_kind(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,1,DM_VK_MEMORY_READBACK);
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c={.imageSubresource={aspect(t->format),level,layer,1},.imageExtent={w,h,1}};
    vkCmdCopyImageToBuffer(S.command,t->image,t->layout,readback.b,1,&c);mtl_sub_commit();
    if(t->bc_block){if(nvn_bc_decode(source->d.format,readback.map,row,w,h,dst,(size_t)w*t->bpp))die("native BC probe decode",source->d.format);}
    else memcpy(dst,readback.map,(size_t)w*h*t->bpp);
    buffer_free(&readback);
}
#endif
#include "nvn_vk_memory.inc"
void mtl_stats(uint64_t *passes,uint64_t *draws) { *passes=G.pass_count;*draws=G.draws; }
void mtl_pkg_stats(uint64_t *uploads,uint64_t *reuploads) { *uploads=G.uploads;*reuploads=0; }
void mtl_set_frame(uint64_t frame) { G.frame=frame; }
double mtl_gpu_ms(uint64_t frame) { (void)frame;return -1; }
int mtl_gpu_span(uint64_t frame,uint64_t *a,uint64_t *b) { (void)frame;(void)a;(void)b;return 0; }
int mtl_link_active(void) { return 0; } /* No Android frame-link service has been installed. */
int mtl_link_wait_tick(uint64_t timeout) { (void)timeout;return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; } /* No cross-frame store-elision ledger exists. */
void mtl_ub_present(double ms) { (void)ms; } /* Disabled diagnostic experiment. */
uint64_t mtl_upload_bytes(void) {
    uint64_t n=0;
    for(uint32_t i=0;i<G.texture_used;i++){
        VTexture *t=&G.textures[i];if(!t->serial||!t->cpu_owned)continue;
        n+=t->bc_block?((uint64_t)t->width+3)/4*((t->height+3)/4)*t->bc_block:(uint64_t)t->width*t->height*t->bpp;
    }
    return n;
}

#else
#ifdef DM_EXPECT_VULKAN_BACKEND
#error "Android Vulkan target requires the NDK __ANDROID__ platform macro"
#endif
typedef int nvn_vk_is_android_only;
#endif
