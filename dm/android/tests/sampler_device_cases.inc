/* Synthetic shaders exercise real NVN descriptor/sampler paths, not game FPS. */
    {
        NVNtexturePool sample_tp={0};NVNsamplerPool sample_sp={0};
        if(!p_TexturePoolInitialize(&sample_tp,&pool,65536,512)||!p_SamplerPoolInitialize(&sample_sp,&pool,98304,512))return 120;
        const char *sample_ps[]={"@UINT_SHADER@","@FLOAT_SHADER@","@DEPTH_SHADER@"};
        const NVNformat sample_formats[]={NVN_FORMAT_R32UI,NVN_FORMAT_R32F,NVN_FORMAT_DEPTH32F};
        for(unsigned test=0;test<3;test++){
            NVNtexture sample_texture={0};NVNtextureBuilder builder={0};
            p_TextureBuilderSetDefaults(&builder);p_TextureBuilderSetDevice(&builder,&device);
            p_TextureBuilderSetWidth(&builder,4);p_TextureBuilderSetHeight(&builder,4);
            p_TextureBuilderSetFormat(&builder,sample_formats[test]);p_TextureBuilderSetStorage(&builder,&pool,32768);
            if(!p_TextureInitialize(&sample_texture,&builder))return 121;
            NVNsamplerBuilder sample_sb={0};NVNsampler sample_sampler={0};
            p_SamplerBuilderSetDefaults(&sample_sb);p_SamplerBuilderSetDevice(&sample_sb,&device);
            p_SamplerBuilderSetMinMagFilter(&sample_sb,NVN_MIN_FILTER_LINEAR,NVN_MAG_FILTER_LINEAR);
            if(test==2)p_SamplerBuilderSetCompare(&sample_sb,NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE,NVN_COMPARE_FUNC_LESS);
            if(!p_SamplerInitialize(&sample_sampler,&sample_sb))return 122;
            p_TexturePoolRegisterTexture(&sample_tp,256,&sample_texture,NULL);
            p_SamplerPoolRegisterSampler(&sample_sp,256,&sample_sampler);
            NVNprogram sample_program={0};p_ProgramInitialize(&sample_program,&device);
            NVNshaderData sample_shaders[2]={{0,control(argv[1],"@VERTEX_SHADER@")},{0,control(argv[1],sample_ps[test])}};
            if(!p_ProgramSetShaders(&sample_program,2,sample_shaders))return 123;
            for(unsigned i=0;i<16;i++){
                if(test==0)((unsigned *)(memory+131072))[i]=255;
                else ((float *)(memory+131072))[i]=test==1?1.f:.75f;
            }
            p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
            NVNcopyRegion upload={0,0,0,4,4,1};
            p_CommandBufferCopyBufferToTexture(&cb,base+131072,&sample_texture,NULL,&upload,0);
            p_CommandBufferSetRenderTargets(&cb,1,targets,NULL,NULL,NULL);
            p_CommandBufferSetScissor(&cb,0,0,64,64);p_CommandBufferSetViewports(&cb,0,1,viewport);
            NVNdepthStencilState sample_ds={0};p_DepthStencilStateSetDefaults(&sample_ds);p_CommandBufferBindDepthStencilState(&cb,&sample_ds);
            NVNcolorState sample_color={0};p_ColorStateSetDefaults(&sample_color);p_CommandBufferBindColorState(&cb,&sample_color);
            NVNchannelMaskState sample_mask={0};p_ChannelMaskStateSetDefaults(&sample_mask);p_CommandBufferBindChannelMaskState(&cb,&sample_mask);
            NVNpolygonState sample_polygon={0};p_PolygonStateSetDefaults(&sample_polygon);p_CommandBufferBindPolygonState(&cb,&sample_polygon);
            p_CommandBufferClearColor(&cb,0,clear,15);
            p_CommandBufferBindProgram(&cb,&sample_program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
            p_CommandBufferSetTexturePool(&cb,&sample_tp);p_CommandBufferSetSamplerPool(&cb,&sample_sp);
            NVNseparateTextureHandle sample_th={256};NVNseparateSamplerHandle sample_sh={256<<20};
            p_CommandBufferBindSeparateTextures(&cb,NVN_SHADER_STAGE_FRAGMENT,0,1,&sample_th);
            p_CommandBufferBindSeparateSamplers(&cb,NVN_SHADER_STAGE_FRAGMENT,0,1,&sample_sh);
            p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);
            unsigned char sample_pixels[sizeof pixels];p_TextureReadTexels(&target,NULL,&region,sample_pixels);
            for(unsigned i=0;i<4096;i++)if(sample_pixels[i*4]!=255||sample_pixels[i*4+1]||sample_pixels[i*4+2]||sample_pixels[i*4+3]!=255){
                fprintf(stderr,"SAMPLER_DEVICE mismatch test=%u pixel=%u value=%u,%u,%u,%u\n",test,i,sample_pixels[i*4],sample_pixels[i*4+1],sample_pixels[i*4+2],sample_pixels[i*4+3]);return 124;}
            p_TextureFinalize(&sample_texture);p_SamplerFinalize(&sample_sampler);p_ProgramFinalize(&sample_program);
        }
        p_TexturePoolFinalize(&sample_tp);p_SamplerPoolFinalize(&sample_sp);
        fprintf(stderr,"SAMPLER_DEVICE integer_nearest=1 float_filter=1 depth_compare=1 compared_bytes=49152 passed=1\n");
    }
