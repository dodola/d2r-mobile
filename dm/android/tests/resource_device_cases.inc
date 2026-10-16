/* Cross-submit immutable-object reuse, invalidation and bounded retention.
 * Pixels have independent color expectations; clear values change on hits. */
    p_QueueFinish(&queue);dm_vk_attachment_config(0);
    unsigned char resource_triangle[4096];for(unsigned p=0;p<4096;p++)resource_triangle[p]=reference[0][p*4]!=0;
    char resource_file[1400];snprintf(resource_file,sizeof resource_file,"%s.resources",argv[2]);
    FILE *resource_output=fopen(resource_file,"wb");if(!resource_output)return 170;
    uint64_t resource_bytes=0;unsigned resource_draws=0;
    NVNtexture resource_other={0};
    p_TextureBuilderSetDefaults(&tb);p_TextureBuilderSetDevice(&tb,&device);
    p_TextureBuilderSetWidth(&tb,64);p_TextureBuilderSetHeight(&tb,64);p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);
    p_TextureBuilderSetStorage(&tb,&pool,32768);if(!p_TextureInitialize(&resource_other,&tb))return 171;
    const unsigned resource_modes[]={0,1,1,0};
    for(unsigned phase=0;phase<4;phase++){
        dm_vk_resource_config(resource_modes[phase]);uint64_t before[8],after[8];dm_vk_resource_stats(before);
        for(unsigned round=0;round<12;round++)for(unsigned image=0;image<2;image++){
            const NVNtexture *active=image?&resource_other:&target;
            float bg[4]={(float)(round&1),0,(float)!(round&1),1};
            p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
            p_CommandBufferBindProgram(&cb,&graphics,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
            p_CommandBufferBindUniformBuffer(&cb,NVN_SHADER_STAGE_VERTEX,0,base+4096,65536);
            p_CommandBufferBindVertexAttribState(&cb,2,attributes);p_CommandBufferBindVertexStreamState(&cb,1,&stream);
            p_CommandBufferBindVertexBuffers(&cb,0,1,&vertex_range);
            NVNdepthStencilState resource_ds={0};p_DepthStencilStateSetDefaults(&resource_ds);p_CommandBufferBindDepthStencilState(&cb,&resource_ds);
            p_CommandBufferSetViewports(&cb,0,1,viewport);p_CommandBufferSetScissor(&cb,0,0,64,64);
            p_CommandBufferSetRenderTargets(&cb,1,&active,NULL,NULL,NULL);p_CommandBufferClearColor(&cb,0,bg,15);
            p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);resource_draws++;
            p_TextureReadTexels(active,NULL,&region,pixels);
            for(unsigned p=0;p<4096;p++){
                const unsigned expected[4]={resource_triangle[p]?255:255*(round&1),resource_triangle[p]?64:0,resource_triangle[p]?32:255*!(round&1),255};
                for(unsigned c=0;c<4;c++)if(abs((int)pixels[p*4+c]-(int)expected[c])>1)return 172;
            }
            if(fwrite(pixels,1,sizeof pixels,resource_output)!=sizeof pixels)return 173;resource_bytes+=sizeof pixels;
        }
        dm_vk_resource_stats(after);
        uint64_t creates=after[1]-before[1],hits=after[2]-before[2];
        if(dm_vk_resource_applied()!=(int)resource_modes[phase]||
            (resource_modes[phase]?(creates!=(phase==1?2u:0u)||hits<22):creates!=24)){
            fprintf(stderr,"RESOURCE_BAD phase=%u creates=%llu hits=%llu\n",phase,(unsigned long long)creates,(unsigned long long)hits);return 174;
        }
        fprintf(stderr,"RESOURCE_PHASE phase=%u enabled=%u draws=24 creates=%llu hits=%llu passed=1\n",phase,resource_modes[phase],(unsigned long long)creates,(unsigned long long)hits);
    }
    p_TextureFinalize(&resource_other);
    /* Reuse the exact NVN object address/storage after freeing its GPU image. */
    dm_vk_resource_config(1);
    for(unsigned incarnation=0;incarnation<4;incarnation++){
        if(!p_TextureInitialize(&resource_other,&tb))return 175;
        for(unsigned iteration=0;iteration<2;iteration++){
            const NVNtexture *active=&resource_other;const float bg[4]={0,1,0,1};
            p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
            p_CommandBufferSetRenderTargets(&cb,1,&active,NULL,NULL,NULL);p_CommandBufferClearColor(&cb,0,bg,15);
            p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);resource_draws++;
            p_TextureReadTexels(active,NULL,&region,pixels);
            for(unsigned p=0;p<4096;p++){
                const unsigned expected[4]={resource_triangle[p]?255:0,resource_triangle[p]?64:255,resource_triangle[p]?32:0,255};
                for(unsigned c=0;c<4;c++)if(abs((int)pixels[p*4+c]-(int)expected[c])>1)return 176;
            }
            if(fwrite(pixels,1,sizeof pixels,resource_output)!=sizeof pixels)return 177;resource_bytes+=sizeof pixels;
        }
        p_TextureFinalize(&resource_other);uint64_t empty[8];dm_vk_resource_stats(empty);if(empty[5]||empty[6])return 178;
    }
    /* Exceed both retention thresholds in one genuine submission. A subsequent
     * begin must trim at its completed fence while preserving all layer data. */
    const size_t resource_size=16u*1024u*1024u;
    unsigned char *resource_memory=allocate(resource_size,4096,NULL);if(!resource_memory)return 179;
    NVNmemoryPool resource_pool={0};p_MemoryPoolBuilderSetStorage(&pb,resource_memory,resource_size);
    if(!p_MemoryPoolInitialize(&resource_pool,&pb))return 180;
    NVNtexture resource_array={0};p_TextureBuilderSetDefaults(&tb);p_TextureBuilderSetDevice(&tb,&device);
    p_TextureBuilderSetWidth(&tb,64);p_TextureBuilderSetHeight(&tb,64);p_TextureBuilderSetDepth(&tb,270);
    p_TextureBuilderSetTarget(&tb,NVN_TEXTURE_TARGET_2D_ARRAY);p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);p_TextureBuilderSetStorage(&tb,&resource_pool,0);
    if(p_TextureBuilderGetStorageSize(&tb)>12u*1024u*1024u||!p_TextureInitialize(&resource_array,&tb))return 181;
    uint64_t limit_before[8],limit_after[8];dm_vk_resource_stats(limit_before);
    p_CommandBufferAddCommandMemory(&cb,&resource_pool,12u*1024u*1024u,2u*1024u*1024u);p_CommandBufferBeginRecording(&cb);
    for(unsigned layer=0;layer<270;layer++){
        NVNtextureView view={0};p_TextureViewSetDefaults(&view);p_TextureViewSetLayers(&view,layer,1);
        const NVNtextureView *views[1]={&view};const NVNtexture *active=&resource_array;
        float bg[4]={(float)(layer&1),(float)((layer>>1)&1),(float)((layer>>2)&1),1};
        p_CommandBufferSetRenderTargets(&cb,1,&active,views,NULL,NULL);p_CommandBufferClearColor(&cb,0,bg,15);
        p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);resource_draws++;
    }
    handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
    dm_vk_resource_stats(limit_after);if(limit_after[5]!=270||limit_after[6]!=270)return 182;
    for(unsigned layer=0;layer<270;layer++){
        NVNtextureView view={0};p_TextureViewSetDefaults(&view);p_TextureViewSetLayers(&view,layer,1);
        p_TextureReadTexels(&resource_array,&view,&region,pixels);
        for(unsigned p=0;p<4096;p++){
            const unsigned expected[4]={resource_triangle[p]?255:255*(layer&1),resource_triangle[p]?64:255*((layer>>1)&1),resource_triangle[p]?32:255*((layer>>2)&1),255};
            for(unsigned c=0;c<4;c++)if(abs((int)pixels[p*4+c]-(int)expected[c])>1)return 183;
        }
        if(fwrite(pixels,1,sizeof pixels,resource_output)!=sizeof pixels)return 184;resource_bytes+=sizeof pixels;
    }
    dm_vk_resource_stats(limit_after);if(limit_after[3]<=limit_before[3]||limit_after[5]||limit_after[6])return 185;
    p_TextureFinalize(&resource_array);p_MemoryPoolFinalize(&resource_pool);free(resource_memory);
    fclose(resource_output);dm_vk_resource_config(-1);
    fprintf(stderr,"RESOURCE_DEVICE modes=4 draws=%u read_bytes=%llu recreations=4 trim_views=270 trim_passes=270 passed=1\n",resource_draws,(unsigned long long)resource_bytes);
