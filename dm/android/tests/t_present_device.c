/* Standalone Android Vulkan presentation-transfer semantics fixture.
 * No shaders, surface, APK, game assets or device-specific extensions.
 * A raw encoded sRGB source distinguishes a format conversion from a bit copy;
 * asymmetric markers also distinguish a Y flip from an X flip or rotation.
 */
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_OK(call) do { VkResult r_=(call); if(r_!=VK_SUCCESS) { \
    fprintf(stderr,"%s: %d\n",#call,r_); exit(3); } } while(0)
#define BUFFER_BYTES 4096u
#define RESULT_BASE 512u
#define RESULT_STRIDE 256u

typedef struct {VkImage image;VkDeviceMemory memory;VkImageLayout layout;} Image;
typedef struct {const char *name;unsigned source,target,x,y,width,height,flip_y,decode;} Case;
static const Case cases[]={
    {"srgb_to_unorm_decode",0,2,0,0,4,4,0,1},
    {"srgb_to_srgb_encoded",0,3,0,0,4,4,0,0},
    {"unorm_bridge_encoded",1,2,0,0,4,4,0,0},
    {"srgb_crop_y_flip",0,3,1,1,2,2,1,0},
    {"bridge_crop_y_flip",1,2,1,1,2,2,1,0},
    {"srgb_full_y_flip",0,3,0,0,4,4,1,0},
    {"srgb_unorm_crop_y_flip_decode",0,2,1,1,2,2,1,1}
};
#define CASE_COUNT ((unsigned)(sizeof(cases)/sizeof(cases[0])))

static uint32_t memory_type(VkPhysicalDevice gpu,uint32_t bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(gpu,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)
        if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;
    fputs("required Vulkan memory type unavailable\n",stderr);exit(6);
}
static Image make_image(VkPhysicalDevice gpu,VkDevice device,VkFormat format) {
    Image out={0};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=format,.extent={4,4,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VK_OK(vkCreateImage(device,&ci,NULL,&out.image));
    VkMemoryRequirements req;vkGetImageMemoryRequirements(device,out.image,&req);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(gpu,req.memoryTypeBits,0)};
    VK_OK(vkAllocateMemory(device,&ma,NULL,&out.memory));
    VK_OK(vkBindImageMemory(device,out.image,out.memory,0));return out;
}
static void transition(VkCommandBuffer command,Image *image,VkImageLayout to) {
    VkAccessFlags from_access=image->layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL?VK_ACCESS_TRANSFER_WRITE_BIT:
        image->layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL?VK_ACCESS_TRANSFER_READ_BIT:0;
    VkAccessFlags to_access=to==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=from_access,
        .dstAccessMask=to_access,.oldLayout=image->layout,.newLayout=to,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,image->layout==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    image->layout=to;
}
static VkBufferImageCopy buffer_region(VkDeviceSize offset) {
    VkBufferImageCopy c={.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                         .imageExtent={4,4,1}};return c;
}
static unsigned decode(unsigned encoded) {
    double value=encoded/255.0;
    return (unsigned)lround(255.0*(value<=0.04045?value/12.92:pow((value+0.055)/1.055,2.4)));
}
static void assert_byte(unsigned actual,unsigned expected,unsigned tolerance,const char *name,unsigned byte) {
    unsigned delta=actual>expected?actual-expected:expected-actual;
    if(delta>tolerance){fprintf(stderr,"present case=%s byte=%u actual=%u expected=%u tolerance=%u\n",
        name,byte,actual,expected,tolerance);exit(7);}
}

int main(void) {
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Presentation transfer semantics",
        .apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance;VK_OK(vkCreateInstance(&ii,NULL,&instance));
    uint32_t gpu_count=0;VK_OK(vkEnumeratePhysicalDevices(instance,&gpu_count,NULL));if(!gpu_count)return 5;
    VkPhysicalDevice *gpus=calloc(gpu_count,sizeof(*gpus));if(!gpus)return 5;
    VK_OK(vkEnumeratePhysicalDevices(instance,&gpu_count,gpus));VkPhysicalDevice gpu=gpus[0];free(gpus);
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(gpu,&properties);
    const VkFormat formats[2]={VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_R8G8B8A8_UNORM};
    for(unsigned i=0;i<2;i++) {
        VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(gpu,formats[i],&fp);
        VkFormatFeatureFlags need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if((fp.optimalTilingFeatures&need)!=need){fprintf(stderr,"format=%d missing transfer/blit features=0x%x\n",
            formats[i],fp.optimalTilingFeatures);return 6;}
    }
    uint32_t family_count=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&family_count,NULL);
    VkQueueFamilyProperties *qp=calloc(family_count,sizeof(*qp));if(!qp)return 6;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&family_count,qp);uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<family_count;i++)if(qp[i].queueCount&&(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}
    free(qp);if(family==UINT32_MAX)return 6;
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,
        .queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice device;VK_OK(vkCreateDevice(gpu,&di,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=BUFFER_BYTES,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;VK_OK(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory memory;VK_OK(vkAllocateMemory(device,&ma,NULL,&memory));VK_OK(vkBindBufferMemory(device,buffer,memory,0));
    uint8_t *mapped;VK_OK(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,(void **)&mapped));
    memset(mapped,0xcd,BUFFER_BYTES);
    uint8_t source[64];
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++) {
        unsigned p=(y*4+x)*4;source[p]=source[p+1]=source[p+2]=(uint8_t)(40+12*x+16*y);source[p+3]=255;
    }
    const uint8_t markers[4][4]={{32,64,96,255},{64,128,192,255},{96,160,224,255},{224,192,128,255}};
    const unsigned corners[4]={0,3,12,15};
    for(unsigned i=0;i<4;i++)memcpy(source+corners[i]*4,markers[i],4);
    const uint8_t grays[4]={64,128,192,224};const unsigned middle[4]={5,6,9,10};
    for(unsigned i=0;i<4;i++)for(unsigned c=0;c<3;c++)source[middle[i]*4+c]=grays[i];
    memcpy(mapped,source,sizeof(source));
    Image images[4]={make_image(gpu,device,formats[0]),make_image(gpu,device,formats[1]),
                     make_image(gpu,device,formats[1]),make_image(gpu,device,formats[0])};
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool pool;VK_OK(vkCreateCommandPool(device,&cpi,NULL,&pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;VK_OK(vkAllocateCommandBuffers(device,&cai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_OK(vkBeginCommandBuffer(command,&begin));
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffer,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&host,0,NULL);
    transition(command,&images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkBufferImageCopy upload=buffer_region(0);
    vkCmdCopyBufferToImage(command,buffer,images[0].image,images[0].layout,1,&upload);
    transition(command,&images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(command,&images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={4,4,1}};
    vkCmdCopyImage(command,images[0].image,images[0].layout,images[1].image,images[1].layout,1,&copy);
    transition(command,&images[1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy read=buffer_region(RESULT_BASE);
    vkCmdCopyImageToBuffer(command,images[1].image,images[1].layout,buffer,1,&read);
    for(unsigned i=0;i<CASE_COUNT;i++) {
        const Case *c=&cases[i];Image *src=&images[c->source],*dst=&images[c->target];
        transition(command,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        /* Raw sentinel pixels protect the destination outside the 2x2 crop. */
        VkBufferImageCopy sentinel=buffer_region(128);
        vkCmdCopyBufferToImage(command,buffer,dst->image,dst->layout,1,&sentinel);
        transition(command,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .srcOffsets={{(int32_t)c->x,(int32_t)(c->y+(c->flip_y?c->height:0)),0},
                         {(int32_t)(c->x+c->width),(int32_t)(c->y+(c->flip_y?0:c->height)),1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstOffsets={{0,0,0},{(int32_t)c->width,(int32_t)c->height,1}}};
        vkCmdBlitImage(command,src->image,src->layout,dst->image,dst->layout,1,&blit,VK_FILTER_NEAREST);
        transition(command,dst,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        read=buffer_region(RESULT_BASE+(i+1)*RESULT_STRIDE);
        vkCmdCopyImageToBuffer(command,dst->image,dst->layout,buffer,1,&read);
    }
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    VK_OK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    VK_OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VK_OK(vkQueueWaitIdle(queue));
    for(unsigned i=0;i<64;i++) {
        assert_byte(mapped[i],source[i],0,"upload_unchanged",i);
        assert_byte(mapped[RESULT_BASE+i],source[i],0,"srgb_unorm_copy_bit_identity",i);
    }
    unsigned image_guard_bytes=0,guard_bytes=0;
    for(unsigned i=0;i<CASE_COUNT;i++) {
        const Case *c=&cases[i];const uint8_t *actual=mapped+RESULT_BASE+(i+1)*RESULT_STRIDE;
        for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)for(unsigned channel=0;channel<4;channel++) {
            unsigned byte=(y*4+x)*4+channel,expected=0xcd,tolerance=0;
            if(x<c->width&&y<c->height) {
                unsigned sx=c->x+x,sy=c->y+(c->flip_y?c->height-1-y:y);
                expected=source[(sy*4+sx)*4+channel];
                if(c->decode&&channel<3)expected=decode(expected);
                tolerance=channel<3?1:0;
            } else image_guard_bytes++;
            assert_byte(actual[byte],expected,tolerance,c->name,byte);
        }
    }
    for(unsigned i=64;i<BUFFER_BYTES;i++) {
        unsigned result=i>=RESULT_BASE?(i-RESULT_BASE)/RESULT_STRIDE:UINT32_MAX;
        if(result<=CASE_COUNT&&(i-RESULT_BASE)%RESULT_STRIDE<64)continue;
        assert_byte(mapped[i],0xcd,0,"readback_buffer_guard",i);guard_bytes++;
    }
    printf("{\"passed\":true,\"fixture_only\":true,\"gpu\":\"%s\",\"shaderless\":true,\"surface_required\":false,"
        "\"source_extent\":[4,4],\"nonzero_crop\":[1,1,2,2],\"blit_cases\":%u,\"copy_bit_identity\":true,"
        "\"srgb_decode_gray128\":%u,\"srgb_preserve_gray128\":%u,\"bridge_preserve_gray128\":%u,"
        "\"y_flip_only\":true,\"checked_image_guard_bytes\":%u,\"checked_buffer_guard_bytes\":%u,\"rgb_tolerance_lsb\":1}\n",
        properties.deviceName,CASE_COUNT,mapped[RESULT_BASE+RESULT_STRIDE+6*4],
        mapped[RESULT_BASE+2*RESULT_STRIDE+6*4],mapped[RESULT_BASE+3*RESULT_STRIDE+6*4],image_guard_bytes,guard_bytes);
    vkDestroyCommandPool(device,pool,NULL);
    for(unsigned i=0;i<4;i++){vkDestroyImage(device,images[i].image,NULL);vkFreeMemory(device,images[i].memory,NULL);}
    vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);return 0;
}
