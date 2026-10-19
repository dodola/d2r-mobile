/* Standalone Vulkan fixture: a single-layer 2D depth image has both 2D and
 * 2D_ARRAY depth views. Real comparison samples must match through both views.
 * No APK, game data, window, surface or device-specific extensions are used. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_OK(call) do { VkResult result=(call); if(result!=VK_SUCCESS) { \
    fprintf(stderr,"%s: %d\n",#call,result); return 3; } } while(0)

static uint32_t *read_spv(const char *path,size_t *words) {
    FILE *file=fopen(path,"rb"); if(!file)return NULL;
    if(fseek(file,0,SEEK_END)){fclose(file);return NULL;}
    long bytes=ftell(file);rewind(file);
    if(bytes<20||bytes%4){fclose(file);return NULL;}
    uint32_t *code=malloc((size_t)bytes);if(!code){fclose(file);return NULL;}
    if(fread(code,1,(size_t)bytes,file)!=(size_t)bytes){free(code);fclose(file);return NULL;}
    fclose(file);*words=(size_t)bytes/4;return code;
}
static uint32_t memory_type(VkPhysicalDevice gpu,uint32_t bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties;vkGetPhysicalDeviceMemoryProperties(gpu,&properties);
    for(uint32_t i=0;i<properties.memoryTypeCount;i++)
        if((bits&(1u<<i))&&(properties.memoryTypes[i].propertyFlags&flags)==flags)return i;
    return UINT32_MAX;
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Single layer depth image views",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance;VK_OK(vkCreateInstance(&ii,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice gpu;VK_OK(vkEnumeratePhysicalDevices(instance,&count,&gpu));if(count!=1)return 5;
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(gpu,&features);
    if(!features.fragmentStoresAndAtomics){fputs("fragmentStoresAndAtomics unavailable\n",stderr);return 6;}
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(gpu,&properties);
    VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(gpu,VK_FORMAT_D32_SFLOAT,&format);
    VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((format.optimalTilingFeatures&required)!=required){fputs("D32 sampled/transfer destination unavailable\n",stderr);return 6;}
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,NULL);
    VkQueueFamilyProperties *qprops=calloc(families,sizeof *qprops);if(!qprops)return 6;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,qprops);uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<families;i++)if(qprops[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
    free(qprops);if(family==UINT32_MAX)return 6;
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures enabled={.fragmentStoresAndAtomics=VK_TRUE};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&enabled};
    VkDevice device;VK_OK(vkCreateDevice(gpu,&di,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=1024,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;VK_OK(vkCreateBuffer(device,&bi,NULL,&buffer));VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,buffer,&requirements);
    uint32_t mt=memory_type(gpu,requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);if(mt==UINT32_MAX)return 6;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=mt};
    VkDeviceMemory memory;VK_OK(vkAllocateMemory(device,&ma,NULL,&memory));VK_OK(vkBindBufferMemory(device,buffer,memory,0));
    void *mapped;VK_OK(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped));memset(mapped,0xcd,1024);
    /* No mutable-format, cube-compatible or 2D-array-compatible image flags. */
    VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_D32_SFLOAT,.extent={2,2,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image;VK_OK(vkCreateImage(device,&image_info,NULL,&image));vkGetImageMemoryRequirements(device,image,&requirements);
    mt=memory_type(gpu,requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(mt==UINT32_MAX)mt=memory_type(gpu,requirements.memoryTypeBits,0);if(mt==UINT32_MAX)return 6;
    ma.allocationSize=requirements.size;ma.memoryTypeIndex=mt;VkDeviceMemory image_memory;VK_OK(vkAllocateMemory(device,&ma,NULL,&image_memory));VK_OK(vkBindImageMemory(device,image,image_memory,0));
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_D32_SFLOAT,.components={VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY},.subresourceRange=range};
    VkImageView views[2];VK_OK(vkCreateImageView(device,&view_info,NULL,&views[0]));view_info.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;VK_OK(vkCreateImageView(device,&view_info,NULL,&views[1]));
    VkSamplerCreateInfo sampler_info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.compareEnable=VK_TRUE,.compareOp=VK_COMPARE_OP_LESS_OR_EQUAL,.maxLod=0};
    VkSampler sampler;VK_OK(vkCreateSampler(device,&sampler_info,NULL,&sampler));
    VkDescriptorSetLayoutBinding bindings[3]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},{.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},{.binding=2,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=3,.pBindings=bindings};VkDescriptorSetLayout layout;VK_OK(vkCreateDescriptorSetLayout(device,&li,NULL,&layout));
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2}};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};VkDescriptorPool pool;VK_OK(vkCreateDescriptorPool(device,&dpi,NULL,&pool));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};VkDescriptorSet ds;VK_OK(vkAllocateDescriptorSets(device,&dai,&ds));
    VkDescriptorBufferInfo db={buffer,0,1024};VkDescriptorImageInfo images[2]={{sampler,views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,views[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[3]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db},{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[0]},{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=2,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[1]}};
    vkUpdateDescriptorSets(device,3,writes,0,NULL);
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};VkPipelineLayout pipeline_layout;VK_OK(vkCreatePipelineLayout(device,&pli,NULL,&pipeline_layout));
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS};VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.subpassCount=1,.pSubpasses=&subpass};VkRenderPass render_pass;VK_OK(vkCreateRenderPass(device,&ri,NULL,&render_pass));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=render_pass,.width=6,.height=1,.layers=1};VkFramebuffer framebuffer;VK_OK(vkCreateFramebuffer(device,&fi,NULL,&framebuffer));
    VkShaderModule modules[2];
    for(unsigned i=0;i<2;i++){size_t words=0;uint32_t *code=read_spv(argv[i+1],&words);if(!code)return 2;VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=words*4,.pCode=code};VK_OK(vkCreateShaderModule(device,&si,NULL,&modules[i]));free(code);}
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
    VkViewport viewport={0,0,6,1,0,1};VkRect2D scissor={{0,0},{6,1}};VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo gi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pColorBlendState=&cb,.layout=pipeline_layout,.renderPass=render_pass};VkPipeline pipeline;VK_OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gi,NULL,&pipeline));
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};VkCommandPool command_pool;VK_OK(vkCreateCommandPool(device,&cpi,NULL,&command_pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;VK_OK(vkAllocateCommandBuffers(device,&cai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_OK(vkBeginCommandBuffer(command,&begin));
    VkImageMemoryBarrier image_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&image_barrier);
    VkClearDepthStencilValue clear={.depth=0.5f};vkCmdClearDepthStencilImage(command,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    image_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;image_barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image_barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,NULL,0,NULL,1,&image_barrier);
    VkRenderPassBeginInfo render={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=render_pass,.framebuffer=framebuffer,.renderArea={{0,0},{6,1}}};vkCmdBeginRenderPass(command,&render,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&ds,0,NULL);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    VK_OK(vkEndCommandBuffer(command));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};VK_OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VK_OK(vkQueueWaitIdle(queue));
    const uint32_t *actual=mapped,expected[6]={1000,0,1000,1000,0,1000};
    for(unsigned i=0;i<256;i++){uint32_t want=i<6?expected[i]:0xcdcdcdcdu;if(actual[i]!=want){fprintf(stderr,"image view slot=%u got=0x%08x expected=0x%08x\n",i,actual[i],want);return 7;}}
    printf("{\"passed\":true,\"fixture_only\":true,\"gpu\":\"%s\",\"image_type\":\"2D\",\"format\":\"D32_SFLOAT\",\"array_layers\":1,\"image_flags\":0,\"depth_aspect\":true,\"comparison\":\"LESS_OR_EQUAL\",\"views\":[\"2D\",\"2D_ARRAY\"],\"checked_slots\":6,\"checked_guard_words\":250,\"results\":[1000,0,1000,1000,0,1000]}\n",properties.deviceName);
    vkDestroyCommandPool(device,command_pool,NULL);vkDestroyPipeline(device,pipeline,NULL);for(unsigned i=0;i<2;i++)vkDestroyShaderModule(device,modules[i],NULL);
    vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyRenderPass(device,render_pass,NULL);vkDestroyPipelineLayout(device,pipeline_layout,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,layout,NULL);vkDestroySampler(device,sampler,NULL);for(unsigned i=0;i<2;i++)vkDestroyImageView(device,views[i],NULL);vkDestroyImage(device,image,NULL);vkFreeMemory(device,image_memory,NULL);vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);return 0;
}
