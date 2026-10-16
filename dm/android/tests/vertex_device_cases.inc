/* Every record has independently decoded CPU expectations. All vertices end at
 * the logical range AND physical pool boundary. Shader reads whole values and
 * dynamic components while retaining the original gl_VertexIndex builtin. */
    struct VertexCase { unsigned format,bytes;const char *shader;unsigned char payload[16];uint32_t expected[4]; };
    static const struct VertexCase vertex_cases[]={@VERTEX_CASES@};
    p_TextureFinalize(&target);
    p_TextureBuilderSetDefaults(&tb);p_TextureBuilderSetDevice(&tb,&device);
    p_TextureBuilderSetWidth(&tb,16);p_TextureBuilderSetHeight(&tb,16);
    p_TextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA32F);p_TextureBuilderSetStorage(&tb,&pool,16384);
    if(!p_TextureInitialize(&target,&tb))return 110;
    char vertex_file[1400];snprintf(vertex_file,sizeof vertex_file,"%s.vertex",argv[2]);FILE *vertex_output=fopen(vertex_file,"wb");if(!vertex_output)return 111;
    uint64_t vertex_compared=0;unsigned vertex_draws=0;
    int bad_vertex=getenv("DM_VERTEX_BAD_RANGE")?atoi(getenv("DM_VERTEX_BAD_RANGE")):0;
    for(unsigned ci=0;ci<sizeof vertex_cases/sizeof *vertex_cases;ci++){
        const struct VertexCase *test=&vertex_cases[ci];
        NVNprogram vertex_program={0};p_ProgramInitialize(&vertex_program,&device);
        NVNshaderData vertex_shaders[2]={{0,control(argv[1],test->shader)},{0,control(argv[1],"@FRAGMENT@")}};
        p_ProgramSetShaders(&vertex_program,2,vertex_shaders);
        for(unsigned layout=0;layout<4;layout++){
            if(bad_vertex&&layout!=(unsigned)(bad_vertex-1))continue;
            unsigned attr_offset=layout==1?4:0,stride=test->bytes+(layout==1?8:0),last=layout==1||layout==3?3:2;
            unsigned length=attr_offset+last*stride+test->bytes;
            unsigned start=262144-length;memset(memory+start,0xa5,length);
            for(unsigned v=0;v<=last;v++)memcpy(memory+start+attr_offset+v*stride,test->payload,test->bytes);
            NVNvertexAttribState va[3]={0};for(unsigned j=0;j<3;j++)p_VertexAttribStateSetDefaults(&va[j]);
            p_VertexAttribStateSetFormat(&va[2],test->format,attr_offset);
            NVNvertexStreamState vs_state={0};p_VertexStreamStateSetDefaults(&vs_state);
            p_VertexStreamStateSetStride(&vs_state,stride);p_VertexStreamStateSetDivisor(&vs_state,layout==3);
            const NVNbufferRange range={base+start,length-(bad_vertex?1:0)};
            uint16_t index16[3]={2,3,4};uint32_t index32[3]={0,1,2};
            memcpy(memory+122880,index16,sizeof index16);memcpy(memory+122896,index32,sizeof index32);
            p_CommandBufferAddCommandMemory(&cb,&pool,0,4096);p_CommandBufferBeginRecording(&cb);
            const NVNtexture *active[1]={&target};p_CommandBufferSetRenderTargets(&cb,1,active,NULL,NULL,NULL);
            const float view[4]={0,0,16,16};p_CommandBufferSetViewports(&cb,0,1,view);p_CommandBufferSetScissor(&cb,0,0,16,16);
            NVNdepthStencilState ds={0};p_DepthStencilStateSetDefaults(&ds);p_CommandBufferBindDepthStencilState(&cb,&ds);
            p_CommandBufferClearColor(&cb,0,clear,15);
            p_CommandBufferBindProgram(&cb,&vertex_program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
            p_CommandBufferBindVertexAttribState(&cb,3,va);p_CommandBufferBindVertexStreamState(&cb,1,&vs_state);
            p_CommandBufferBindVertexBuffers(&cb,0,1,&range);
            if(layout<2)p_CommandBufferDrawArrays(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,layout==1,3);
            else p_CommandBufferDrawElementsInstanced(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,layout==2?NVN_INDEX_TYPE_UNSIGNED_SHORT:NVN_INDEX_TYPE_UNSIGNED_INT,
                3,base+(layout==2?122880:122896),layout==2?-2:0,layout==3?2:0,layout==3?2:1);
            handle=p_CommandBufferEndRecording(&cb);p_QueueSubmitCommands(&queue,1,&handle);p_QueueFinish(&queue);
            float actual[16*16*4],expected[4];memcpy(expected,test->expected,sizeof expected);
            NVNcopyRegion vr={0,0,0,16,16,1};p_TextureReadTexels(&target,NULL,&vr,actual);
            for(unsigned pixel=0;pixel<256;pixel++)for(unsigned c=0;c<4;c++){
                float tolerance=fmaxf(1e-7f,fabsf(expected[c])*1e-7f);
                if(!isfinite(actual[pixel*4+c])||fabsf(actual[pixel*4+c]-expected[c])>tolerance){
                    fprintf(stderr,"VERTEX_MISMATCH case=%u format=%u layout=%u pixel=%u component=%u actual=%.9g expected=%.9g\n",ci,test->format,layout,pixel,c,actual[pixel*4+c],expected[c]);return 112;
                }
            }
            if(fwrite(actual,1,sizeof actual,vertex_output)!=sizeof actual)return 113;
            vertex_compared+=sizeof actual;vertex_draws++;
        }
        p_ProgramFinalize(&vertex_program);free((void *)vertex_shaders[0].control);free((void *)vertex_shaders[1].control);
    }
    fclose(vertex_output);
    fprintf(stderr,"VERTEX_DEVICE formats=79 input_widths=4 layouts=4 cases=%u read_bytes=%llu passed=1\n",vertex_draws,(unsigned long long)vertex_compared);
