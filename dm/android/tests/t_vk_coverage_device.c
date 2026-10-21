/* Positive controls on a real Vulkan device. Each format clears only mip 1,
 * layer 1, copies it, overwrites it, then copies again in the same submission.
 * Checks the production sentinel encoding, isolation and exact comparison. */
#include "../../gfx/nvn/nvn_vk_coverage.h"
#include "../render_budget.h"
#include <stdio.h>
#include <stdlib.h>
#define OK(x) do {VkResult r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"%s: %d\n",#x,r);return 3;}}while(0)
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"failed: %s line %d\n",#x,__LINE__);return 4;}}while(0)
typedef struct {VkImage image;VkFormat format;} VTexture;
typedef struct {VkBuffer b;} VBuffer;
static struct {uint64_t pass_count;} G;
static struct {VkCommandBuffer command;unsigned ncoverage;struct {
    VTexture *texture;VBuffer *buffer;uint64_t pass;uint32_t w,h,mip,layer;VkImageLayout layout;
} coverage[1];} S;
static int g_cov_selftest;
static void die(const char *message,long long value) {fprintf(stderr,"%s %lld\n",message,value);abort();}
#include "../../gfx/nvn/nvn_vk_coverage_readback.inc"
static uint32_t memory_type(VkPhysicalDevice gpu,uint32_t bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(gpu,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;
    abort();
}
int main(void) {
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="coverage positive controls",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance;OK(vkCreateInstance(&ii,NULL,&instance));
    uint32_t n=1;VkPhysicalDevice gpu;OK(vkEnumeratePhysicalDevices(instance,&n,&gpu));REQUIRE(n==1);
    uint32_t nf=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&nf,NULL);VkQueueFamilyProperties *q=calloc(nf,sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&nf,q);uint32_t family=0;while(family<nf&&!(q[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))family++;
    free(q);REQUIRE(family<nf);float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice device;OK(vkCreateDevice(gpu,&di,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    VkCommandPool pool;OK(vkCreateCommandPool(device,&pi,NULL,&pool));
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd;OK(vkAllocateCommandBuffers(device,&ai,&cmd));
    VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R32_UINT,VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    for(unsigned f=0;f<sizeof formats/sizeof *formats;f++){
        VkClearValue sentinel;uint32_t word;REQUIRE(nvn_cov_sentinel(formats[f],&sentinel,&word));
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=formats[f],.extent={4,4,1},.mipLevels=2,.arrayLayers=2,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        VkImage image;OK(vkCreateImage(device,&ci,NULL,&image));VkMemoryRequirements mr;vkGetImageMemoryRequirements(device,image,&mr);
        VkMemoryAllocateInfo mi={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=memory_type(gpu,mr.memoryTypeBits,0)};
        VkDeviceMemory im;OK(vkAllocateMemory(device,&mi,NULL,&im));OK(vkBindImageMemory(device,image,im,0));
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=32,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        VkBuffer buffer;OK(vkCreateBuffer(device,&bi,NULL,&buffer));vkGetBufferMemoryRequirements(device,buffer,&mr);
        mi.allocationSize=mr.size;mi.memoryTypeIndex=memory_type(gpu,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory bm;OK(vkAllocateMemory(device,&mi,NULL,&bm));OK(vkBindBufferMemory(device,buffer,bm,0));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(vkBeginCommandBuffer(cmd,&begin));
        VkImageSubresourceRange all={VK_IMAGE_ASPECT_COLOR_BIT,0,2,0,2},target={VK_IMAGE_ASPECT_COLOR_BIT,1,1,1,1};
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=all};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkClearColorValue zero={0};vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&all);
        VkMemoryBarrier mem={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mem,0,NULL,0,NULL);
        vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&sentinel.color,1,&target);
        VkBufferImageCopy cp={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,1,1,1},.imageExtent={2,2,1}};
        for(unsigned round=0;round<2;round++){
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mem,0,NULL,0,NULL);
            if(!round){
                VTexture texture={image,formats[f]};VBuffer readback={buffer};
                S.command=cmd;S.ncoverage=1;G.pass_count=1;
                S.coverage[0].texture=&texture;S.coverage[0].buffer=&readback;S.coverage[0].pass=0;
                S.coverage[0].w=S.coverage[0].h=2;S.coverage[0].mip=S.coverage[0].layer=1;S.coverage[0].layout=VK_IMAGE_LAYOUT_GENERAL;
                cov_probe_end();REQUIRE(S.coverage[0].texture==NULL);
            }else{
                /* The production hook must never overwrite the earlier snapshot. */
                G.pass_count=2;cov_probe_end();
                cp.bufferOffset=16;vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&cp);
            }
            if(!round){vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mem,0,NULL,0,NULL);vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&target);}
        }
        mem.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mem.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mem,0,NULL,0,NULL);
        OK(vkEndCommandBuffer(cmd));VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
        OK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));OK(vkQueueWaitIdle(queue));
        uint8_t *data;OK(vkMapMemory(device,bm,0,32,0,(void **)&data));
        REQUIRE(nvn_cov_uncovered(data,4,word)==4);REQUIRE(nvn_cov_uncovered(data+16,4,word)==0);
        printf("format=%u mip=1 layer=1 before=4 after=0 PASS\n",formats[f]);vkUnmapMemory(device,bm);
        OK(vkResetCommandBuffer(cmd,0));vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,bm,NULL);vkDestroyImage(device,image,NULL);vkFreeMemory(device,im,NULL);
    }
    VkClearValue clear;uint32_t word;REQUIRE(!nvn_cov_sentinel(VK_FORMAT_R16_SFLOAT,&clear,&word));
    uint64_t res=dm_android_tier_resolution(2624,1220,1);REQUIRE((uint32_t)(res>>32)==1080);REQUIRE((uint32_t)res==2323);
    REQUIRE(dm_android_world_scale(2323,1080,1)==.70f);REQUIRE(dm_android_tier_resolution(1280,720,0)==((uint64_t)720<<32|1280));
    vkDestroyCommandPool(device,pool,NULL);vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);puts("coverage/budget positive controls PASS");return 0;
}
