/* Every effective vertex fetch remains in range. CPU-authored controls and
 * Compute-produced indices must render the same independently known full-screen
 * triangle, including raw indices above 24/31 bits, negative base vertices,
 * UINT32_MAX without primitive restart, and command-buffer chunk boundaries. */
    p_QueueFinish(&queue);
    if(!dm_vk_index_full_uint32()){fprintf(stderr,"INDEX_DEVICE full_uint32_unavailable=1\n");return 150;}
    struct IndexCase { unsigned bits,first;int base_vertex;unsigned stride;const char *compute;uint32_t expected[4]; };
    static const struct IndexCase index_cases[]={@INDEX_CASES@};
    const unsigned index_case_count=sizeof index_cases/sizeof *index_cases;
    NVNtexture index_other={0};p_TextureBuilderSetDefaults(&tb);p_TextureBuilderSetDevice(&tb,&device);
    p_TextureBuilderSetWidth(&tb,64);p_TextureBuilderSetHeight(&tb,64);p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);
    p_TextureBuilderSetStorage(&tb,&pool,32768);if(!p_TextureInitialize(&index_other,&tb))return 160;
    NVNprogram index_graphics={0},index_compute[7]={0};
    p_ProgramInitialize(&index_graphics,&device);
    NVNshaderData index_graphics_shaders[2]={{0,control(argv[1],"@INDEX_VERTEX@")},{0,control(argv[1],"@INDEX_FRAGMENT@")}};
    p_ProgramSetShaders(&index_graphics,2,index_graphics_shaders);
    NVNshaderData index_compute_shaders[7]={0};
    for(unsigned i=0;i<index_case_count;i++){
        p_ProgramInitialize(&index_compute[i],&device);index_compute_shaders[i].control=control(argv[1],index_cases[i].compute);
        p_ProgramSetShaders(&index_compute[i],1,&index_compute_shaders[i]);
    }
    unsigned char *index_commands=allocate(262144,4096,NULL);if(!index_commands)return 151;
    NVNmemoryPool index_pool={0};p_MemoryPoolBuilderSetStorage(&pb,index_commands,262144);
    if(!p_MemoryPoolInitialize(&index_pool,&pb))return 152;
    char index_file[1400];snprintf(index_file,sizeof index_file,"%s.indices",argv[2]);
    FILE *index_output=fopen(index_file,"wb");if(!index_output)return 153;
    unsigned index_draws=0,index_dispatches=0,index_reads=0;
    const int index_modes[]={0,16,48};
    unsigned char index_reference[7][2][sizeof pixels];
    for(unsigned mode=0;mode<3;mode++)for(unsigned ci=0;ci<index_case_count;ci++)for(unsigned producer=0;producer<2;producer++){
        const struct IndexCase *test=&index_cases[ci];
        p_QueueFinish(&queue);dm_vk_chunk_config(index_modes[mode]);
        const float color[4]={.25f,.5f,.75f,1};
        for(unsigned v=0;v<3;v++)memcpy(memory+147456+v*16,color,16);
        /* Poison stays geometrically degenerate if a stale CPU copy overwrites
         * the producer. Check both GPU writeback and adjacent canaries. */
        memset(memory+151548,0xa5,24);memset(memory+151552,0,16);
        if(!producer)memcpy(memory+151552,test->expected,16);
        /* End clears before the indexed submission. Alternating actual render
         * targets below forces the pass boundaries at which chunks can split. */
        p_CommandBufferAddCommandMemory(&cb,&index_pool,0,262144);p_CommandBufferBeginRecording(&cb);
        p_CommandBufferSetScissor(&cb,0,0,64,64);
        for(unsigned image=0;image<2;image++){
            const NVNtexture *active=image?&index_other:&target;p_CommandBufferSetRenderTargets(&cb,1,&active,NULL,NULL,NULL);
            p_CommandBufferClearColor(&cb,0,clear,15);
        }
        handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
        p_CommandBufferAddCommandMemory(&cb,&index_pool,0,262144);p_CommandBufferBeginRecording(&cb);
        p_CommandBufferSetViewports(&cb,0,1,viewport);p_CommandBufferSetScissor(&cb,0,0,64,64);
        NVNdepthStencilState index_ds={0};p_DepthStencilStateSetDefaults(&index_ds);p_CommandBufferBindDepthStencilState(&cb,&index_ds);
        if(producer){
            p_CommandBufferBindProgram(&cb,&index_compute[ci],NVN_SHADER_STAGE_COMPUTE_BIT);
            p_CommandBufferBindStorageBuffer(&cb,NVN_SHADER_STAGE_COMPUTE,3,base+151552,16);
            p_CommandBufferDispatchCompute(&cb,1,1,1);
            p_CommandBufferBarrier(&cb,NVN_BARRIER_ORDER_PRIMITIVES_BIT|NVN_BARRIER_ORDER_INDIRECT_DATA_BIT);
            index_dispatches++;
        }
        p_CommandBufferBindProgram(&cb,&index_graphics,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
        NVNvertexAttribState index_attribute={0};p_VertexAttribStateSetDefaults(&index_attribute);
        p_VertexAttribStateSetFormat(&index_attribute,NVN_FORMAT_RGBA32F,0);
        p_CommandBufferBindVertexAttribState(&cb,1,&index_attribute);
        NVNvertexStreamState index_stream={0};p_VertexStreamStateSetDefaults(&index_stream);
        p_VertexStreamStateSetStride(&index_stream,test->stride);p_CommandBufferBindVertexStreamState(&cb,1,&index_stream);
        const NVNbufferRange range={base+147456,test->stride?48:16};p_CommandBufferBindVertexBuffers(&cb,0,1,&range);
        for(unsigned draw=0;draw<64;draw++){
            const NVNtexture *active=(draw&1)?&index_other:&target;p_CommandBufferSetRenderTargets(&cb,1,&active,NULL,NULL,NULL);
            p_CommandBufferDrawElementsInstanced(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,
                test->bits==16?NVN_INDEX_TYPE_UNSIGNED_SHORT:NVN_INDEX_TYPE_UNSIGNED_INT,3,base+151552,test->base_vertex,0,1);
        }
        uint64_t chunks_before=dm_vk_chunk_count();
        handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
        uint64_t chunks=dm_vk_chunk_count()-chunks_before;
        if(chunks!=(index_modes[mode]?63u/(unsigned)index_modes[mode]:0)||dm_vk_chunk_applied()!=index_modes[mode]){
            fprintf(stderr,"INDEX_CHUNKS threshold=%d actual=%llu\n",index_modes[mode],(unsigned long long)chunks);return 154;
        }
        if(memcmp(memory+151552,test->expected,16)){fprintf(stderr,"INDEX_WRITEBACK case=%u producer=%u\n",ci,producer);return 155;}
        for(unsigned b=0;b<4;b++)if(memory[151548+b]!=0xa5||memory[151568+b]!=0xa5)return 156;
        for(unsigned image=0;image<2;image++){
        p_TextureReadTexels(image?&index_other:&target,NULL,&region,pixels);
        const unsigned expected[4]={64,128,191,255};
        for(unsigned pixel=0;pixel<4096;pixel++)for(unsigned c=0;c<4;c++)if(abs((int)pixels[pixel*4+c]-(int)expected[c])>1){
            fprintf(stderr,"INDEX_PIXEL mode=%u case=%u producer=%u pixel=%u c=%u got=%u expected=%u\n",mode,ci,producer,pixel,c,pixels[pixel*4+c],expected[c]);return 157;
        }
        if(!mode&&!producer)memcpy(index_reference[ci][image],pixels,sizeof pixels);
        else if(memcmp(index_reference[ci][image],pixels,sizeof pixels))return 158;
        if(fwrite(pixels,1,sizeof pixels,index_output)!=sizeof pixels)return 159;
        index_reads+=sizeof pixels;
        }
        index_draws+=64;
        fprintf(stderr,"INDEX_PHASE mode=%u threshold=%d case=%u bits=%u first=%u base=%d stride=%u gpu_producer=%u draws=64 chunks=%llu passed=1\n",
            mode,index_modes[mode],ci,test->bits,test->first,test->base_vertex,test->stride,producer,(unsigned long long)chunks);
    }
    fclose(index_output);dm_vk_chunk_config(-1);
    p_TextureFinalize(&index_other);
    p_ProgramFinalize(&index_graphics);
    for(unsigned i=0;i<2;i++)free((void *)index_graphics_shaders[i].control);
    for(unsigned i=0;i<index_case_count;i++){p_ProgramFinalize(&index_compute[i]);free((void *)index_compute_shaders[i].control);}
    p_MemoryPoolFinalize(&index_pool);free(index_commands);
    fprintf(stderr,"INDEX_DEVICE modes=3 index_cases=7 sources=2 draws=%u compute=%u read_bytes=%u full_uint32=1 writeback_canaries=1 passed=1\n",
        index_draws,index_dispatches,index_reads);
