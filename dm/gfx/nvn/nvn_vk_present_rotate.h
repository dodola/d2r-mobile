/* 应用侧预旋转呈现：避免 Turnip 在 SM8750 上补偿窗口方向时交替转置。
 * Application pre-rotation avoids intermittent Turnip orientation compensation on SM8750.
 * 自编全屏三角形；遵循 Vulkan preTransform 与获取信号量依赖。
 * Authored full-screen triangle, following Vulkan preTransform and acquire semaphore dependencies. */
#include "nvn_vk_present_rotate_spv.inc"
static struct {
    VkDescriptorSetLayout descriptors;VkDescriptorPool pool;VkDescriptorSet set;
    VkPipelineLayout layout;VkPipeline pipeline;VkRenderPass pass;VkSampler sampler;
    VkImageView views[16];VkFramebuffer framebuffers[16];
} PresentRotate;
typedef struct {float crop[4],destination[2];uint32_t rotation,flip_y;} PresentRotateRegion;
_Static_assert(sizeof(PresentRotateRegion)==32,"present rotation push ABI");
static void present_rotate_destroy(void) {
    for(unsigned i=0;i<16;i++){
        if(PresentRotate.framebuffers[i])vkDestroyFramebuffer(G.device,PresentRotate.framebuffers[i],NULL);
        if(PresentRotate.views[i])vkDestroyImageView(G.device,PresentRotate.views[i],NULL);
    }
    if(PresentRotate.pipeline)vkDestroyPipeline(G.device,PresentRotate.pipeline,NULL);
    if(PresentRotate.pass)vkDestroyRenderPass(G.device,PresentRotate.pass,NULL);
    if(PresentRotate.pool)vkDestroyDescriptorPool(G.device,PresentRotate.pool,NULL);
    if(PresentRotate.sampler)vkDestroySampler(G.device,PresentRotate.sampler,NULL);
    if(PresentRotate.layout)vkDestroyPipelineLayout(G.device,PresentRotate.layout,NULL);
    if(PresentRotate.descriptors)vkDestroyDescriptorSetLayout(G.device,PresentRotate.descriptors,NULL);
    memset(&PresentRotate,0,sizeof PresentRotate);
}
static void present_rotate_resources(void) {
    if(PresentRotate.pipeline)return;
    VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo di={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    CHECK(vkCreateDescriptorSetLayout(G.device,&di,NULL,&PresentRotate.descriptors));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    CHECK(vkCreateDescriptorPool(G.device,&pi,NULL,&PresentRotate.pool));
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=PresentRotate.pool,.descriptorSetCount=1,.pSetLayouts=&PresentRotate.descriptors};
    CHECK(vkAllocateDescriptorSets(G.device,&ai,&PresentRotate.set));
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PresentRotateRegion)};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&PresentRotate.descriptors,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    CHECK(vkCreatePipelineLayout(G.device,&li,NULL,&PresentRotate.layout));
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    CHECK(vkCreateSampler(G.device,&si,NULL,&PresentRotate.sampler));
    VkAttachmentDescription attachment={.format=G.swap_format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&sub};
    CHECK(vkCreateRenderPass(G.device,&ri,NULL,&PresentRotate.pass));
    VkShaderModule vertex=depth_copy_module(present_rotate_vert_spv,sizeof present_rotate_vert_spv);
    VkShaderModule fragment=depth_copy_module(present_rotate_frag_spv,sizeof present_rotate_frag_spv);
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=states};
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,
        .pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&cb,.pDynamicState=&dy,
        .layout=PresentRotate.layout,.renderPass=PresentRotate.pass};
    CHECK(vkCreateGraphicsPipelines(G.device,VK_NULL_HANDLE,1,&gp,NULL,&PresentRotate.pipeline));
    vkDestroyShaderModule(G.device,vertex,NULL);vkDestroyShaderModule(G.device,fragment,NULL);
    for(unsigned i=0;i<G.nswap;i++){
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=G.swap_images[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,
            .format=G.swap_format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        CHECK(vkCreateImageView(G.device,&view,NULL,&PresentRotate.views[i]));
        VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=PresentRotate.pass,.attachmentCount=1,.pAttachments=&PresentRotate.views[i],
            .width=G.swap_extent.width,.height=G.swap_extent.height,.layers=1};
        CHECK(vkCreateFramebuffer(G.device,&fb,NULL,&PresentRotate.framebuffers[i]));
    }
}
static void present_rotate_draw(VTexture *source,unsigned index,int x,int y,int width,int height,int flip_y) {
    /* 所有呈现都复用 submission_slot2；sub_begin_ctx 已等其上次 fence，单个描述符集可安全更新。
     * Every present uses submission_slot2; sub_begin_ctx waited its previous fence, so this one set is safe to update. */
    if(G.current_submission!=&G.submission_slot2)die("present rotation slot ownership",0);
    present_rotate_resources();texture_transition(source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=source->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=source->format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDescriptorImageInfo image={PresentRotate.sampler,submission_image_view(&vi),source->layout};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=PresentRotate.set,.dstBinding=0,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image};
    vkUpdateDescriptorSets(G.device,1,&write,0,NULL);
    PresentRotateRegion region={{(float)x/source->width,(float)y/source->height,(float)width/source->width,(float)height/source->height},
        {(float)G.swap_extent.width,(float)G.swap_extent.height},G.swap_rotation,(uint32_t)flip_y};
    VkRect2D area={{0,0},G.swap_extent};VkViewport viewport={0,0,(float)G.swap_extent.width,(float)G.swap_extent.height,0,1};
    VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=PresentRotate.pass,.framebuffer=PresentRotate.framebuffers[index],.renderArea=area};
    vkCmdBeginRenderPass(S.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(S.command,0,1,&viewport);vkCmdSetScissor(S.command,0,1,&area);
    vkCmdBindPipeline(S.command,VK_PIPELINE_BIND_POINT_GRAPHICS,PresentRotate.pipeline);
    vkCmdBindDescriptorSets(S.command,VK_PIPELINE_BIND_POINT_GRAPHICS,PresentRotate.layout,0,1,&PresentRotate.set,0,NULL);
    vkCmdPushConstants(S.command,PresentRotate.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof region,&region);
    vkCmdDraw(S.command,3,1,0,0);vkCmdEndRenderPass(S.command);
    nvn_vk_command_state_reset(&S.command_state);
}
