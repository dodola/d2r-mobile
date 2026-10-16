/* Preserve every mip/layer across late storage promotion. Initial GPU clears
 * and the first storage read/modify/write are in the SAME submission; cached
 * old framebuffer references must stay alive until the real fence completes. */
    p_QueueFinish(&queue);dm_vk_attachment_config(3);
    unsigned char *usage_memory=allocate(4*1024*1024,4096,NULL);if(!usage_memory)return 190;
    NVNmemoryPool usage_pool={0};p_MemoryPoolBuilderSetStorage(&pb,usage_memory,4*1024*1024);
    if(!p_MemoryPoolInitialize(&usage_pool,&pb))return 191;
    NVNtexturePool usage_tp={0};if(!p_TexturePoolInitialize(&usage_tp,&pool,65536,512))return 192;
    NVNprogram usage_compute={0};NVNshaderData usage_cs={0,control(argv[1],"@IMAGE_COMPUTE@")};
    p_ProgramInitialize(&usage_compute,&device);if(!p_ProgramSetShaders(&usage_compute,1,&usage_cs))return 193;
    char usage_name[1400];snprintf(usage_name,sizeof usage_name,"%s.images",argv[2]);
    FILE *usage_file=fopen(usage_name,"wb");if(!usage_file)return 194;
    uint64_t usage_bytes=0;unsigned usage_dispatches=0;
    NVNtexture usage_texture={0};
    for(unsigned variant=0;variant<4;variant++){
        uint64_t usage_before[5],usage_after[5];dm_vk_image_usage_stats(usage_before);
        p_TextureBuilderSetDefaults(&tb);p_TextureBuilderSetDevice(&tb,&device);
        p_TextureBuilderSetWidth(&tb,64);p_TextureBuilderSetHeight(&tb,64);p_TextureBuilderSetDepth(&tb,3);
        p_TextureBuilderSetLevels(&tb,3);p_TextureBuilderSetTarget(&tb,NVN_TEXTURE_TARGET_2D_ARRAY);
        p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);p_TextureBuilderSetFlags(&tb,(variant&1)?NVN_TEXTURE_FLAGS_IMAGE:0);
        p_TextureBuilderSetStorage(&tb,&usage_pool,0);if(!p_TextureInitialize(&usage_texture,&tb))return 195;
        NVNtextureView usage_view={0};p_TextureViewSetDefaults(&usage_view);p_TextureViewSetLevels(&usage_view,1,1);
        p_TextureViewSetLayers(&usage_view,1,1);p_TextureViewSetTarget(&usage_view,NVN_TEXTURE_TARGET_2D);
        p_TexturePoolRegisterTexture(&usage_tp,256,&usage_texture,&usage_view);
        for(unsigned round=0;round<2;round++){
            p_CommandBufferAddCommandMemory(&cb,&usage_pool,2*1024*1024,2*1024*1024);p_CommandBufferBeginRecording(&cb);
            const NVNtexture *active=&usage_texture;
            for(unsigned layer=0;layer<3;layer++)for(unsigned mip=0;mip<3;mip++){
                if(round&&!(layer==2&&mip==2))continue;
                NVNtextureView clear_view={0};p_TextureViewSetDefaults(&clear_view);
                p_TextureViewSetLevels(&clear_view,mip,1);p_TextureViewSetLayers(&clear_view,layer,1);
                const NVNtextureView *views[1]={&clear_view};
                float bg[4]={(float)(layer&1),(float)(mip&1),(float)((layer+mip)&1),1};
                if(round){bg[0]=0;bg[1]=1;bg[2]=0;}
                p_CommandBufferSetRenderTargets(&cb,1,&active,views,NULL,NULL);
                p_CommandBufferClearColor(&cb,0,bg,15);
            }
            p_CommandBufferBindProgram(&cb,&usage_compute,NVN_SHADER_STAGE_COMPUTE_BIT);
            p_CommandBufferSetTexturePool(&cb,&usage_tp);NVNimageHandle usage_handle=256;
            p_CommandBufferBindImages(&cb,NVN_SHADER_STAGE_COMPUTE,0,1,&usage_handle);
            p_CommandBufferDispatchCompute(&cb,1,1,1);usage_dispatches++;
            p_CommandBufferBarrier(&cb,NVN_BARRIER_ORDER_PRIMITIVES_BIT);
            handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
            for(unsigned layer=0;layer<3;layer++)for(unsigned mip=0;mip<3;mip++){
                NVNtextureView read_view={0};p_TextureViewSetDefaults(&read_view);
                p_TextureViewSetLevels(&read_view,mip,1);p_TextureViewSetLayers(&read_view,layer,1);
                unsigned w=64>>mip;NVNcopyRegion read_region={0,0,0,w,w,1};
                p_TextureReadTexels(&usage_texture,&read_view,&read_region,pixels);
                for(unsigned p=0;p<w*w;p++){
                    unsigned expected[4]={255*(layer&1),255*(mip&1),255*((layer+mip)&1),255};
                    if(!round&&layer==1&&mip==1&&!p)for(unsigned c=0;c<3;c++)expected[c]=255-expected[c];
                    if(round&&layer==2&&mip==2){expected[0]=0;expected[1]=255;expected[2]=0;}
                    for(unsigned c=0;c<4;c++)if(pixels[p*4+c]!=expected[c]){
                        fprintf(stderr,"IMAGE_USAGE_PIXEL variant=%u round=%u layer=%u mip=%u pixel=%u channel=%u got=%u expected=%u\n",
                            variant,round,layer,mip,p,c,pixels[p*4+c],expected[c]);return 196;
                    }
                }
                size_t bytes=w*w*4;if(fwrite(pixels,1,bytes,usage_file)!=bytes)return 197;usage_bytes+=bytes;
            }
        }
        dm_vk_image_usage_stats(usage_after);
        uint64_t promotions=usage_after[2]-usage_before[2],wanted=usage_before[4]&&!(variant&1);
        if(promotions!=wanted||usage_after[3]-usage_before[3]!=wanted||
           usage_after[0]-usage_before[0]!=wanted||usage_after[1]-usage_before[1]!=(uint64_t)!wanted)return 198;
        p_TextureFinalize(&usage_texture);dm_vk_image_usage_stats(usage_after);
        if(usage_after[3]!=usage_before[3])return 199;
        fprintf(stderr,"IMAGE_USAGE_PHASE variant=%u declared=%llu image_flag=%u promotions=%llu freed_old_images=1 passed=1\n",
            variant,(unsigned long long)usage_before[4],variant&1,(unsigned long long)promotions);
    }
    fclose(usage_file);p_ProgramFinalize(&usage_compute);free((void *)usage_cs.control);
    p_TexturePoolFinalize(&usage_tp);p_MemoryPoolFinalize(&usage_pool);free(usage_memory);
    fprintf(stderr,"IMAGE_USAGE_DEVICE variants=4 levels=3 layers=3 compute=%u compared_bytes=%llu same_submit_producer=1 passed=1\n",
        usage_dispatches,(unsigned long long)usage_bytes);
