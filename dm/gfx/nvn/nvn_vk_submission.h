/* Private replay submission ownership, included after the backend's types.
 * Device resources and guest queue state outlive this context. Every object
 * below belongs to one recorded submission and is reused only after its real
 * GPU fence and ordered CPU publication. The first scheduler has one slot;
 * asynchronous issue still requires immutable pool inputs/output versions.
 */
#ifndef NVN_VK_SUBMISSION_H
#define NVN_VK_SUBMISSION_H

/* One GPU timestamp entry of the per-draw timing diagnostic. kind 0 = pass start, 1 = draw. */
#define DT_CAP 4096
typedef struct {
    char vs[13],ps[13];uint8_t kind,n,depth,blend,dt,dw;uint16_t pass;
    uint32_t w,h,count,instances,vpw,vph;
} DrawTime;

typedef enum {
    SUBMISSION_UNUSED, SUBMISSION_RECORDING, SUBMISSION_INFLIGHT,
    SUBMISSION_GPU_DONE, SUBMISSION_PUBLISHED, SUBMISSION_RETIRED
} SubmissionPhase;

typedef struct {
    SubmissionPhase phase;
    int slot_index; /* Render slot (0..3); -1 for the present slot. */
    uint64_t epoch,ticket;
    pthread_t recorder;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkCommandBuffer command_primary,chunk_cb[48];
    uint32_t chunk_alloc,chunk_index;
    uint32_t chunk_draws; /* Latched at begin; immutable through final fence. */
    uint64_t chunk_mark;
    unsigned profile_segment; /* Captured at submit, before asynchronous completion. */
    VkDescriptorPool descriptors;
    /* Descriptor sets are recycled by layout between uses of this submission slot instead of being freed by a pool
     * reset and allocated again for every draw. extra_pools holds overflow pools created when the main pool ran dry. */
    struct DescSetBucket {VkDescriptorSetLayout layout;VkDescriptorSet *sets;uint32_t count,used,cap;} *set_buckets;
    uint32_t nset_buckets,set_buckets_cap,sets_total;
    VkDescriptorPool extra_pools[4];uint32_t nextra_pools,alloc_pool; /* alloc_pool: 0 = main pool, i+1 = extra_pools[i] */
    VkFence gpu_fence;
    VkQueryPool occlusion,timestamps,pass_ts,chunk_ts,pass_stats;
    uint32_t nchunk_ts;
    uint64_t chunk_pass0[130],chunk_draw0[130];
    uint32_t nocc,ntime,active_occlusion;
    uint32_t occlusion_reset_count,timestamp_reset_count,pass_reset_count,pass_stats_reset_count;
    int command_dirty;
    VkSemaphore present_available;
    struct { unsigned reports;int candidate,traced;uint64_t wait_cpu,wait_start; } commit_info;
    VBuffer wb_stage;
    VBuffer depth_input_guard;
    unsigned depth_input_guard_active,depth_input,pass_depth_input;
    uint64_t wb_stage_total;
    int wb_staged;
    struct { uint32_t stages,access; } pend[4];
    NvnVkCommandState command_state;
    uint64_t null_bindings[6];

    Range writes[LIMIT_WRITES];
    const uint8_t *write_cpu[LIMIT_WRITES];
    int nwrites;
    NvnVkWritebackSpan writeback_plan[LIMIT_WRITES];
    Event events[LIMIT_WRITES];
    int nevents;
    CounterOp counter_ops[LIMIT_WRITES];
    int ncounter;
    int submission_cpu_readback,submission_event_wait;

    VBuffer **temporaries;
    size_t temporary_capacity;
    int ntemporary;
    AlignedBinding aligned[2*LIMIT_BINDINGS];
    int naligned;
    ReadSnapshot read_snapshots[LIMIT_WRITES];
    int nread_snapshots,read_snapshots_allowed;
    DriverTable driver_tables[LIMIT_WRITES];
    uint32_t driver_table_slots[DRIVER_TABLE_SLOTS],ndriver_tables;
    VBuffer *last_driver_table[6][2];
    uint64_t driver_table_revision[6][2];
    ImageUse image_uses[2*LIMIT_BINDINGS];
    unsigned nimage_uses;
    uint32_t storage_bias[6][LIMIT_BINDINGS];
    uint32_t dyn_offset[6][LIMIT_BINDINGS];
    uint32_t dyn_count[6];
    Range direct_reads[LIMIT_WRITES];
    int direct_read_write_count[LIMIT_WRITES],ndirect_reads;

    /* Optional slot-local immutable resources survive fence completion. They
     * are invalidated before any source image is destroyed, never while live. */
    int retain_resources,retained_views,retained_passes;
    VkImageView temporary_views[LIMIT_WRITES+RETAINED_VIEW_LIMIT];
    int nview;
    ImageViewKey view_keys[LIMIT_WRITES+RETAINED_VIEW_LIMIT];
    uint32_t view_table[VIEW_TABLE_SIZE];
    VkBufferView buffer_views[LIMIT_WRITES];
    int nbuffer_view;
    VkPipeline pipelines[LIMIT_WRITES];
    VkPipelineLayout layouts[LIMIT_WRITES];
    int npipeline;
    PassKey pass_key;
    VkRenderPass passes[LIMIT_WRITES+RETAINED_PASS_LIMIT];
    uint8_t pass_owned[LIMIT_WRITES+RETAINED_PASS_LIMIT];
    VkFramebuffer framebuffers[LIMIT_WRITES+RETAINED_PASS_LIMIT];
    int npass;
    FramebufferKey framebuffer_keys[LIMIT_WRITES+RETAINED_PASS_LIMIT];
    int active_pass,in_pass,depth_sampled,pass_depth_read_only;
    unsigned pending_clear,pass_discard;
    int flushing_clear;
    VkClearValue clear_values[9]; /* NVN color slots 0..7, depth/stencil slot 8. */
    VkPipelineCreateFlags pass_feedback_flags,feedback_flags;

    DescEntry *desc_entries;
    uint8_t *desc_arena;
    uint32_t desc_table[DESC_TABLE],ndesc;
    size_t desc_arena_used;
    uint32_t npt,pt_open;
    uint64_t pt_draws0;
    struct {uint32_t q0,q1,w,h,n;uint64_t key,draws;} pt[512];
    /* Per-draw GPU timing (DM_DRAW_TIMING, diagnostic only): one timestamp per draw plus one per pass start. */
    VkQueryPool draw_ts;uint32_t ndt,dt_pass;
    DrawTime *dt;
    TraceCapture trace_captures[512];
    unsigned ntrace_captures;
    /* Exactly one readback per probed pass, before any following pass writes. */
    struct {
        VTexture *texture;VBuffer *buffer;
        uint64_t key,pass,serial;
        uint32_t w,h,format,slot,mip,layer,sentinel;
        VkImageLayout layout;
    } coverage[16];
    unsigned ncoverage;
    uint64_t coverage_bytes;
} VkSubmission;

#endif
