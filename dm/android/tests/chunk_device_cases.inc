/* GPU writes change vertex colours while alternating attachments forces real
 * pass boundaries. Three disjoint stripes retain A/B/A to expose lost/reordered
 * producers, stale CPU reuploads, and binding loss across early submissions. */
    p_QueueFinish(&queue);
    NVNtexture chunk_other={0};
    p_TextureBuilderSetDefaults(&tb);p_TextureBuilderSetDevice(&tb,&device);
    p_TextureBuilderSetWidth(&tb,64);p_TextureBuilderSetHeight(&tb,64);
    p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);p_TextureBuilderSetStorage(&tb,&pool,32768);
    if(!p_TextureInitialize(&chunk_other,&tb))return 130;
    NVNprogram chunk_compute[2]={0};
    NVNshaderData chunk_cs[2]={{0,control(argv[1],"@COMPUTE_A@")},{0,control(argv[1],"@COMPUTE_B@")}};
    for(unsigned i=0;i<2;i++){p_ProgramInitialize(&chunk_compute[i],&device);p_ProgramSetShaders(&chunk_compute[i],1,&chunk_cs[i]);}
    unsigned char chunk_triangle[4096];for(unsigned i=0;i<4096;i++)chunk_triangle[i]=reference[0][i*4]!=0;
    unsigned char chunk_reference[2][sizeof pixels];
    unsigned char *chunk_commands=allocate(2*1024*1024,4096,NULL);if(!chunk_commands)return 137;
    NVNmemoryPool chunk_pool={0};p_MemoryPoolBuilderSetStorage(&pb,chunk_commands,2*1024*1024);
    if(!p_MemoryPoolInitialize(&chunk_pool,&pb))return 138;
    const int chunk_modes[]={0,16,48,256,256,48,16,0};
    char chunk_file[1400];snprintf(chunk_file,sizeof chunk_file,"%s.chunks",argv[2]);FILE *chunk_output=fopen(chunk_file,"wb");if(!chunk_output)return 131;
    uint64_t chunk_bytes=0;
    for(unsigned mode=0;mode<sizeof chunk_modes/sizeof *chunk_modes;mode++){
        p_QueueFinish(&queue);dm_vk_chunk_config(chunk_modes[mode]);
        memcpy(memory+131072,vertices,sizeof vertices);
        p_CommandBufferAddCommandMemory(&cb,&chunk_pool,0,2*1024*1024);p_CommandBufferBeginRecording(&cb);
        p_CommandBufferBindProgram(&cb,&graphics,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
        p_CommandBufferBindUniformBuffer(&cb,NVN_SHADER_STAGE_VERTEX,0,base+4096,65536);
        p_CommandBufferBindVertexAttribState(&cb,2,attributes);p_CommandBufferBindVertexStreamState(&cb,1,&stream);
        p_CommandBufferBindVertexBuffers(&cb,0,1,&vertex_range);
        NVNdepthStencilState chunk_ds={0};p_DepthStencilStateSetDefaults(&chunk_ds);p_CommandBufferBindDepthStencilState(&cb,&chunk_ds);
        p_CommandBufferSetViewports(&cb,0,1,viewport);p_CommandBufferSetScissor(&cb,0,0,64,64);
        for(unsigned draw=0;draw<600;draw++){
            const NVNtexture *active=(draw&1)?&chunk_other:&target;
            p_CommandBufferSetRenderTargets(&cb,1,&active,NULL,NULL,NULL);
            if(draw<2)p_CommandBufferClearColor(&cb,0,clear,15);
            if(draw==100||draw==300||draw==500){
                p_CommandBufferBindProgram(&cb,&chunk_compute[draw==300],NVN_SHADER_STAGE_COMPUTE_BIT);
                p_CommandBufferBindStorageBuffer(&cb,NVN_SHADER_STAGE_COMPUTE,3,base+131072,sizeof vertices);
                p_CommandBufferDispatchCompute(&cb,1,1,1);
                p_CommandBufferBarrier(&cb,NVN_BARRIER_ORDER_PRIMITIVES_BIT|NVN_BARRIER_ORDER_INDIRECT_DATA_BIT);
                p_CommandBufferBindProgram(&cb,&graphics,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
                p_CommandBufferSetScissor(&cb,draw==100?0:draw==300?21:43,0,draw==300?22:21,64);
            }
            p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
        }
        uint64_t chunks_before=dm_vk_chunk_count();
        handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
        uint64_t chunks=dm_vk_chunk_count()-chunks_before;
        if(dm_vk_chunk_applied()!=chunk_modes[mode]||(!chunk_modes[mode]?chunks!=0:chunks!=599/(unsigned)chunk_modes[mode])){
            fprintf(stderr,"CHUNK_COUNT mode=%u threshold=%d actual=%llu\n",mode,chunk_modes[mode],(unsigned long long)chunks);return 132;
        }
        const float final_color[4]={.25f,.5f,.75f,1};
        for(unsigned v=0;v<3;v++)if(memcmp(memory+131072+v*32,vertices+v*8,16)||memcmp(memory+131072+v*32+16,final_color,16))return 133;
        for(unsigned image=0;image<2;image++){
            p_TextureReadTexels(image?&chunk_other:&target,NULL,&region,pixels);
            for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
                unsigned at=y*64+x;int lit=chunk_triangle[at],middle=x>=21&&x<43;
                unsigned expected[4]={lit?(middle?191:64):0,lit?(middle?64:128):0,lit?(middle?128:191):0,255};
                for(unsigned c=0;c<4;c++)if(abs((int)pixels[at*4+c]-(int)expected[c])>1){
                    fprintf(stderr,"CHUNK_PIXEL mode=%u image=%u xy=%u,%u channel=%u got=%u expected=%u\n",mode,image,x,y,c,pixels[at*4+c],expected[c]);return 134;
                }
            }
            if(!mode)memcpy(chunk_reference[image],pixels,sizeof pixels);
            else if(memcmp(chunk_reference[image],pixels,sizeof pixels))return 135;
            if(fwrite(pixels,1,sizeof pixels,chunk_output)!=sizeof pixels)return 136;chunk_bytes+=sizeof pixels;
        }
        fprintf(stderr,"CHUNK_PHASE mode=%u threshold=%d draws=600 compute=3 chunks=%llu gpu_vertex_writeback=1 passed=1\n",mode,chunk_modes[mode],(unsigned long long)chunks);
    }
    fclose(chunk_output);dm_vk_chunk_config(-1);
    p_TextureFinalize(&chunk_other);
    for(unsigned i=0;i<2;i++){p_ProgramFinalize(&chunk_compute[i]);free((void *)chunk_cs[i].control);}
    p_MemoryPoolFinalize(&chunk_pool);free(chunk_commands);
    fprintf(stderr,"CHUNK_DEVICE modes=8 draws=4800 compute=24 read_bytes=%llu same_process=1 passed=1\n",(unsigned long long)chunk_bytes);
