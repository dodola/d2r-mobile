/* Image usages influence tile/compression choices on mobile drivers. Images
 * without the guest IMAGE hint can start without STORAGE; the first actual
 * storage binding upgrades them before either shader stage gets descriptors. */
static void texture_image_allocate(const VTexture *t,VkImageUsageFlags usage,
    VkImage *image,VkDeviceMemory *memory,VkImageView *view) {
    const tex_t *source=&t->source;
    int one=source->d.target==NVN_TEXTURE_TARGET_1D||source->d.target==NVN_TEXTURE_TARGET_1D_ARRAY;
    int layered=source->d.target==NVN_TEXTURE_TARGET_1D_ARRAY||source->d.target==NVN_TEXTURE_TARGET_2D_ARRAY;
    int volume=source->d.target==NVN_TEXTURE_TARGET_3D;
    int cube=source->d.target==NVN_TEXTURE_TARGET_CUBEMAP||source->d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=volume?VK_IMAGE_TYPE_3D:one?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D,
        .format=t->format,.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
        .extent={t->width,t->height,t->depth},.mipLevels=source->d.levels,.arrayLayers=source->L.layers,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateImage(G.device,&ii,NULL,image));VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(G.device,*image,&requirements);
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,
        .memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(G.device,&allocation,NULL,memory));CHECK(vkBindImageMemory(G.device,*image,*memory,0));
    VkImageViewType view_type=volume?VK_IMAGE_VIEW_TYPE_3D:one?(layered?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D):
        cube?(source->L.layers==6?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY):layered?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=*image,.viewType=view_type,.format=t->format,
        .subresourceRange={aspect(t->format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,
            0,source->d.levels,0,source->L.layers}};
    CHECK(vkCreateImageView(G.device,&vi,NULL,view));
}
static void texture_require_storage(VTexture *t) {
    if(t->usage&VK_IMAGE_USAGE_STORAGE_BIT)return;
    VkFormatProperties properties=format_properties(t->format);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)||aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)
        die("storage image format unsupported",t->format);
    if(t->before_storage)die("repeated image storage promotion",t->serial);
    VTexture upgraded=*t;upgraded.usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    texture_image_allocate(t,upgraded.usage,&upgraded.image,&upgraded.memory,&upgraded.view);
    upgraded.layout=VK_IMAGE_LAYOUT_UNDEFINED;upgraded.mixed=0;memset(upgraded.lvl,0,sizeof upgraded.lvl);
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    texture_transition(&upgraded,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for(unsigned level=0;level<t->source.d.levels;level++){
        unsigned w=t->width>>level,h=t->height>>level,d=t->depth>>level;if(!w)w=1;if(!h)h=1;if(!d)d=1;
        VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,t->source.L.layers},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,t->source.L.layers},.extent={w,h,d}};
        vkCmdCopyImage(S.command,t->image,t->layout,upgraded.image,upgraded.layout,1,&copy);
    }
    texture_transition(&upgraded,VK_IMAGE_LAYOUT_GENERAL);
    /* Old views may occur in already-recorded descriptors and framebuffers.
     * Keep their allocation with this texture; only Finalize frees it, after
     * the same drain/cache invalidation used for every texture destruction.
     * Promotion is one-way, so this is bounded to one old image per texture. */
    t->before_storage=t->image;t->before_storage_memory=t->memory;t->before_storage_view=t->view;
    t->image=upgraded.image;t->memory=upgraded.memory;t->view=upgraded.view;t->usage=upgraded.usage;
    t->layout=upgraded.layout;t->mixed=upgraded.mixed;memcpy(t->lvl,upgraded.lvl,sizeof t->lvl);
    t->incarnation=++G.texture_incarnation;G.registry_revision++;G.image_promotions++;G.image_retired_live++;
    fprintf(stderr,"[nvn-vulkan] IMAGE_STORAGE_PROMOTION serial=%llu flags=%#x usage=%#x extent=%u,%u,%u levels=%u layers=%u retained_old_images=%llu\n",
        (unsigned long long)t->serial,t->source.d.flags,t->usage,t->width,t->height,t->depth,t->source.d.levels,t->source.L.layers,
        (unsigned long long)G.image_retired_live);
}
#ifdef DM_VK_IMAGE_USAGE_TEST
void dm_vk_image_usage_stats(uint64_t out[5]) {
    pthread_mutex_lock(&G.lock);out[0]=G.image_usage_omitted;out[1]=G.image_usage_storage;
    out[2]=G.image_promotions;out[3]=G.image_retired_live;out[4]=G.image_usage_by_flags;
    pthread_mutex_unlock(&G.lock);
}
#endif
