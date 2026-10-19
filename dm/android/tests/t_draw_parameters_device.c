/* Real vertex-stage output verifies both native imports and the portable
 * specialization path, with nonzero first/base vertices and first instances.
 * Standalone Vulkan fixture: no game modules, surface, APK or saves. */
#include <vulkan/vulkan.h>
#include "../../gfx/nvn/nvn_vk_draw_parameters.h"
#include <stdio.h>
#include <assert.h>
#define VK_OK(call) do { VkResult r=(call); if(r!=VK_SUCCESS){fprintf(stderr,"%s: %d\n",#call,r);return 3;} } while(0)
static uint32_t memory_type(VkPhysicalDevice gpu,uint32_t bits) {
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(gpu,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&
       (p.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
       (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return i;
    return UINT32_MAX;
}
int main(int argc,char **argv) {
    if(argc!=4|| (strcmp(argv[2],"native")&&strcmp(argv[2],"portable")))return 2;
    int portable=!strcmp(argv[2],"portable");
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    if(fseek(f,0,SEEK_END))return 2;long bytes=ftell(f);rewind(f);
    if(bytes<20||bytes%4)return 2;uint32_t *code=malloc((size_t)bytes);
    if(!code||fread(code,1,(size_t)bytes,f)!=(size_t)bytes)return 2;fclose(f);
    uint32_t mask=0;if(portable){size_t words=0;uint32_t *out=nvn_vk_draw_parameters(code,(size_t)bytes/4,&words,&mask);if(!out||mask!=7)return 4;free(code);code=out;bytes=(long)words*4;}
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Draw parameters semantic fixture",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;VK_OK(vkCreateInstance(&ii,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice gpu;VK_OK(vkEnumeratePhysicalDevices(instance,&count,&gpu));if(count!=1)return 5;
    VkPhysicalDeviceShaderDrawParametersFeatures draw={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceFeatures2 feats={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&draw};
    PFN_vkGetPhysicalDeviceFeatures2 get_features=(PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceFeatures2");
    if(!get_features)return 6;get_features(gpu,&feats);
    if(!feats.features.fragmentStoresAndAtomics||(!portable&&!draw.shaderDrawParameters)){fprintf(stderr,"unsupported stores=%u draw_parameters=%u\n",feats.features.fragmentStoresAndAtomics,draw.shaderDrawParameters);return 6;}
    VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(gpu,&props);
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,NULL);
    VkQueueFamilyProperties *qp=calloc(families,sizeof *qp);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,qp);uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<families;i++)if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}free(qp);if(family==UINT32_MAX)return 6;
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures enabled={.fragmentStoresAndAtomics=VK_TRUE};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&draw,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&enabled};
    VkDevice dev;VK_OK(vkCreateDevice(gpu,&di,NULL,&dev));VkQueue queue;vkGetDeviceQueue(dev,family,0,&queue);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4096,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};VkBuffer buffer;VK_OK(vkCreateBuffer(dev,&bi,NULL,&buffer));
    VkMemoryRequirements mr;vkGetBufferMemoryRequirements(dev,buffer,&mr);uint32_t mt=memory_type(gpu,mr.memoryTypeBits);if(mt==UINT32_MAX)return 6;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=mt};VkDeviceMemory memory;VK_OK(vkAllocateMemory(dev,&ma,NULL,&memory));VK_OK(vkBindBufferMemory(dev,buffer,memory,0));
    void *mapped;VK_OK(vkMapMemory(dev,memory,0,VK_WHOLE_SIZE,0,&mapped));
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};VkDescriptorSetLayout layout;VK_OK(vkCreateDescriptorSetLayout(dev,&li,NULL,&layout));
    VkDescriptorPoolSize poolSize={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&poolSize};VkDescriptorPool dp;VK_OK(vkCreateDescriptorPool(dev,&dpi,NULL,&dp));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&layout};VkDescriptorSet ds;VK_OK(vkAllocateDescriptorSets(dev,&dai,&ds));
    VkDescriptorBufferInfo db={buffer,0,1024};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};vkUpdateDescriptorSets(dev,1,&write,0,NULL);
    VkPushConstantRange pc={VK_SHADER_STAGE_VERTEX_BIT,0,4};VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout,.pushConstantRangeCount=1,.pPushConstantRanges=&pc};VkPipelineLayout pl;VK_OK(vkCreatePipelineLayout(dev,&pli,NULL,&pl));
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS};VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.subpassCount=1,.pSubpasses=&sub};VkRenderPass rp;VK_OK(vkCreateRenderPass(dev,&ri,NULL,&rp));
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=rp,.width=3,.height=2,.layers=1};VkFramebuffer fb;VK_OK(vkCreateFramebuffer(dev,&fi,NULL,&fb));
    VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)bytes,.pCode=code};VkShaderModule shader;VK_OK(vkCreateShaderModule(dev,&si,NULL,&shader));free(code);
    f=fopen(argv[3],"rb");if(!f||fseek(f,0,SEEK_END))return 2;bytes=ftell(f);rewind(f);if(bytes<20||bytes%4)return 2;
    code=malloc((size_t)bytes);if(!code||fread(code,1,(size_t)bytes,f)!=(size_t)bytes)return 2;fclose(f);
    si.codeSize=(size_t)bytes;si.pCode=code;VkShaderModule fragment;VK_OK(vkCreateShaderModule(dev,&si,NULL,&fragment));free(code);
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool cp;VK_OK(vkCreateCommandPool(dev,&cpi,NULL,&cp));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer cmd;VK_OK(vkAllocateCommandBuffers(dev,&cai,&cmd));
    const int32_t args[3][3]={{5,7,0},{-2,11,0},{9,3,0}};
    for(unsigned c=0;c<3;c++) {
        memset(mapped,0xcd,1024);uint32_t indices[3]={8,9,10};memcpy((char *)mapped+2048,indices,sizeof indices);
        VkSpecializationMapEntry entries[3];for(unsigned i=0;i<3;i++)entries[i]=(VkSpecializationMapEntry){NVN_DRAW_SPEC_BASE+i,i*4,4};
        VkSpecializationInfo spec={3,entries,sizeof args[c],args[c]};
        VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=shader,.pName="main",.pSpecializationInfo=portable?&spec:NULL},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
        VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
        VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
        VkViewport viewport={0,0,3,2,0,1};VkRect2D scissor={{0,0},{3,2}};
        VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
        VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo gi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pViewportState=&vp,.pMultisampleState=&ms,.pColorBlendState=&cb,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pRasterizationState=&rs,.layout=pl,.renderPass=rp};
        VkPipeline pipeline;VK_OK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gi,NULL,&pipeline));
        VK_OK(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_OK(vkBeginCommandBuffer(cmd,&begin));
        VkRenderPassBeginInfo render={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,.framebuffer=fb,.renderArea={{0,0},{3,2}}};vkCmdBeginRenderPass(cmd,&render,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&ds,0,NULL);
        int32_t first=c==1?6:args[c][0];vkCmdPushConstants(cmd,pl,VK_SHADER_STAGE_VERTEX_BIT,0,4,&first);
        if(c==1){vkCmdBindIndexBuffer(cmd,buffer,2048,VK_INDEX_TYPE_UINT32);vkCmdDrawIndexed(cmd,3,2,0,args[c][0],(uint32_t)args[c][1]);}
        else vkCmdDraw(cmd,3,2,(uint32_t)args[c][0],(uint32_t)args[c][1]);
        vkCmdEndRenderPass(cmd);VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
        VK_OK(vkEndCommandBuffer(cmd));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};VK_OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VK_OK(vkQueueWaitIdle(queue));
        const int32_t *actual=mapped;for(unsigned i=0;i<6;i++)for(unsigned j=0;j<5;j++){
            int32_t expected=j<3?args[c][j]:j==3?first+(int32_t)(i%3):args[c][1]+(int32_t)(i/3);
            if(actual[i*5+j]!=expected){fprintf(stderr,"case=%u slot=%u field=%u got=%d expected=%d\n",c,i,j,actual[i*5+j],expected);return 7;}
        }
        for(unsigned i=30;i<256;i++)if((uint32_t)actual[i]!=0xcdcdcdcd)return 8;
        vkDestroyPipeline(dev,pipeline,NULL);
    }
    printf("{\"passed\":true,\"fixture_only\":true,\"mode\":\"%s\",\"gpu\":\"%s\",\"cases\":3,\"checked_values\":90,\"negative_base_vertex\":true,\"nonzero_base_instance\":true}\n",argv[2],props.deviceName);
    vkDestroyCommandPool(dev,cp,NULL);vkDestroyShaderModule(dev,fragment,NULL);vkDestroyShaderModule(dev,shader,NULL);vkDestroyFramebuffer(dev,fb,NULL);vkDestroyRenderPass(dev,rp,NULL);vkDestroyPipelineLayout(dev,pl,NULL);vkDestroyDescriptorPool(dev,dp,NULL);vkDestroyDescriptorSetLayout(dev,layout,NULL);vkUnmapMemory(dev,memory);vkDestroyBuffer(dev,buffer,NULL);vkFreeMemory(dev,memory,NULL);vkDestroyDevice(dev,NULL);vkDestroyInstance(instance,NULL);return 0;
}
