/* 合成 subgroup helper 经生产内联器后在真实 Vulkan 驱动计算，输出 32 个活跃 lane 的和。
 * A synthetic subgroup helper runs through the production inliner and real Vulkan driver, writing the sum of 32 active lanes. */
#include "../../gfx/nvn/nvn_vk_inline.h"
#include <spirv-tools/libspirv.hpp>
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define VK_CHECK(x) do{VkResult result=(x);if(result){fprintf(stderr,"FAIL %s result=%d line=%d\n",#x,result,__LINE__);abort();}}while(0)
int main(){
    const char *source=R"(
OpCapability Shader
OpCapability GroupNonUniform
OpCapability GroupNonUniformArithmetic
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %gid
OpExecutionMode %main LocalSize 32 1 1
OpDecorate %gid BuiltIn GlobalInvocationId
OpDecorate %array ArrayStride 4
OpMemberDecorate %block 0 Offset 0
OpDecorate %block Block
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%v3 = OpTypeVector %uint 3
%ptr_in = OpTypePointer Input %v3
%gid = OpVariable %ptr_in Input
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%scope = OpConstant %uint 3
%length = OpConstant %uint 32
%array = OpTypeArray %uint %length
%block = OpTypeStruct %array
%ptr_block = OpTypePointer StorageBuffer %block
%ptr_uint = OpTypePointer StorageBuffer %uint
%output = OpVariable %ptr_block StorageBuffer
%helper_fn = OpTypeFunction %uint
%helper = OpFunction %uint None %helper_fn
%h = OpLabel
%sum = OpGroupNonUniformIAdd %uint %scope Reduce %one
OpReturnValue %sum
OpFunctionEnd
%main = OpFunction %void None %fn
%m = OpLabel
%xyz = OpLoad %v3 %gid
%index = OpCompositeExtract %uint %xyz 0
%value = OpFunctionCall %uint %helper
%address = OpAccessChain %ptr_uint %output %zero %index
OpStore %address %value
OpReturn
OpFunctionEnd
)";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);std::vector<uint32_t> input;
    assert(tools.Assemble(source,&input)&&tools.Validate(input));uint32_t *words=nullptr;size_t count=0;
    assert(dm_vk_inline_helpers(input.data(),input.size(),&words,&count)==1&&tools.Validate(words,count));
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;VkInstance instance;VK_CHECK(vkCreateInstance(&ici,nullptr,&instance));
    uint32_t n=0;VK_CHECK(vkEnumeratePhysicalDevices(instance,&n,nullptr));assert(n);std::vector<VkPhysicalDevice> physical(n);VK_CHECK(vkEnumeratePhysicalDevices(instance,&n,physical.data()));
    VkPhysicalDevice gpu=physical[0];VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(gpu,&properties);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,families.data());
    uint32_t family=0;while(family<n&&!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;assert(family<n);
    float priority=1;VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;VkDevice device;VK_CHECK(vkCreateDevice(gpu,&dci,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=128;bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;VkBuffer buffer;VK_CHECK(vkCreateBuffer(device,&bci,nullptr,&buffer));
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(gpu,&memory);uint32_t type=0;
    for(;type<memory.memoryTypeCount;type++)if((req.memoryTypeBits&(1u<<type))&&(memory.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;assert(type<memory.memoryTypeCount);
    VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};mai.allocationSize=req.size;mai.memoryTypeIndex=type;VkDeviceMemory allocation;VK_CHECK(vkAllocateMemory(device,&mai,nullptr,&allocation));VK_CHECK(vkBindBufferMemory(device,buffer,allocation,0));void *mapped;VK_CHECK(vkMapMemory(device,allocation,0,VK_WHOLE_SIZE,0,&mapped));memset(mapped,0,128);
    VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};VkDescriptorSetLayoutCreateInfo dlci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dlci.bindingCount=1;dlci.pBindings=&binding;VkDescriptorSetLayout layout;VK_CHECK(vkCreateDescriptorSetLayout(device,&dlci,nullptr,&layout));
    VkPipelineLayoutCreateInfo plci={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&layout;VkPipelineLayout pipeline_layout;VK_CHECK(vkCreatePipelineLayout(device,&plci,nullptr,&pipeline_layout));
    VkShaderModuleCreateInfo smci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=count*4;smci.pCode=words;VkShaderModule module;VK_CHECK(vkCreateShaderModule(device,&smci,nullptr,&module));
    VkComputePipelineCreateInfo cpci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cpci.layout=pipeline_layout;cpci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};VkPipeline pipeline;VK_CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline));
    VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo dpci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=1;dpci.pPoolSizes=&pool_size;VkDescriptorPool pool;VK_CHECK(vkCreateDescriptorPool(device,&dpci,nullptr,&pool));
    VkDescriptorSetAllocateInfo dsai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsai.descriptorPool=pool;dsai.descriptorSetCount=1;dsai.pSetLayouts=&layout;VkDescriptorSet set;VK_CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
    VkDescriptorBufferInfo dbi={buffer,0,128};VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&dbi;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    VkCommandPoolCreateInfo cpi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpi.queueFamilyIndex=family;VkCommandPool commands;VK_CHECK(vkCreateCommandPool(device,&cpi,nullptr,&commands));VkCommandBufferAllocateInfo cbai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbai.commandPool=commands;cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cbai.commandBufferCount=1;VkCommandBuffer command;VK_CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
    VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_CHECK(vkBeginCommandBuffer(command,&begin));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);vkCmdDispatch(command,1,1,1);
    VkMemoryBarrier barrier={VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);VK_CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;VkFenceCreateInfo fci={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;VK_CHECK(vkCreateFence(device,&fci,nullptr,&fence));VK_CHECK(vkQueueSubmit(queue,1,&submit,fence));VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull));
    for(unsigned i=0;i<32;i++)assert(((uint32_t*)mapped)[i]==32);
    printf("INLINE_DEVICE PASS gpu=%s vendor=%x driver=%u lanes=32 helper_removed=1 values=32\n",properties.deviceName,properties.vendorID,properties.driverVersion);
    vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,commands,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,module,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyDescriptorSetLayout(device,layout,nullptr);vkUnmapMemory(device,allocation);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,allocation,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);free(words);
}
