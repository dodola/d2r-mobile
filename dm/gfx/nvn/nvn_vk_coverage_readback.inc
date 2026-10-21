/* This hook is called immediately after EndRenderPass, before another writer.
 * The buffer is reserved once at BEGIN and stays owned by the submission. */
static void cov_probe_end(void) {
    if(!S.ncoverage)return;
    unsigned i=S.ncoverage-1;
    if(S.coverage[i].pass!=G.pass_count-1||!S.coverage[i].texture)return;
    VTexture *t=S.coverage[i].texture;VBuffer *b=S.coverage[i].buffer;
    VkImageLayout layout=S.coverage[i].layout;
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,S.coverage[i].mip,1,S.coverage[i].layer,1};
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange=range};
    if(g_cov_selftest){
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkClearValue clear;uint32_t word;
        if(!nvn_cov_sentinel(t->format,&clear,&word))die("coverage selftest format",t->format);
        vkCmdClearColorImage(S.command,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear.color,1,&range);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    }
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,range.baseMipLevel,range.baseArrayLayer,1},
        .imageExtent={S.coverage[i].w,S.coverage[i].h,1}};
    vkCmdCopyImageToBuffer(S.command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b->b,1,&copy);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=layout;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
    S.coverage[i].texture=NULL; /* Consumed: never copy this probe again. */
}
