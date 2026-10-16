/* Actual NVN recorder, Vulkan copies/draws, CPU reference and GPU readback.
 * No game assets are modified. Included before the attachment test cases.
 */
    {
        unsigned char saved_color[sizeof pixels];memcpy(saved_color,pixels,sizeof pixels);
        unsigned char *copy_memory=allocate(2u<<20,4096,NULL);if(!copy_memory)return 140;memset(copy_memory,0,2u<<20);
        NVNmemoryPoolBuilder copy_pb={0};NVNmemoryPool copy_pool={0};
        p_MemoryPoolBuilderSetDefaults(&copy_pb);p_MemoryPoolBuilderSetDevice(&copy_pb,&device);
        p_MemoryPoolBuilderSetStorage(&copy_pb,copy_memory,2u<<20);
        p_MemoryPoolBuilderSetFlags(&copy_pb,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        if(!p_MemoryPoolInitialize(&copy_pool,&copy_pb))return 141;
        const NVNformat formats[]={NVN_FORMAT_DEPTH16,NVN_FORMAT_DEPTH32F,NVN_FORMAT_DEPTH24_STENCIL8,NVN_FORMAT_DEPTH32F_STENCIL8};
        const unsigned bytes_per_pixel[]={2,4,4,8};
        /* source/destination mip, first layer, layer count, source/dest rectangle */
        const struct {unsigned sm,dm,sl,dl,layers;NVNcopyRegion src,dst;} cases[]={
            {0,0,0,0,1,{0,0,0,8,6,1},{0,0,0,32,24,1}},
            {0,0,0,0,1,{0,0,0,32,24,1},{3,2,0,13,9,1}},
            {0,0,0,0,1,{2,3,0,13,9,1},{5,7,0,17,13,1}},
            {0,0,0,0,1,{2,3,0,9,7,1},{4,5,0,9,7,1}},
            {1,0,0,1,1,{1,2,0,11,7,1},{4,5,0,17,13,1}},
            {0,1,1,0,1,{3,2,0,23,17,1},{2,3,0,11,7,1}},
            {0,0,0,0,2,{2,3,0,13,9,2},{5,7,0,17,13,2}},
            {0,1,1,0,1,{3,2,0,23,17,1},{1,2,0,9,7,1}},
            {1,1,0,0,2,{2,1,0,9,7,2},{3,2,0,11,9,2}},
        };
        char copy_path[1200];snprintf(copy_path,sizeof copy_path,"%s.depth",argv[2]);FILE *copy_file=fopen(copy_path,"wb");if(!copy_file)return 142;
        uint64_t read_texels=0,read_bytes=0;unsigned triangle_samples=0;
        for(unsigned pixel=0;pixel<4096;pixel++)triangle_samples+=saved_color[pixel*4]!=0;
        for(unsigned format_index=0;format_index<4;format_index++){
            unsigned bpp=bytes_per_pixel[format_index];NVNtexture textures[2]={0};NVNtextureBuilder builder={0};
            p_TextureBuilderSetDefaults(&builder);p_TextureBuilderSetDevice(&builder,&device);
            p_TextureBuilderSetTarget(&builder,NVN_TEXTURE_TARGET_2D_ARRAY);p_TextureBuilderSetFormat(&builder,formats[format_index]);
            p_TextureBuilderSetWidth(&builder,32);p_TextureBuilderSetHeight(&builder,24);p_TextureBuilderSetDepth(&builder,2);p_TextureBuilderSetLevels(&builder,2);
            for(unsigned image=0;image<2;image++){
                p_TextureBuilderSetStorage(&builder,&copy_pool,262144+image*262144);
                if(p_TextureBuilderGetStorageSize(&builder)>262144||!p_TextureInitialize(&textures[image],&builder))return 143;
            }
            NVNtextureView views[2][2]={0};
            for(unsigned mip=0;mip<2;mip++)for(unsigned layer=0;layer<2;layer++){
                p_TextureViewSetDefaults(&views[mip][layer]);p_TextureViewSetLevels(&views[mip][layer],mip,1);
                p_TextureViewSetLayers(&views[mip][layer],layer,1);
            }
            for(unsigned test=0;test<sizeof cases/sizeof *cases;test++){
                p_CommandBufferAddCommandMemory(&cb,&copy_pool,0,131072);p_CommandBufferBeginRecording(&cb);
                for(unsigned image=0;image<2;image++)for(unsigned mip=0;mip<2;mip++)for(unsigned layer=0;layer<2;layer++){
                    unsigned w=32>>mip,h=24>>mip,tile=4>>mip;
                    p_CommandBufferSetRenderTargets(&cb,0,NULL,NULL,&textures[image],&views[mip][layer]);
                    if(image){
                        p_CommandBufferSetScissor(&cb,0,0,w,h);p_CommandBufferClearDepthStencil(&cb,.3125f,1,0xa6,255);
                    }else for(unsigned y=0;y<6;y++)for(unsigned x=0;x<8;x++){
                        uint32_t seed=(x*73856093u)^(y*19349663u)^(layer*83492791u)^(mip*2654435761u)^(test*2246822519u);
                        float z=(float)(seed&0xfffff)/1048575.f;
                        if(!x&&!y)z=0;if(x==7&&y==5)z=1;
                        unsigned stencil=(x*73+y*37+layer*91+mip*43+test*19)&255;
                        p_CommandBufferSetScissor(&cb,x*tile,y*tile,tile,tile);p_CommandBufferClearDepthStencil(&cb,z,1,stencil,255);
                    }
                }
                /* Same guest bindings on both sides of the internal utility.
                 * A red clear between draws makes a missing rebind observable. */
                NVNdepthStencilState copy_ds={0};p_DepthStencilStateSetDefaults(&copy_ds);p_CommandBufferBindDepthStencilState(&cb,&copy_ds);
                p_CommandBufferSetRenderTargets(&cb,1,targets,NULL,NULL,NULL);
                p_CommandBufferSetScissor(&cb,0,0,64,64);p_CommandBufferSetViewports(&cb,0,1,viewport);
                p_CommandBufferBindProgram(&cb,&graphics,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
                p_CommandBufferResetCounter(&cb,NVN_COUNTER_TYPE_SAMPLES_PASSED);
                p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
                NVNtextureView source_view=views[cases[test].sm][cases[test].sl],destination_view=views[cases[test].dm][cases[test].dl];
                p_TextureViewSetLayers(&source_view,cases[test].sl,cases[test].layers);
                p_TextureViewSetLayers(&destination_view,cases[test].dl,cases[test].layers);
                p_CommandBufferCopyTextureToTexture(&cb,&textures[0],&source_view,&cases[test].src,&textures[1],&destination_view,&cases[test].dst,
                    test==3?0:NVN_COPY_FLAGS_ENGINE_2D_BIT);
                const float copy_red[4]={1,0,0,1};p_CommandBufferClearColor(&cb,0,copy_red,15);
                p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
                p_CommandBufferReportCounter(&cb,NVN_COUNTER_TYPE_SAMPLES_PASSED,base+8192);
                handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
                uint64_t samples;memcpy(&samples,memory+8192,8);
                if(samples!=2*triangle_samples){fprintf(stderr,"DEPTH_COPY_QUERY samples=%llu expected=%u\n",(unsigned long long)samples,2*triangle_samples);return 152;}
                unsigned char after_copy[sizeof pixels];p_TextureReadTexels(&target,NULL,&region,after_copy);
                for(unsigned pixel=0;pixel<4096;pixel++)for(unsigned c=0;c<4;c++){
                    unsigned expected=saved_color[pixel*4]?saved_color[pixel*4+c]:copy_red[c]*255;
                    if(after_copy[pixel*4+c]!=expected){fprintf(stderr,"DEPTH_COPY_COLOR format=%u test=%u pixel=%u channel=%u got=%u expected=%u\n",
                        format_index,test,pixel,c,after_copy[pixel*4+c],expected);return 144;}
                }
                if(fwrite(after_copy,1,sizeof after_copy,copy_file)!=sizeof after_copy)return 145;
                for(unsigned image=0;image<2;image++)for(unsigned mip=0;mip<2;mip++)for(unsigned layer=0;layer<2;layer++){
                    unsigned w=32>>mip,h=24>>mip,pitch=w*bpp+16;unsigned char readback[24*(32*8+16)];memset(readback,0xcd,sizeof readback);
                    NVNcopyRegion read_region={0,0,0,(int)w,(int)h,1};
                    p_TextureReadTexelsStrided(&textures[image],&views[mip][layer],&read_region,readback,pitch,pitch*h);
                    for(unsigned y=0;y<h;y++){
                        for(unsigned pad=w*bpp;pad<pitch;pad++)if(readback[y*pitch+pad]!=0xcd)return 146;
                        if(fwrite(readback+y*pitch,1,w*bpp,copy_file)!=w*bpp)return 147;
                        read_bytes+=w*bpp;
                        for(unsigned x=0;x<w;x++){
                            float z=.3125f;unsigned stencil=0xa6,sx=x,sy=y,sm=mip,sl=layer;
                            int copied=image&&mip==cases[test].dm&&layer>=cases[test].dl&&layer<cases[test].dl+cases[test].layers&&
                                x>=(unsigned)cases[test].dst.xoffset&&x<(unsigned)(cases[test].dst.xoffset+cases[test].dst.width)&&
                                y>=(unsigned)cases[test].dst.yoffset&&y<(unsigned)(cases[test].dst.yoffset+cases[test].dst.height);
                            if(copied){
                                sx=cases[test].src.xoffset+((2*(x-cases[test].dst.xoffset)+1)*cases[test].src.width)/(2*cases[test].dst.width);
                                sy=cases[test].src.yoffset+((2*(y-cases[test].dst.yoffset)+1)*cases[test].src.height)/(2*cases[test].dst.height);
                                sm=cases[test].sm;sl=cases[test].sl+layer-cases[test].dl;
                            }
                            if(!image||copied){unsigned tile=4>>sm,tx=sx/tile,ty=sy/tile;
                                uint32_t seed=(tx*73856093u)^(ty*19349663u)^(sl*83492791u)^(sm*2654435761u)^(test*2246822519u);
                                z=(float)(seed&0xfffff)/1048575.f;
                                if(!tx&&!ty)z=0;if(tx==7&&ty==5)z=1;
                                stencil=(tx*73+ty*37+sl*91+sm*43+test*19)&255;}
                            const unsigned char *value=readback+y*pitch+x*bpp;int valid=1;
                            if(format_index==0){uint16_t got;memcpy(&got,value,2);valid=abs((int)got-(int)lroundf(z*65535.f))<=1;}
                            else if(format_index==2){uint32_t got;memcpy(&got,value,4);valid=llabs((long long)(got&0xffffff)-(long long)llround((double)z*16777215.))<=1&&got>>24==stencil;}
                            else{float got;memcpy(&got,value,4);valid=got==z;
                                if(format_index==3)valid=valid&&value[4]==stencil&&!value[5]&&!value[6]&&!value[7];}
                            if(!valid){fprintf(stderr,"DEPTH_COPY_VALUE format=%u test=%u image=%u mip=%u layer=%u xy=%u,%u expected_z=%g expected_stencil=%u\n",
                                format_index,test,image,mip,layer,x,y,z,stencil);return 148;}
                            read_texels++;
                        }
                    }
                }
            }
            for(unsigned image=0;image<2;image++)p_TextureFinalize(&textures[image]);
        }
        uint64_t plain,blits,draws;dm_vk_depth_copy_stats(&plain,&blits,&draws);
        if(plain!=4||blits+draws!=32)return 149;
        if(fclose(copy_file))return 150;
        /* Restore the base fixture's expected color for its subsequent tests. */
        p_CommandBufferAddCommandMemory(&cb,&copy_pool,0,131072);p_CommandBufferBeginRecording(&cb);
        p_CommandBufferClearColor(&cb,0,clear,15);p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
        handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
        p_TextureReadTexels(&target,NULL,&region,pixels);if(memcmp(pixels,saved_color,sizeof pixels))return 151;
        p_MemoryPoolFinalize(&copy_pool);free(copy_memory);
        fprintf(stderr,"DEPTH_COPY_DEVICE formats=4 cases=36 read_texels=%llu read_bytes=%llu plain=%llu blits=%llu draws=%llu color_rebind=36 occlusion=36 passed=1\n",
            (unsigned long long)read_texels,(unsigned long long)read_bytes,(unsigned long long)plain,(unsigned long long)blits,(unsigned long long)draws);
    }
