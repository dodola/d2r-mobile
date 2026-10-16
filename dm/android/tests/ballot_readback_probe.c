/* Standalone system-driver ballot readback. Args: lowered.spv output-prefix. */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r=(x);if(r){fprintf(stderr,"line %d VkResult %d\n",__LINE__,r);return 20;} }while(0)
#define I(name) PFN_vk##name name=(PFN_vk##name)get(instance,"vk"#name)
#define D(name) PFN_vk##name name=(PFN_vk##name)gd(device,"vk"#name)
int main(int argc,char **argv){
 if(argc!=3)return 2;
 void *library=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
 if(!library)return 3;
 PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)dlsym(library,"vkGetInstanceProcAddr");
 PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(NULL,"vkCreateInstance");
 VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="D2R warp32 standalone",.apiVersion=VK_API_VERSION_1_1};
 VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};VkInstance instance;CHECK(create(&ic,NULL,&instance));
 I(EnumeratePhysicalDevices);I(GetPhysicalDeviceProperties2);I(GetPhysicalDeviceMemoryProperties);I(GetPhysicalDeviceQueueFamilyProperties);I(CreateDevice);I(DestroyInstance);
 uint32_t count=1;VkPhysicalDevice physical;CHECK(EnumeratePhysicalDevices(instance,&count,&physical));
 VkPhysicalDeviceSubgroupProperties subgroup={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
 VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,.pNext=&subgroup};
 VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver};GetPhysicalDeviceProperties2(physical,&props);
 uint32_t n=0;GetPhysicalDeviceQueueFamilyProperties(physical,&n,NULL);VkQueueFamilyProperties *q=calloc(n,sizeof *q);GetPhysicalDeviceQueueFamilyProperties(physical,&n,q);uint32_t family=0;for(;family<n;family++)if(q[family].queueFlags&VK_QUEUE_COMPUTE_BIT)break;if(family==n)return 4;
 float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};VkDevice device;CHECK(CreateDevice(physical,&dc,NULL,&device));
 PFN_vkGetDeviceProcAddr gd=(PFN_vkGetDeviceProcAddr)get(instance,"vkGetDeviceProcAddr");
 D(CreateBuffer);D(GetBufferMemoryRequirements);D(AllocateMemory);D(BindBufferMemory);D(MapMemory);D(UnmapMemory);D(FlushMappedMemoryRanges);D(InvalidateMappedMemoryRanges);D(CreateDescriptorSetLayout);D(CreatePipelineLayout);D(CreateShaderModule);D(CreateComputePipelines);D(CreateDescriptorPool);D(AllocateDescriptorSets);D(UpdateDescriptorSets);D(CreateCommandPool);D(AllocateCommandBuffers);D(BeginCommandBuffer);D(CmdBindPipeline);D(CmdBindDescriptorSets);D(CmdDispatch);D(CmdPipelineBarrier);D(EndCommandBuffer);D(GetDeviceQueue);D(QueueSubmit);D(QueueWaitIdle);D(DestroyDevice);
 enum{WORDS=256*64+16};size_t bytes=WORDS*4;VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};VkBuffer buffer;CHECK(CreateBuffer(device,&bc,NULL,&buffer));VkMemoryRequirements req;GetBufferMemoryRequirements(device,buffer,&req);VkPhysicalDeviceMemoryProperties memory;GetPhysicalDeviceMemoryProperties(physical,&memory);uint32_t type=0;for(;type<memory.memoryTypeCount;type++)if((req.memoryTypeBits&(1u<<type))&&(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))break;if(type==memory.memoryTypeCount)return 5;
 VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};VkDeviceMemory allocation;CHECK(AllocateMemory(device,&ma,NULL,&allocation));CHECK(BindBufferMemory(device,buffer,allocation,0));uint32_t *mapped;CHECK(MapMemory(device,allocation,0,VK_WHOLE_SIZE,0,(void**)&mapped));
 VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};VkDescriptorSetLayoutCreateInfo sc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};VkDescriptorSetLayout set_layout;CHECK(CreateDescriptorSetLayout(device,&sc,NULL,&set_layout));VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};VkPipelineLayout layout;CHECK(CreatePipelineLayout(device,&lc,NULL,&layout));
 VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};VkDescriptorPool pool;CHECK(CreateDescriptorPool(device,&pc,NULL,&pool));VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};VkDescriptorSet set;CHECK(AllocateDescriptorSets(device,&da,&set));VkDescriptorBufferInfo bi={buffer,0,bytes};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bi};UpdateDescriptorSets(device,1,&write,0,NULL);
 VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};VkCommandPool commands;CHECK(CreateCommandPool(device,&cp,NULL,&commands));VkQueue queue;GetDeviceQueue(device,family,0,&queue);VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=allocation,.offset=0,.size=VK_WHOLE_SIZE};
 for(unsigned which=1;which<2;which++){
  FILE *sp=fopen(argv[1],"rb");if(!sp)return 6;fseek(sp,0,SEEK_END);long length=ftell(sp);rewind(sp);uint32_t *words=malloc(length);if(fread(words,1,length,sp)!=(size_t)length)return 7;fclose(sp);VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=length,.pCode=words};VkShaderModule module;CHECK(CreateShaderModule(device,&sm,NULL,&module));VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=layout};VkPipeline pipeline;CHECK(CreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
  for(size_t j=0;j<WORDS;j++)mapped[j]=0xdeadbeef;CHECK(FlushMappedMemoryRanges(device,1,&range));VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(AllocateCommandBuffers(device,&ca,&command));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(BeginCommandBuffer(command,&begin));CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);CmdDispatch(command,2,1,1);VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);CHECK(EndCommandBuffer(command));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};CHECK(QueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(QueueWaitIdle(queue));CHECK(InvalidateMappedMemoryRanges(device,1,&range));char filename[1024];snprintf(filename,sizeof filename,"%s.%s.bin",argv[2],which?"lowered":"original");FILE *output=fopen(filename,"wb");if(!output||fwrite(mapped,1,bytes,output)!=bytes)return 8;fclose(output);free(words);
 }
 printf("{\"device\":\"%s\",\"driver_id\":%u,\"subgroup_size\":%u,\"groups\":2,\"output_words\":%u}\n",props.properties.deviceName,driver.driverID,subgroup.subgroupSize,WORDS);UnmapMemory(device,allocation);DestroyDevice(device,NULL);DestroyInstance(instance,NULL);free(q);return 0;
}
