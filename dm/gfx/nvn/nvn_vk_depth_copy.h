/* Vulkan scaled depth/stencil copies. Authored for the NVN backend.
 * A sampled depth draw plus eight masked stencil draws preserves both planes
 * without shader-stencil-export. Native blits remain the preferred path.
 * Included after copy_layers(), with the backend's submission helpers in scope.
 */
#include "nvn_vk_depth_copy_spv.inc"
typedef struct {
    int32_t source[2],destination[2];uint32_t source_extent[2],destination_extent[2],bit;
} DepthCopyRegion;
_Static_assert(sizeof(DepthCopyRegion)==36,"depth copy push constant ABI");
static struct {
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkSampler sampler;
    VkShaderModule vertex,depth,stencil;
    VkPipeline pipelines[256][2];
    unsigned logged[256];
    uint64_t blits,draws,plain_copies;
} DepthCopy;

static VkShaderModule depth_copy_module(const uint32_t *words,size_t size) {
    VkShaderModule module;
    VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=words};
    CHECK(vkCreateShaderModule(G.device,&info,NULL,&module));return module;
}
static void depth_copy_resources(void) {
    if(DepthCopy.layout)return;
    VkDescriptorSetLayoutBinding bindings[2]={
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo di={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    CHECK(vkCreateDescriptorSetLayout(G.device,&di,NULL,&DepthCopy.descriptors));
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(DepthCopyRegion)};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&DepthCopy.descriptors,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push};
    CHECK(vkCreatePipelineLayout(G.device,&li,NULL,&DepthCopy.layout));
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(vkCreateSampler(G.device,&si,NULL,&DepthCopy.sampler));
    DepthCopy.vertex=depth_copy_module(depth_copy_vert_spv,sizeof(depth_copy_vert_spv));
    DepthCopy.depth=depth_copy_module(depth_copy_frag_spv,sizeof(depth_copy_frag_spv));
    DepthCopy.stencil=depth_copy_module(stencil_copy_frag_spv,sizeof(stencil_copy_frag_spv));
}
static VkPipeline depth_copy_pipeline(VkFormat format,VkRenderPass pass,int stencil) {
    if((unsigned)format>=256)die("depth copy format",format);
    VkPipeline *result=&DepthCopy.pipelines[format][stencil];if(*result)return *result;
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=DepthCopy.vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,
         .module=stencil?DepthCopy.stencil:DepthCopy.depth,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkStencilOpState operation={.failOp=VK_STENCIL_OP_KEEP,.passOp=VK_STENCIL_OP_REPLACE,.depthFailOp=VK_STENCIL_OP_KEEP,
        .compareOp=VK_COMPARE_OP_ALWAYS,.compareMask=255,.writeMask=255};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=!stencil,.depthWriteEnable=!stencil,.depthCompareOp=VK_COMPARE_OP_ALWAYS,
        .stencilTestEnable=(aspect(format)&VK_IMAGE_ASPECT_STENCIL_BIT)!=0,.front=operation,.back=operation};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_STENCIL_REFERENCE,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=ds.stencilTestEnable?4u:2u,.pDynamicStates=states};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,
        .pDepthStencilState=&ds,.pColorBlendState=&cb,.pDynamicState=&dy,.layout=DepthCopy.layout,.renderPass=pass};
    uint64_t started=dm_android_nvn_clock_ns();
    pthread_mutex_lock(&PC.cache_lock);
    VkResult status=vkCreateGraphicsPipelines(G.device,G.pipeline_cache,1,&gp,NULL,result);
    if(status==VK_SUCCESS)atomic_fetch_add(&PC.revision,1);
    pthread_mutex_unlock(&PC.cache_lock);CHECK(status);
    G.profile_pipeline_ns+=dm_android_nvn_clock_ns()-started;return *result;
}
static VkImageView depth_copy_view(VTexture *t,uint32_t level,uint32_t layer,VkImageAspectFlags plane) {
    VkImageViewCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=t->format,.subresourceRange={plane,level,1,layer,1}};
    return submission_image_view(&info);
}
static int depth_copy_framebuffer(VTexture *t,uint32_t level,uint32_t layer) {
    FramebufferKey key;memset(&key,0,sizeof key);
    key.width=t->width>>level;if(!key.width)key.width=1;key.height=t->height>>level;if(!key.height)key.height=1;
    key.views[0]=depth_copy_view(t,level,layer,aspect(t->format));
    key.pass.count=key.pass.has_depth=1;
    key.pass.attachments[0]=(VkAttachmentDescription){.format=t->format,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    key.pass.depth=(VkAttachmentReference){0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    int index=0;
    if(!G.disable_resource_cache){for(;index<S.npass;index++)if(!memcmp(&S.framebuffer_keys[index],&key,sizeof key))return index;}
    else index=S.npass;
    if(S.npass==LIMIT_WRITES)die("depth copy render pass count",S.npass);
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.pDepthStencilAttachment=&key.pass.depth};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=key.pass.attachments,
        .subpassCount=1,.pSubpasses=&sub};
    CHECK(vkCreateRenderPass(G.device,&ri,NULL,&S.passes[index]));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=S.passes[index],.attachmentCount=1,
        .pAttachments=key.views,.width=key.width,.height=key.height,.layers=1};
    CHECK(vkCreateFramebuffer(G.device,&fi,NULL,&S.framebuffers[index]));
    S.framebuffer_keys[index]=key;S.npass++;return index;
}
static void depth_copy_draw(VTexture *src,VTexture *dst,VkImageSubresourceLayers sl,VkImageSubresourceLayers dl,
                            const NVNcopyRegion *sr,const NVNcopyRegion *dr) {
    if(src->depth!=1||dst->depth!=1||src->source.d.target==NVN_TEXTURE_TARGET_1D||src->source.d.target==NVN_TEXTURE_TARGET_1D_ARRAY||
       dst->source.d.target==NVN_TEXTURE_TARGET_1D||dst->source.d.target==NVN_TEXTURE_TARGET_1D_ARRAY)
        die("drawn depth copy needs 2D image layers",src->source.d.target);
    if((uint64_t)(2ull*dr->width-1)*sr->width>UINT32_MAX||(uint64_t)(2ull*dr->height-1)*sr->height>UINT32_MAX)
        die("drawn depth copy mapping overflow",sr->width);
    depth_copy_resources();pass_end();
    texture_transition(src,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture_transition(dst,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    int stencil=(aspect(src->format)&VK_IMAGE_ASPECT_STENCIL_BIT)!=0;
    DepthCopyRegion region={{sr->xoffset,sr->yoffset},{dr->xoffset,dr->yoffset},
        {(uint32_t)sr->width,(uint32_t)sr->height},{(uint32_t)dr->width,(uint32_t)dr->height},0};
    VkViewport viewport={(float)dr->xoffset,(float)dr->yoffset,(float)dr->width,(float)dr->height,0,1};
    VkRect2D area={{dr->xoffset,dr->yoffset},{(uint32_t)dr->width,(uint32_t)dr->height}};
    for(uint32_t layer=0;layer<sl.layerCount;layer++) {
        VkDescriptorSet set;
        VkDescriptorSetAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=S.descriptors,
            .descriptorSetCount=1,.pSetLayouts=&DepthCopy.descriptors};
        CHECK(vkAllocateDescriptorSets(G.device,&allocation,&set));
        VkDescriptorImageInfo images[2]={{DepthCopy.sampler,depth_copy_view(src,sl.mipLevel,sl.baseArrayLayer+layer,VK_IMAGE_ASPECT_DEPTH_BIT),src->layout},{0}};
        if(stencil)images[1]=(VkDescriptorImageInfo){DepthCopy.sampler,depth_copy_view(src,sl.mipLevel,sl.baseArrayLayer+layer,VK_IMAGE_ASPECT_STENCIL_BIT),src->layout};
        VkWriteDescriptorSet writes[2];memset(writes,0,sizeof writes);
        for(unsigned i=0;i<(stencil?2u:1u);i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
        vkUpdateDescriptorSets(G.device,stencil?2:1,writes,0,NULL);
        int fb=depth_copy_framebuffer(dst,dl.mipLevel,dl.baseArrayLayer+layer);
        VkPipeline depth=depth_copy_pipeline(dst->format,S.passes[fb],0);
        VkPipeline bits=stencil?depth_copy_pipeline(dst->format,S.passes[fb],1):VK_NULL_HANDLE;
        VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=S.passes[fb],.framebuffer=S.framebuffers[fb],.renderArea=area};
        if(G.precise_sync)hz_pass_sync();
        vkCmdBeginRenderPass(S.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(S.command,0,1,&viewport);vkCmdSetScissor(S.command,0,1,&area);
        vkCmdBindDescriptorSets(S.command,VK_PIPELINE_BIND_POINT_GRAPHICS,DepthCopy.layout,0,1,&set,0,NULL);
        vkCmdBindPipeline(S.command,VK_PIPELINE_BIND_POINT_GRAPHICS,depth);
        if(stencil){vkCmdSetStencilReference(S.command,VK_STENCIL_FACE_FRONT_AND_BACK,0);vkCmdSetStencilWriteMask(S.command,VK_STENCIL_FACE_FRONT_AND_BACK,255);}
        region.bit=0;vkCmdPushConstants(S.command,DepthCopy.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof region,&region);
        vkCmdDraw(S.command,3,1,0,0);
        if(stencil){
            vkCmdBindPipeline(S.command,VK_PIPELINE_BIND_POINT_GRAPHICS,bits);
            vkCmdSetStencilReference(S.command,VK_STENCIL_FACE_FRONT_AND_BACK,255);
            for(region.bit=0;region.bit<8;region.bit++){
                vkCmdSetStencilWriteMask(S.command,VK_STENCIL_FACE_FRONT_AND_BACK,1u<<region.bit);
                vkCmdPushConstants(S.command,DepthCopy.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof region,&region);
                vkCmdDraw(S.command,3,1,0,0);
            }
        }
        vkCmdEndRenderPass(S.command);
    }
    /* Utility bindings replace Vulkan command state, but not the guest state.
     * The next guest draw/dispatch must rebind even if its handles match. */
    nvn_vk_command_state_reset(&S.command_state);DepthCopy.draws++;
}
static void depth_copy_log(VkFormat format,int draw) {
    unsigned bit=draw?2u:1u;
    if(!(DepthCopy.logged[format]&bit)){
        fprintf(stderr,"[nvn-vulkan] DEPTH_COPY_PATH format=%u path=%s\n",format,draw?"draw":"blit");
        DepthCopy.logged[format]|=bit;
    }
}
#ifdef DM_VK_COPY_TEST
void dm_vk_depth_copy_stats(uint64_t *plain,uint64_t *blits,uint64_t *draws) {
    *plain=DepthCopy.plain_copies;*blits=DepthCopy.blits;*draws=DepthCopy.draws;
}
#endif
