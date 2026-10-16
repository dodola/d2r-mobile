/* Reproduce build-91738 fluid pipeline compilation on the system Vulkan driver.
 * Does not dispatch a shader or open/modify application resources or saves. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../gfx/nvn/nvn_vk_storage_bias.h"
#include "../../gfx/nvn/nvn_vk_ballot.h"
#define CHECK(call) do { VkResult r=(call);if(r){fprintf(stderr,"%s: %d\n",#call,r);return 2;} } while(0)
int main(int argc,char **argv) {
    if(argc<3||argc>4)return 1;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="D2R fluid compiler reproduction",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance;CHECK(vkCreateInstance(&ic,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical;CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
    VkPhysicalDeviceSubgroupProperties subgroup={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,.pNext=&subgroup};
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver};vkGetPhysicalDeviceProperties2(physical,&props);
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(physical,&features);
    uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,NULL);VkQueueFamilyProperties *queues=calloc(nq,sizeof(*queues));
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&nq,queues);uint32_t family=0;
    while(family<nq&&!(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;
    if(family==nq)return 3;
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&features};
    VkDevice device;CHECK(vkCreateDevice(physical,&dc,NULL,&device));
    FILE *file=fopen(argv[1],"rb");if(!file)return 4;fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);
    if(length<20||length%4)return 5;
    size_t bytes=(size_t)length;uint8_t *data=malloc(bytes);if(fread(data,1,bytes,file)!=bytes)return 6;fclose(file);
    unsigned replaced=0;
    if(argc==4){data=dm_compute_ballot_apply(data,&bytes,&replaced);if(!data)return 9;
        FILE *out=fopen(argv[3],"wb");if(!out||fwrite(data,1,bytes,out)!=bytes)return 10;fclose(out);}
    SBInfo info={0};
    if(atoi(argv[2])) {SBResource resources[]={{0,0},{1,1},{2,1},{3,0}};data=dm_storage_bias_apply(data,&bytes,resources,4,&info);if(!data)return 7;}
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=(uint32_t *)data};
    VkShaderModule module;CHECK(vkCreateShaderModule(device,&sm,NULL,&module));
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{3,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo sc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=4,.pBindings=bindings};
    VkDescriptorSetLayout set;CHECK(vkCreateDescriptorSetLayout(device,&sc,NULL,&set));
    VkPipelineLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set};
    VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&lc,NULL,&layout));
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
    uint32_t offsets[4]={0};VkSpecializationMapEntry entries[4];uint32_t n=0;
    for(unsigned i=0;i<4;i++)if(info.mask&(1ull<<i))entries[n++]=(VkSpecializationMapEntry){info.spec_id[i],i*4,4};
    VkSpecializationInfo spec={n,entries,sizeof(offsets),offsets};if(n)pc.stage.pSpecializationInfo=&spec;
    VkPipeline pipeline;VkResult result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,NULL,&pipeline);
    printf("{\"device\":\"%s\",\"driver_id\":%u,\"subgroup_size\":%u,\"shader_bytes\":%zu,\"storage_bias_mask\":%llu,\"pipeline_result\":%d}\n",props.properties.deviceName,driver.driverID,subgroup.subgroupSize,bytes,(unsigned long long)info.mask,result);
    if(!result)vkDestroyPipeline(device,pipeline,NULL);
    vkDestroyPipelineLayout(device,layout,NULL);vkDestroyDescriptorSetLayout(device,set,NULL);vkDestroyShaderModule(device,module,NULL);
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);free(data);free(queues);return result?8:0;
}
