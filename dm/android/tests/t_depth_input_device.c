/* Standalone native ARM depth-input ordering fixture. No runtime/game/ADB hooks.
 * Three overlapping primitives in ONE draw perform genuine depth writes:
 *   .875 -> .4375 -> .34375 -> .421875.
 * The final color records the last fragment's prior depth and primitive index.
 * A snapshot, disabled writes, or any other primitive permutation is different.
 * Shader contract negatives are rejected before Vulkan initialization. */
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../gfx/nvn/nvn_vk_image_type.h"

#define VK_OK(call) do { VkResult r_=(call); if(r_!=VK_SUCCESS) { \
    fprintf(stderr,"%s: %d\n",#call,r_); exit(3); } } while(0)
#define WIDTH 8u
#define HEIGHT 6u
#define PIXELS (WIDTH*HEIGHT)
#define BUFFER_BYTES 4096u
#define COLOR_OFFSET 0u
#define DEPTH_OFFSET 1024u
#define STENCIL_OFFSET 1280u

typedef struct {VkImage image;VkDeviceMemory memory;VkImageView attachment,input;} Image;
static unsigned validation_errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)validation_errors++;
    fprintf(stderr,"[depth-input-validation] %s\n",data->pMessage);return VK_FALSE;
}
static int reject(const char *reason) {
    fprintf(stderr,"DEPTH_INPUT_REJECT reason=%s gpu_commands_issued=0\n",reason);
    printf("{\"passed\":false,\"fixture_only\":true,\"rejected\":true,\"reason\":\"%s\",\"gpu_commands_issued\":0}\n",reason);
    return 4;
}
static uint32_t *read_spv(const char *path,size_t *words) {
    FILE *file=fopen(path,"rb");if(!file)return NULL;
    if(fseek(file,0,SEEK_END)){fclose(file);return NULL;}
    long bytes=ftell(file);rewind(file);
    if(bytes<20||bytes%4){fclose(file);return NULL;}
    uint32_t *code=malloc((size_t)bytes);if(!code){fclose(file);return NULL;}
    if(fread(code,1,(size_t)bytes,file)!=(size_t)bytes){free(code);fclose(file);return NULL;}
    fclose(file);*words=(size_t)bytes/4;
    if(code[0]!=0x07230203u){free(code);return NULL;}return code;
}
static int zero_coordinate(const uint32_t *code,size_t words,uint32_t id) {
    const uint32_t *vector=NULL;
    for(size_t at=5;at<words;at+=code[at]>>16)
        if((code[at]&65535u)==44&&code[at]>>16==5&&code[at+2]==id)vector=code+at;
    if(!vector)return 0;
    for(unsigned c=3;c<5;c++){
        int zero=0;
        for(size_t at=5;at<words;at+=code[at]>>16)
            if((code[at]&65535u)==43&&code[at]>>16==4&&code[at+2]==vector[c]&&code[at+3]==0)zero=1;
        if(!zero)return 0;
    }
    return 1;
}
static const char *input_shader_contract(const uint32_t *code,size_t words) {
    NvnVkImageType types[NVN_VK_IMAGE_TYPE_MAX];size_t count=0;const char *error=NULL;
    if(!nvn_vk_image_types(code,words,types,&count,&error))return "image-reflection-unsupported";
    if(count!=1||types[0].dim!=6||types[0].sampled!=2||types[0].arrayed||types[0].multisampled||
       types[0].descriptor_set||types[0].binding)return "ordinary-sampled-or-nonlocal-image";
    unsigned reads=0,index_decorations=0;
    for(size_t at=5;at<words;at+=code[at]>>16){
        const uint32_t *a=code+at;uint32_t op=a[0]&65535u,n=a[0]>>16;
        if(op==16&&n>=3&&a[2]==9)return "EarlyFragmentTests-forbidden-with-ARM-depth-order";
        if(op==71&&n==4&&a[1]==types[0].variable_id&&a[2]==43){
            if(a[3])return "input-attachment-index-not-zero";index_decorations++;
        }
        if((op>=87&&op<=97)||(op>=305&&op<=315)||op==99||op==320)
            return "sample-fetch-gather-sparse-or-image-write-not-input-read";
        if(op==98){
            if(n!=5||!zero_coordinate(code,words,a[4]))return "neighbor-or-nonconstant-input-coordinate";
            reads++;
        }
    }
    return reads==1&&index_decorations==1?NULL:"expected-one-depth-input-read";
}
static uint32_t memory_type(VkPhysicalDevice gpu,uint32_t bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(gpu,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)
        if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;
    fputs("required memory type unavailable\n",stderr);exit(6);
}
static Image make_image(VkPhysicalDevice gpu,VkDevice device,VkFormat format,VkImageUsageFlags usage,VkImageAspectFlags aspects) {
    Image out={0};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,
        .extent={WIDTH,HEIGHT,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkImageFormatProperties supported;
    VK_OK(vkGetPhysicalDeviceImageFormatProperties(gpu,format,ci.imageType,ci.tiling,usage,0,&supported));
    VK_OK(vkCreateImage(device,&ci,NULL,&out.image));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,out.image,&req);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(gpu,req.memoryTypeBits,0)};
    VK_OK(vkAllocateMemory(device,&ma,NULL,&out.memory));VK_OK(vkBindImageMemory(device,out.image,out.memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=out.image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=format,.subresourceRange={aspects,0,1,0,1}};
    VK_OK(vkCreateImageView(device,&vi,NULL,&out.attachment));
    if(aspects&VK_IMAGE_ASPECT_DEPTH_BIT){vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;VK_OK(vkCreateImageView(device,&vi,NULL,&out.input));}
    return out;
}
static void transition(VkCommandBuffer command,VkImage image,VkImageAspectFlags aspects,VkImageLayout old,VkImageLayout to,
        VkPipelineStageFlags src,VkPipelineStageFlags dst,VkAccessFlags from_access,VkAccessFlags to_access) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=from_access,.dstAccessMask=to_access,
        .oldLayout=old,.newLayout=to,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image,.subresourceRange={aspects,0,1,0,1}};
    vkCmdPipelineBarrier(command,src,dst,0,0,NULL,0,NULL,1,&b);
}
static void check_float(float actual,float expected,float tolerance,const char *kind,unsigned index) {
    if(!isfinite(actual)||fabsf(actual-expected)>tolerance){
        fprintf(stderr,"depth-input %s index=%u got=%.9g expected=%.9g tolerance=%.9g\n",kind,index,actual,expected,tolerance);exit(7);
    }
}
int main(int argc,char **argv) {
    if(argc<3||argc>5){fprintf(stderr,"usage: %s vertex.spv fragment.spv [--d32] [--disable-depth-feature]\n",argv[0]);return 2;}
    int d32=0,force_disabled=0;
    for(int i=3;i<argc;i++){
        if(!strcmp(argv[i],"--d32"))d32=1;
        else if(!strcmp(argv[i],"--disable-depth-feature"))force_disabled=1;
        else return 2;
    }
    size_t shader_words[2];uint32_t *shader_code[2]={read_spv(argv[1],&shader_words[0]),read_spv(argv[2],&shader_words[1])};
    if(!shader_code[0]||!shader_code[1])return 2;
    const char *contract=input_shader_contract(shader_code[1],shader_words[1]);if(contract)return reject(contract);
    int validation=getenv("DM_VULKAN_VALIDATION")!=NULL;
    const char *layers[]={"VK_LAYER_KHRONOS_validation"},*instance_extensions[]={VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback=validation_message};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="ARM native depth input ordering",
        .apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,
        .enabledLayerCount=validation?1u:0u,.ppEnabledLayerNames=validation?layers:NULL,
        .enabledExtensionCount=validation?1u:0u,.ppEnabledExtensionNames=validation?instance_extensions:NULL,.pNext=validation?&debug:NULL};
    VkInstance instance;VK_OK(vkCreateInstance(&ici,NULL,&instance));VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    if(validation){PFN_vkCreateDebugUtilsMessengerEXT create=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");
        if(!create)return 6;VK_OK(create(instance,&debug,NULL,&messenger));}
    uint32_t count=0;VK_OK(vkEnumeratePhysicalDevices(instance,&count,NULL));if(!count)return 5;
    VkPhysicalDevice *gpus=calloc(count,sizeof(*gpus));if(!gpus)return 5;
    VK_OK(vkEnumeratePhysicalDevices(instance,&count,gpus));VkPhysicalDevice gpu=gpus[0];free(gpus);
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(gpu,&properties);
    VK_OK(vkEnumerateDeviceExtensionProperties(gpu,NULL,&count,NULL));VkExtensionProperties *ext=calloc(count,sizeof(*ext));if(!ext)return 6;
    VK_OK(vkEnumerateDeviceExtensionProperties(gpu,NULL,&count,ext));int has_arm=0,has_ext=0;
    for(uint32_t i=0;i<count;i++){
        has_arm|=!strcmp(ext[i].extensionName,VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);
        has_ext|=!strcmp(ext[i].extensionName,VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);
    }
    free(ext);if(!has_arm&&!has_ext)return reject("rasterization-order-extension-unavailable");
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesARM available={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_ARM};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&available};
    vkGetPhysicalDeviceFeatures2(gpu,&features);
    fprintf(stderr,"DEPTH_INPUT_FEATURES gpu=%s ARM=%d EXT=%d color=%u depth=%u stencil=%u subTexelPrecisionBits=%u subPixelPrecisionBits=%u\n",
        properties.deviceName,has_arm,has_ext,available.rasterizationOrderColorAttachmentAccess,available.rasterizationOrderDepthAttachmentAccess,
        available.rasterizationOrderStencilAttachmentAccess,properties.limits.subTexelPrecisionBits,properties.limits.subPixelPrecisionBits);
    if(!available.rasterizationOrderDepthAttachmentAccess||force_disabled)return reject(force_disabled?"depth-order-feature-forced-disabled":"depth-order-feature-unavailable");
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,NULL);
    VkQueueFamilyProperties *qp=calloc(families,sizeof(*qp));if(!qp)return 6;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&families,qp);uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<families;i++)if(qp[i].queueCount&&(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}
    free(qp);if(family==UINT32_MAX)return 6;
    const char *device_extension=has_arm?VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME:VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesARM enabled={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_ARM,
        .rasterizationOrderDepthAttachmentAccess=VK_TRUE};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&enabled,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=&device_extension};
    VkDevice device;VK_OK(vkCreateDevice(gpu,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkFormat depth_format=d32?VK_FORMAT_D32_SFLOAT:VK_FORMAT_D24_UNORM_S8_UINT;
    VkImageAspectFlags depth_aspects=VK_IMAGE_ASPECT_DEPTH_BIT|(d32?0:VK_IMAGE_ASPECT_STENCIL_BIT);
    Image images[2]={make_image(gpu,device,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT),
        make_image(gpu,device,depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,depth_aspects)};
    VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=BUFFER_BYTES,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;VK_OK(vkCreateBuffer(device,&bci,NULL,&buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffer,&req);
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory memory;VK_OK(vkAllocateMemory(device,&ma,NULL,&memory));VK_OK(vkBindBufferMemory(device,buffer,memory,0));
    uint8_t *mapped;VK_OK(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,(void **)&mapped));memset(mapped,0xcd,BUFFER_BYTES);
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    VkDescriptorSetLayout layout;VK_OK(vkCreateDescriptorSetLayout(device,&li,NULL,&layout));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1};VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    VkDescriptorPool descriptor_pool;VK_OK(vkCreateDescriptorPool(device,&dpi,NULL,&descriptor_pool));
    VkDescriptorSetAllocateInfo dai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set;VK_OK(vkAllocateDescriptorSets(device,&dai,&set));VkDescriptorImageInfo image_info={.imageView=images[1].input,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.pImageInfo=&image_info};vkUpdateDescriptorSets(device,1,&write,0,NULL);
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};
    VkPipelineLayout pipeline_layout;VK_OK(vkCreatePipelineLayout(device,&pli,NULL,&pipeline_layout));
    VkAttachmentDescription attachments[2]={
        {.format=VK_FORMAT_R32G32B32A32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format=depth_format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL}};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depth={1,VK_IMAGE_LAYOUT_GENERAL},input=depth;
    VkSubpassDescription sub={.flags=VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_ARM,.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount=1,.pInputAttachments=&input,.colorAttachmentCount=1,.pColorAttachments=&color,.pDepthStencilAttachment=&depth};
    VkInputAttachmentAspectReference aspect_reference={.subpass=0,.inputAttachmentIndex=0,.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT};
    VkRenderPassInputAttachmentAspectCreateInfo aspect_info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,.aspectReferenceCount=1,.pAspectReferences=&aspect_reference};
    VkSubpassDependency deps[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT}};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.pNext=&aspect_info,.attachmentCount=2,.pAttachments=attachments,
        .subpassCount=1,.pSubpasses=&sub,.dependencyCount=2,.pDependencies=deps};
    VkRenderPass render_pass;VK_OK(vkCreateRenderPass(device,&ri,NULL,&render_pass));
    VkImageView views[2]={images[0].attachment,images[1].attachment};VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=render_pass,.attachmentCount=2,.pAttachments=views,.width=WIDTH,.height=HEIGHT,.layers=1};
    VkFramebuffer framebuffer;VK_OK(vkCreateFramebuffer(device,&fi,NULL,&framebuffer));
    VkShaderModule modules[2];
    for(unsigned i=0;i<2;i++){VkShaderModuleCreateInfo si={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=shader_words[i]*4,.pCode=shader_code[i]};
        VK_OK(vkCreateShaderModule(device,&si,NULL,&modules[i]));free(shader_code[i]);}
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
    VkViewport viewport={0,0,WIDTH,HEIGHT,0,1};VkRect2D scissor={{1,1},{WIDTH-2,HEIGHT-2}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .flags=VK_PIPELINE_DEPTH_STENCIL_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_ARM,
        .depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=15};VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkGraphicsPipelineCreateInfo gi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,
        .pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pDepthStencilState=&ds,.pColorBlendState=&cb,.layout=pipeline_layout,.renderPass=render_pass};
    VkPipeline pipeline;VK_OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gi,NULL,&pipeline));
    VkCommandPoolCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};VkCommandPool command_pool;VK_OK(vkCreateCommandPool(device,&cpi,NULL,&command_pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;VK_OK(vkAllocateCommandBuffers(device,&cai,&command));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_OK(vkBeginCommandBuffer(command,&begin));
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffer,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&host,0,NULL);
    for(unsigned i=0;i<2;i++)transition(command,images[i].image,i?depth_aspects:VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange color_range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},depth_range={depth_aspects,0,1,0,1};
    const float sentinel[4]={-7,-11,-13,-17};VkClearColorValue clear_color={.float32={-7,-11,-13,-17}};
    VkClearDepthStencilValue clear_depth={.depth=0.875f,.stencil=0x5a};
    vkCmdClearColorImage(command,images[0].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear_color,1,&color_range);
    vkCmdClearDepthStencilImage(command,images[1].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear_depth,1,&depth_range);
    transition(command,images[0].image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkPipelineStageFlags depth_stages=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkAccessFlags depth_access=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    transition(command,images[1].image,depth_aspects,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_TRANSFER_BIT,depth_stages,VK_ACCESS_TRANSFER_WRITE_BIT,depth_access);
    VkRenderPassBeginInfo render={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=render_pass,.framebuffer=framebuffer,.renderArea={{0,0},{WIDTH,HEIGHT}}};
    vkCmdBeginRenderPass(command,&render,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&set,0,NULL);
    vkCmdDraw(command,9,1,0,0);vkCmdEndRenderPass(command);
    transition(command,images[0].image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    transition(command,images[1].image,depth_aspects,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,depth_stages,VK_PIPELINE_STAGE_TRANSFER_BIT,depth_access,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy={.bufferOffset=COLOR_OFFSET,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={WIDTH,HEIGHT,1}};
    vkCmdCopyImageToBuffer(command,images[0].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    copy.bufferOffset=DEPTH_OFFSET;copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImageToBuffer(command,images[1].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    if(!d32){copy.bufferOffset=STENCIL_OFFSET;copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;vkCmdCopyImageToBuffer(command,images[1].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);}
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    VK_OK(vkEndCommandBuffer(command));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    VK_OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));VK_OK(vkQueueWaitIdle(queue));
    unsigned shaded=0,guard_pixels=0,guard_bytes=0;
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++){
        unsigned pixel=y*WIDTH+x;int inside=x>0&&x<WIDTH-1&&y>0&&y<HEIGHT-1;
        const float *color_data=(const float *)(mapped+COLOR_OFFSET)+pixel*4;
        const float expected[4]={0.34375f,0.421875f,2,1};
        for(unsigned c=0;c<4;c++)check_float(color_data[c],inside?expected[c]:sentinel[c],2e-6f,"color-prior-depth-order",pixel*4+c);
        float depth_value;
        if(d32)memcpy(&depth_value,mapped+DEPTH_OFFSET+pixel*4,4);
        else {uint32_t raw;memcpy(&raw,mapped+DEPTH_OFFSET+pixel*4,4);depth_value=(float)(raw&0xffffffu)/16777215.0f;}
        check_float(depth_value,inside?0.421875f:0.875f,2e-6f,"final-real-depth-write",pixel);
        if(!d32&&mapped[STENCIL_OFFSET+pixel]!=0x5a){fprintf(stderr,"stencil guard pixel=%u got=%u\n",pixel,mapped[STENCIL_OFFSET+pixel]);return 7;}
        if(inside)shaded++;else guard_pixels++;
    }
    for(unsigned i=0;i<BUFFER_BYTES;i++){
        if(i<COLOR_OFFSET+PIXELS*16||(i>=DEPTH_OFFSET&&i<DEPTH_OFFSET+PIXELS*4)||(!d32&&i>=STENCIL_OFFSET&&i<STENCIL_OFFSET+PIXELS))continue;
        if(mapped[i]!=0xcd){fprintf(stderr,"buffer guard byte=%u got=%u\n",i,mapped[i]);return 7;}guard_bytes++;
    }
    if(validation_errors){fprintf(stderr,"validation errors=%u\n",validation_errors);return 8;}
    printf("{\"passed\":true,\"fixture_only\":true,\"gpu\":\"%s\",\"extension\":\"%s\",\"format\":\"%s\","
        "\"draw_calls\":1,\"overlapping_primitives\":3,\"depth_test_enable\":true,\"depth_write_enable\":true,\"input_layout\":\"GENERAL\","
        "\"input_reads\":[0.875,0.4375,0.34375],\"depth_writes\":[0.4375,0.34375,0.421875],\"checked_final_prior_depth\":0.34375,\"checked_final_depth\":0.421875,"
        "\"checked_shaded_pixels\":%u,\"checked_guard_pixels\":%u,\"checked_buffer_guard_bytes\":%u,\"checked_stencil_pixels\":%u,"
        "\"absolute_tolerance\":0.000002,\"validation_enabled\":%s,\"validation_errors\":%u,\"subTexelPrecisionBits\":%u,\"subPixelPrecisionBits\":%u}\n",
        properties.deviceName,device_extension,d32?"D32_SFLOAT":"D24_UNORM_S8_UINT",shaded,guard_pixels,guard_bytes,d32?0u:PIXELS,
        validation?"true":"false",validation_errors,properties.limits.subTexelPrecisionBits,properties.limits.subPixelPrecisionBits);
    vkDestroyCommandPool(device,command_pool,NULL);vkDestroyPipeline(device,pipeline,NULL);
    for(unsigned i=0;i<2;i++)vkDestroyShaderModule(device,modules[i],NULL);
    vkDestroyFramebuffer(device,framebuffer,NULL);vkDestroyRenderPass(device,render_pass,NULL);vkDestroyPipelineLayout(device,pipeline_layout,NULL);
    vkDestroyDescriptorPool(device,descriptor_pool,NULL);vkDestroyDescriptorSetLayout(device,layout,NULL);
    for(unsigned i=0;i<2;i++){if(images[i].input)vkDestroyImageView(device,images[i].input,NULL);vkDestroyImageView(device,images[i].attachment,NULL);vkDestroyImage(device,images[i].image,NULL);vkFreeMemory(device,images[i].memory,NULL);}
    vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);vkDestroyDevice(device,NULL);
    if(messenger){PFN_vkDestroyDebugUtilsMessengerEXT destroy=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT");if(destroy)destroy(instance,messenger,NULL);}
    vkDestroyInstance(instance,NULL);return 0;
}
