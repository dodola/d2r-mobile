/* Standalone guard regression: no game, APK, surface or external assets.
 * Four points have z=-.25,.25,.75,1.25 at w=1. After the exact redundant-clip
 * proof and unused-member pass, only the middle two may rasterize. This tests
 * Vulkan's native 0..1 depth clipping with the mandatory depth-clamp guard.
 */
#include <vulkan/vulkan.h>
#include "../../gfx/nvn/nvn_vk_redundant_clip.h"
#include "../../gfx/nvn/nvn_vk_unused_distance.h"
#include <stdio.h>
#define VK_OK(call) do {VkResult result=(call);if(result!=VK_SUCCESS){fprintf(stderr,"%s: %d\n",#call,result);return 3;}}while(0)
static uint32_t *read_spv(const char *path,size_t *words) {
    FILE *file=fopen(path,"rb");if(!file)return NULL;
    if(fseek(file,0,SEEK_END)){fclose(file);return NULL;}
    long bytes=ftell(file);rewind(file);
    if(bytes<20||bytes%4){fclose(file);return NULL;}
    uint32_t *code=malloc((size_t)bytes);if(!code){fclose(file);return NULL;}
    if(fread(code,1,(size_t)bytes,file)!=(size_t)bytes){free(code);fclose(file);return NULL;}
    fclose(file);*words=(size_t)bytes/4;return code;
}
static uint32_t memory_type(VkPhysicalDevice gpu,uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties;vkGetPhysicalDeviceMemoryProperties(gpu,&properties);
    for(uint32_t i=0;i<properties.memoryTypeCount;i++)if((bits&(1u<<i))&&
        (properties.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return i;
    return UINT32_MAX;
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    size_t words=0,proven_words=0,clean_words=0;int guard=0;uint32_t removed=0;const char *error=NULL;
    uint32_t *input=read_spv(argv[1],&words);if(!input)return 2;
    uint32_t *proven=nvn_vk_redundant_clip(input,words,&proven_words,&guard,&error);free(input);
    if(!proven||!guard||proven_words>=words){fprintf(stderr,"redundant clip proof failed: %s guard=%d\n",error?error:"unsupported",guard);free(proven);return 4;}
    uint32_t *code=nvn_vk_unused_distance(proven,proven_words,&clean_words,&removed,&error);free(proven);
    if(!code||!(removed&1)){fprintf(stderr,"unused clip cleanup failed: %s mask=%u\n",error?error:"unsupported",removed);free(code);return 4;}
    for(size_t at=5;at<clean_words;at+=code[at]>>16){
        unsigned op=code[at]&65535u,n=code[at]>>16;
        if((op==17&&n==2&&(code[at+1]==32||code[at+1]==33))||
           (op==71&&n==4&&code[at+2]==11&&(code[at+3]==3||code[at+3]==4))||
           (op==72&&n==5&&code[at+3]==11&&(code[at+4]==3||code[at+4]==4))){
            fputs("distance declarations remain after cleanup\n",stderr);free(code);return 4;
        }
    }
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Native depth clip guard regression",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;VK_OK(vkCreateInstance(&ii,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice gpu;VK_OK(vkEnumeratePhysicalDevices(instance,&count,&gpu));if(count!=1)return 5;
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(gpu,&features);
    if(!features.fragmentStoresAndAtomics){fputs("fragmentStoresAndAtomics unavailable\n",stderr);return 6;}
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(gpu,&properties);
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,NULL);
    VkQueueFamilyProperties *qprops=calloc(families,sizeof *qprops);if(!qprops)return 6;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,qprops);uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<families;i++)if(qprops[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
    free(qprops);if(family==UINT32_MAX)return 6;
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    /* Intentionally enable neither shaderClipDistance nor shaderCullDistance,
     * nor depthClamp, even on devices that advertise those optional features. */
    VkPhysicalDeviceFeatures enabled={.fragmentStoresAndAtomics=VK_TRUE};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&enabled};
    VkDevice device;VK_OK(vkCreateDevice(gpu,&di,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4096,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};VkBuffer buffer;VK_OK(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,buffer,&requirements);uint32_t mt=memory_type(gpu,requirements.memoryTypeBits);if(mt==UINT32_MAX)return 6;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=mt};VkDeviceMemory memory;VK_OK(vkAllocateMemory(device,&ma,NULL,&memory));VK_OK(vkBindBufferMemory(device,buffer,memory,0));
    void *mapped;VK_OK(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped));memset(mapped,0xcd,1024);
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};VkDescriptorSetLayout layout;VK_OK(vkCreateDescriptorSetLayout(device,&li,NULL,&layout));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};VkDescriptorPool pool;VK_OK(vkCreateDescriptorPool(device,&dpi,NULL,&pool));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};VkDescriptorSet ds;VK_OK(vkAllocateDescriptorSets(device,&dai,&ds));
    VkDescriptorBufferInfo db={buffer,0,1024};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};vkUpdateDescriptorSets(device,1,&write,0,NULL);
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};VkPipelineLayout pipeline_layout;VK_OK(vkCreatePipelineLayout(device,&pli,NULL,&pipeline_layout));
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS};VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.subpassCount=1,.pSubpasses=&subpass};VkRenderPass render_pass;VK_OK(vkCreateRenderPass(device,&ri,NULL,&render_pass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=render_pass,.width=4,.height=1,.layers=1};VkFramebuffer framebuffer;VK_OK(vkCreateFramebuffer(device,&fi,NULL,&framebuffer));
    VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=clean_words*4,.pCode=code};VkShaderModule vertex;VK_OK(vkCreateShaderModule(device,&si,NULL,&vertex));free(code);
    code=read_spv(argv[2],&words);if(!code)return 2;
    si.codeSize=words*4;si.pCode=code;VkShaderModule fragment;VK_OK(vkCreateShaderModule(device,&si,NULL,&fragment));free(code);
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.depthClampEnable=VK_FALSE,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
    VkViewport viewport={0,0,4,1,0,1};VkRect2D scissor={{0,0},{4,1}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo gi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pColorBlendState=&cb,.layout=pipeline_layout,.renderPass=render_pass};
    VkPipeline pipeline;VK_OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gi,NULL,&pipeline));
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};VkCommandPool command_pool;VK_OK(vkCreateCommandPool(device,&cpi,NULL,&command_pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;VK_OK(vkAllocateCommandBuffers(device,&cai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_OK(vkBeginCommandBuffer(command,&begin));
    VkRenderPassBeginInfo render={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=render_pass,.framebuffer=framebuffer,.renderArea={{0,0},{4,1}}};vkCmdBeginRenderPass(command,&render,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&ds,0,NULL);vkCmdDraw(command,4,1,0,0);vkCmdEndRenderPass(command);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    VK_OK(vkEndCommandBuffer(command));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};VK_OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VK_OK(vkQueueWaitIdle(queue));
    const uint32_t *actual=mapped,expected[4]={0xcdcdcdcdu,1,1,0xcdcdcdcdu};
    for(unsigned i=0;i<256;i++){uint32_t want=i<4?expected[i]:0xcdcdcdcdu;if(actual[i]!=want){fprintf(stderr,"native depth clip slot=%u got=0x%08x expected=0x%08x\n",i,actual[i],want);return 7;}}
    printf("{\"passed\":true,\"fixture_only\":true,\"mode\":\"guarded\",\"gpu\":\"%s\",\"require_native_depth_clip\":true,\"depth_clamp_enabled\":false,\"clip_cull_features_enabled\":false,\"removed_mask\":%u,\"checked_slots\":4,\"checked_guard_words\":252,\"negative_z_clipped\":true,\"positive_z_visible\":true,\"beyond_far_clipped\":true}\n",properties.deviceName,removed);
    vkDestroyCommandPool(device,command_pool,NULL);vkDestroyPipeline(device,pipeline,NULL);vkDestroyShaderModule(device,fragment,NULL);vkDestroyShaderModule(device,vertex,NULL);vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyRenderPass(device,render_pass,NULL);vkDestroyPipelineLayout(device,pipeline_layout,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,layout,NULL);vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);return 0;
}
