/* Independent device fixture. Include the production backend to exercise its
 * real private GPU cache paths without shipping test entry points in the APK. */
#include "hle.h"
#undef die
#include "../../gfx/nvn/nvn_vk.c"
struct dm_flags g_flag;uint64_t g_t0_ns;
void *dm_android_native_window(void){return NULL;}
static unsigned checks;
#define REQUIRE(x) do{checks++;if(!(x)){fprintf(stderr,"MEMORY_TRIM_FAIL line=%d expression=%s\n",__LINE__,#x);abort();}}while(0)
static DmVkMemoryStats stats(void){DmVkMemoryStats s;REQUIRE(!dm_vk_memory_get_stats(&s));uint64_t sum=0;for(unsigned i=0;i<DM_VK_MEMORY_CLASS_COUNT;i++){sum+=s.bytes[i];REQUIRE(s.peak_bytes[i]>=s.bytes[i]);}REQUIRE(sum==s.total_bytes&&s.peak_total_bytes>=sum);return s;}
static tex_t make_texture(uint8_t *memory,unsigned format,unsigned side,uint64_t serial){
    tex_t t={.d={.target=NVN_TEXTURE_TARGET_2D,.format=format,.width=side,.height=side,.depth=1,.levels=1},.mem=memory,.serial=serial};nvn_tex_layout(&t.d,&t.L);return t;
}
static void prepare_derived(tex_t *source){
    VTexture *t=texture_get(source,0);comparison_image(t);
    VShader sampled={.h={.stage=1,.nb=1},.b={{.kind=4,.slot=0}}};
    G.state.tex[1][0]=1;G.state.rt=(p_set_rt){.n=1,.color={source}};prepare_sampling_copies(&sampled);
    REQUIRE(t->sample_copy&&t->shadow&&t->sc_valid&&t->shadow_version==t->version);
}
/* Sample both actual derived images on the GPU. Their production usage omits
 * TRANSFER_SRC, so a direct readback copy would be invalid Vulkan. */
static void verify_derived_pixels(VTexture *t,const char *shader_path){
    FILE *f=fopen(shader_path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long length=ftell(f);REQUIRE(length>0&&!(length&3));rewind(f);
    uint32_t *code=malloc(length);REQUIRE(code&&fread(code,1,length,f)==(size_t)length);fclose(f);
    VkShaderModule module;VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)length,.pCode=code};CHECK(vkCreateShaderModule(G.device,&mi,NULL,&module));free(code);
    VkDescriptorSetLayoutBinding bindings[3]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=2,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayout set_layout;VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=3,.pBindings=bindings};CHECK(vkCreateDescriptorSetLayout(G.device,&li,NULL,&set_layout));
    VkPipelineLayout layout;VkPipelineLayoutCreateInfo pi={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};CHECK(vkCreatePipelineLayout(G.device,&pi,NULL,&layout));
    VkPipeline pipeline;VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=layout};CHECK(vkCreateComputePipelines(G.device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPool descriptors;VkDescriptorPoolCreateInfo di={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};CHECK(vkCreateDescriptorPool(G.device,&di,NULL,&descriptors));
    VkDescriptorSet set;VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&set_layout};CHECK(vkAllocateDescriptorSets(G.device,&ai,&set));
    VkSampler sampler;VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};CHECK(vkCreateSampler(G.device,&si,NULL,&sampler));
    VBuffer readback=buffer_create_kind(128*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,1,DM_VK_MEMORY_READBACK);
    DmVkMemoryStats allocated=stats();REQUIRE(allocated.bytes[DM_VK_MEMORY_READBACK]==readback.allocation_size);
    VkDescriptorImageInfo images[2]={{sampler,t->sample_view,t->sample_layout},{sampler,t->shadow_view,t->shadow_layout}};
    VkDescriptorBufferInfo buffer={readback.b,0,128*sizeof(float)};
    VkWriteDescriptorSet writes[3]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[0]},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[1]},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=2,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer}};
    vkUpdateDescriptorSets(G.device,3,writes,0,NULL);mtl_sub_begin();vkCmdBindPipeline(S.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(S.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(S.command,1,1,1);
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);mtl_sub_commit();
    for(unsigned i=0;i<128;i++){float value;memcpy(&value,readback.map+i*4,4);REQUIRE(value==.625f);}
    vkDestroyDescriptorPool(G.device,descriptors,NULL);vkDestroySampler(G.device,sampler,NULL);vkDestroyPipeline(G.device,pipeline,NULL);vkDestroyPipelineLayout(G.device,layout,NULL);vkDestroyDescriptorSetLayout(G.device,set_layout,NULL);vkDestroyShaderModule(G.device,module,NULL);buffer_free(&readback);
}
static void verify_inflight_trim(void){
    REQUIRE(mtl_async_enabled());
    PFN_vkCreateEvent create=(PFN_vkCreateEvent)dm_vk_get(G.instance,"vkCreateEvent");
    PFN_vkCmdWaitEvents wait=(PFN_vkCmdWaitEvents)dm_vk_get(G.instance,"vkCmdWaitEvents");
    PFN_vkSetEvent signal=(PFN_vkSetEvent)dm_vk_get(G.instance,"vkSetEvent");
    PFN_vkDestroyEvent destroy=(PFN_vkDestroyEvent)dm_vk_get(G.instance,"vkDestroyEvent");REQUIRE(create&&wait&&signal&&destroy);
    VkEvent event;VkEventCreateInfo info={.sType=VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};CHECK(create(G.device,&info,NULL,&event));
    mtl_sub_begin();VBuffer *held=temporary(8192,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=held->b,.size=VK_WHOLE_SIZE};
    wait(S.command,1,&event,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,NULL,1,&barrier,0,NULL);mtl_sub_commit_async();
    DmVkMemoryStats out;memset(&out,0xa7,sizeof out);DmVkMemoryStats original=out;
    REQUIRE(dm_vk_memory_trim_caches(DM_VK_TRIM_ALL,&out)==EBUSY);REQUIRE(!memcmp(&out,&original,sizeof out));
    int result;uint64_t deadline=dm_android_nvn_clock_ns()+1000000000;
    while((result=dm_vk_memory_get_stats(&out))==EBUSY&&dm_android_nvn_clock_ns()<deadline){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    REQUIRE(!result&&out.temp_inflight_bytes>=8192);
    CHECK(signal(G.device,event));mtl_finish();destroy(G.device,event,NULL);REQUIRE(!stats().temp_inflight_bytes);
}
int main(int argc,char **argv){
    DmVkMemoryStats zero=stats();REQUIRE(!G.device&&zero.total_bytes==0);
    DmVkMemoryStats untouched;memset(&untouched,0xa7,sizeof untouched);DmVkMemoryStats original=untouched;
    REQUIRE(dm_vk_memory_get_stats(NULL)==EINVAL);REQUIRE(dm_vk_memory_trim_caches(128,&untouched)==EINVAL);REQUIRE(!memcmp(&untouched,&original,sizeof original));
    mtl_init();g_t0_ns=dm_android_nvn_clock_ns();DmVkMemoryStats base=stats();
    REQUIRE(base.bytes[DM_VK_MEMORY_SUPPORT]==G.support.allocation_size+G.zero.allocation_size);
    pthread_mutex_lock(&G.lock);uint64_t now=dm_android_nvn_clock_ns();REQUIRE(dm_vk_memory_get_stats(&untouched)==EBUSY);REQUIRE(dm_vk_memory_trim_caches(DM_VK_TRIM_ALL,&untouched)==EBUSY);REQUIRE(dm_android_nvn_clock_ns()-now<20000000);pthread_mutex_unlock(&G.lock);REQUIRE(!memcmp(&untouched,&original,sizeof original));
    pthread_mutex_lock(&memory_mu);REQUIRE(dm_vk_memory_get_stats(&untouched)==EBUSY);pthread_mutex_unlock(&memory_mu);REQUIRE(!memcmp(&untouched,&original,sizeof original));
    uint8_t *memory;REQUIRE(!posix_memalign((void **)&memory,16384,262144));memset(memory,0,262144);mtl_pool_add(memory,262144);
    tex_t source=make_texture(memory+32768,NVN_FORMAT_R32F,8,7001),bc=make_texture(memory+65536,NVN_FORMAT_RGBA_DXT1,8,7002);
    float source_pixels[64];for(unsigned i=0;i<64;i++)source_pixels[i]=.25f;REQUIRE(!nvn_swizzle(&source.d,0,0,source_pixels,32,source.mem));
    mtl_set_tex_pool_n(2);mtl_reg_tex(1,&source,NULL);
    mtl_sub_begin();VTexture *t=texture_get(&source,1);VTexture *bt=texture_get(&bc,1);REQUIRE(bt->cpu_snapshot&&bt->cpu_snapshot_size==bc.L.size);
    VPool *pool=pool_find((uint64_t)(uintptr_t)memory,4,NULL);REQUIRE(pool);pool_materialize(pool);
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearColorValue color={.float32={.625f,0,0,0}};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(S.command,t->image,t->layout,&color,1,&range);t->cpu_owned=0;tex_dirty(t);
    prepare_derived(&source);VBuffer *scratch=temporary(4096,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);memset(scratch->map,0x5a,4096);
    S.wb_stage=buffer_create_kind(4096,VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,DM_VK_MEMORY_WRITEBACK);
    uint64_t primary=t->allocation_size+bt->allocation_size,derived=t->sample_allocation_size+t->shadow_allocation_size;
    VkMemoryRequirements req;vkGetImageMemoryRequirements(G.device,t->image,&req);REQUIRE(req.size==t->allocation_size);vkGetBufferMemoryRequirements(G.device,pool->v.b,&req);REQUIRE(req.size==pool->v.allocation_size);
    mtl_sub_commit();if(argc==2)verify_derived_pixels(t,argv[1]);verify_inflight_trim();DmVkMemoryStats before=stats();REQUIRE(before.bytes[DM_VK_MEMORY_TEXTURE]==primary);REQUIRE(before.bytes[DM_VK_MEMORY_SAMPLE_COPY]+before.bytes[DM_VK_MEMORY_SHADOW]==derived);REQUIRE(before.cpu_baseline_bytes==bc.L.size);REQUIRE(before.temp_retired_bytes==before.bytes[DM_VK_MEMORY_TEMP]);REQUIRE(!before.temp_recording_bytes&&!before.temp_inflight_bytes);
    VkImage main_image=t->image;TextureRef ref=texture_reference(t);uint64_t pool_bytes=before.bytes[DM_VK_MEMORY_POOL];uint64_t reclaimed=derived+before.temp_retired_bytes+before.bytes[DM_VK_MEMORY_WRITEBACK];
    REQUIRE(!dm_vk_memory_trim_caches(DM_VK_TRIM_ALL,&untouched));DmVkMemoryStats after=stats();REQUIRE(after.total_bytes+reclaimed==before.total_bytes);REQUIRE(after.last_trim_freed_bytes==reclaimed);REQUIRE(after.trim_count==before.trim_count+1);
    REQUIRE(!after.bytes[DM_VK_MEMORY_TEMP]&&!after.bytes[DM_VK_MEMORY_SAMPLE_COPY]&&!after.bytes[DM_VK_MEMORY_SHADOW]&&!after.bytes[DM_VK_MEMORY_WRITEBACK]);REQUIRE(after.bytes[DM_VK_MEMORY_TEXTURE]==primary&&after.bytes[DM_VK_MEMORY_POOL]==pool_bytes&&after.cpu_baseline_bytes==bc.L.size);
    REQUIRE(t->image==main_image&&texture_reference_get(ref)==t&&!t->sample_copy&&!t->shadow&&!t->sc_valid&&!t->shadow_version&&!t->cpu_owned);
    REQUIRE(!dm_vk_memory_trim_caches(DM_VK_TRIM_ALL,&untouched)&&untouched.last_trim_freed_bytes==0);
    mtl_sub_begin();prepare_derived(&source);VBuffer *fresh=temporary(4096,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);for(unsigned i=0;i<4096;i++)REQUIRE(fresh->map[i]==0);mtl_sub_commit();if(argc==2)verify_derived_pixels(t,argv[1]);DmVkMemoryStats restored=stats();REQUIRE(restored.bytes[DM_VK_MEMORY_SAMPLE_COPY]+restored.bytes[DM_VK_MEMORY_SHADOW]==derived);
    float readback[64];NVNcopyRegion region={0,0,0,8,8,1};mtl_read_texels(&source,0,0,&region,(uint8_t *)readback,32,0);for(unsigned i=0;i<64;i++)REQUIRE(readback[i]==.625f);
    /* Releasing the lower slot must not hide the remaining higher slot. */
    mtl_tex_release(&source);DmVkMemoryStats sparse=stats();REQUIRE(G.texture_used==2&&G.ntexture==1&&sparse.bytes[DM_VK_MEMORY_TEXTURE]==bt->allocation_size);REQUIRE(mtl_upload_bytes()==(uint64_t)bt->width*bt->height*bt->bpp);
    mtl_tex_release(&bc);mtl_pool_del(memory);REQUIRE(!dm_vk_memory_trim_caches(DM_VK_TRIM_ALL,&untouched));DmVkMemoryStats final=stats();REQUIRE(final.total_bytes==base.total_bytes&&!final.cpu_baseline_bytes);
    free(memory);printf("ANDROID_MEMORY_TRIM_OK checks=%u allocation_sizes=1 sparse_registry=1 nonblocking_stats=1 real_inflight_fence_blocks_trim=1 retired_derived_writeback_trim=1 primary_pool_retained=1 gpu_written_primary_preserved=1 derived_recreated=1 derived_gpu_pixels=%d idempotent=1 exact_reclaimed_bytes=%llu synthetic_fixture=1 original_game=0\n",checks,argc==2,(unsigned long long)reclaimed);return 0;
}
