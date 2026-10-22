/* Original-draw replay, never a screen capture or a substitute icon. Included
 * after draw(); recording remains under G.lock. The request lock is never
 * acquired while holding the touch-state lock. */
static int ghost_request_read(GhostRequest *r){
    if(!atomic_load_explicit(&ghost_on,memory_order_acquire))return 0;
    pthread_mutex_lock(&ghost_lock);*r=ghost_request;pthread_mutex_unlock(&ghost_lock);
    return r->on&&(!r->live||atomic_load_explicit(r->live,memory_order_acquire)==r->token);
}
void dm_vk_ui_ghost_request(int on,float px,float py,const float grid[4],float cx,float cy,int dim,
        const _Atomic uint64_t *live,uint64_t token){
    GhostRequest r={.on=on&&grid,.origin={px,py},.dest={cx,cy},.dim=dim,.live=live,.token=token};
    if(r.on){memcpy(r.grid,grid,sizeof r.grid);for(unsigned i=0;i<4;i++)if(!isfinite(grid[i]))r.on=0;
        if(!isfinite(px)||!isfinite(py)||!isfinite(cx)||!isfinite(cy)||grid[2]<=0||grid[3]<=0)r.on=0;}
    pthread_mutex_lock(&ghost_lock);
    if(!r.on||!ghost_request.on||r.live!=ghost_request.live||r.token!=ghost_request.token||
       memcmp(r.origin,ghost_request.origin,sizeof r.origin)||memcmp(r.grid,ghost_request.grid,sizeof r.grid))
        ++ghost_serial;
    r.serial=ghost_serial;ghost_request=r;if(!r.on)ghost_found_valid=0;
    atomic_store_explicit(&ghost_on,r.on,memory_order_release);pthread_mutex_unlock(&ghost_lock);
}
void mtl_ui_ghost(int on,float px,float py,const float grid[4],float cx,float cy){
    dm_vk_ui_ghost_request(on,px,py,grid,cx,cy,0,NULL,0);
}
int mtl_ui_ghost_rect(float r[4]){
    GhostRequest q;if(!r||!ghost_request_read(&q))return 0;
    pthread_mutex_lock(&ghost_lock);int have=ghost_found_valid&&ghost_found_serial==q.serial;
    if(have)memcpy(r,ghost_found,sizeof ghost_found);pthread_mutex_unlock(&ghost_lock);return have;
}
static void ghost_clear(void){for(unsigned i=0;i<ghost.n;i++)free(ghost.c[i]);ghost.n=ghost.nt=0;}
/* Only CPU-known reads are eligible. Exact queued CPU updates are overlaid in
 * order; a GPU producer causes this optional overlay candidate to be rejected. */
static int ghost_cpu_read(Range r,void *out){
    uint64_t off=0;VPool *p=pool_find(r.addr,r.size,&off);if(!p||(!G.buffer_versions&&!p->v.map))return 0;
    for(int i=0;i<S.nwrites;i++)if(!S.write_cpu[i]&&overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size))return 0;
    /* 等待旧槽发布，再读 guest；录制中的 CPU 更新仍按顺序覆盖。
     * Wait for older slot publication before reading guest bytes; recorded CPU updates remain ordered overlays.
     */
    if(G.buffer_versions)pool_conflict_wait(p,off,r.size,0);
    memcpy(out,G.buffer_versions?(const void *)(uintptr_t)r.addr:p->v.map+off,r.size);
    for(int i=0;i<S.nwrites;i++)if(S.write_cpu[i]&&overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size)){
        Range w=S.writes[i];uint64_t lo=r.addr>w.addr?r.addr:w.addr,hi=r.addr+r.size<w.addr+w.size?r.addr+r.size:w.addr+w.size;
        memcpy((uint8_t *)out+lo-r.addr,S.write_cpu[i]+lo-w.addr,hi-lo);
    }
    return 1;
}
static int ghost_buffer_add(GhostCandidate *c,Range r){
    if(!r.addr&&!r.size)return 1;
    if(!r.addr||!r.size||r.size>65536||c->bytes>262144-r.size)return 0;
    for(unsigned i=0;i<c->nb;i++)if(c->buffer[i].range.addr==r.addr&&c->buffer[i].range.size==r.size)return 1;
    if(c->nb==34)return 0;
    /* Check ownership before allocating a fence-owned snapshot. */
    uint8_t *bytes=malloc(r.size);if(!bytes)return 0;
    if(!ghost_cpu_read(r,bytes)){free(bytes);return 0;}
    VBuffer *v=temporary(r.size,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(v->map,bytes,r.size);free(bytes);
    GhostBuffer *b=&c->buffer[c->nb++];b->range=r;b->pool=(VPool){.guest=(const uint8_t *)(uintptr_t)r.addr,.size=r.size,.v=*v};c->bytes+=r.size;return 1;
}
static VPool *ghost_pool_override(uint64_t addr,uint64_t len,uint64_t *off){
    if(!ghost.busy||!ghost.replaying)return NULL;
    GhostCandidate *c=ghost.replaying;
    for(unsigned i=0;i<c->nb;i++){GhostBuffer *b=&c->buffer[i];
        if(addr>=b->range.addr&&addr-b->range.addr<=b->range.size&&len<=b->range.size-(addr-b->range.addr)){
            *off=addr-b->range.addr;return &b->pool;
        }
    }
    /* Eligible shaders have no other mutable buffer reads. A missed range is
     * an internal error; falling back would silently replay newer CPU data. */
    die("UI ghost unsnapshotted buffer",addr);
}
static int ghost_shader_capture(GhostCandidate *c,VShader *s){
    for(unsigned i=0;i<s->h.nb;i++){RBinding *b=&s->b[i];
        if(b->kind==2||b->kind==9){
            if(b->flags||b->slot<0||b->slot>=16||!ghost_buffer_add(c,G.state.uniform[s->h.stage][b->slot]))return 0;
        }else if(b->kind==4){
            if(b->slot<0||b->slot>=128||c->nt==16)return 0;
            uint64_t h=G.state.tex[s->h.stage][b->slot];unsigned id=h&0xfffff;
            if(id>=G.texture_slots||!G.tslot[id].live)return 0;
            TSlot *slot=&G.tslot[id];VTexture *t=slot_texture(slot,0);
            if(t->source.serial==G.state.rt.color[0]->serial||aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)return 0;
            unsigned sid=(unsigned)((b->sampler>=0?G.state.samp[s->h.stage][b->sampler]:h)>>20)&0xfff;
            VkSampler sampler=sid<LIMIT_OBJECTS&&G.sslot[sid].live?G.sslot[sid].sampler:VK_NULL_HANDLE;
            c->texture[c->nt++]=(GhostTexture){id,sid,slot->revision,t->serial,t->incarnation,t->version,sampler};
        }else if(b->kind!=1&&b->kind!=7&&b->kind!=8)return 0;
    }
    return 1;
}
static int ghost_textures_valid(const GhostCandidate *c){
    for(unsigned i=0;i<c->nt;i++){const GhostTexture *v=&c->texture[i];
        if(v->id>=G.texture_slots||!G.tslot[v->id].live||G.tslot[v->id].revision!=v->revision)return 0;
        VTexture *t=slot_texture(&G.tslot[v->id],0);
        if(t->serial!=v->serial||t->incarnation!=v->incarnation||t->version!=v->version)return 0;
        VkSampler sampler=v->sid<LIMIT_OBJECTS&&G.sslot[v->sid].live?G.sslot[v->sid].sampler:VK_NULL_HANDLE;
        if(sampler!=v->sampler)return 0;
    }
    return 1;
}
static void ghost_note(const p_draw *a,const p_draw_idx *idx,int instances,int base_instance,VShader *vs,VShader *ps){
    if(ghost.busy||!atomic_load_explicit(&ghost_on,memory_order_acquire)||!a||idx||a->first||a->count!=4||a->prim!=NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP||instances!=1||base_instance||
       !ps||vs->h.na!=2||G.state.streams[0].stride!=20||G.state.streams[0].divisor||G.state.vbuf[0].size!=80||
       G.state.rt.n!=1||!G.state.rt.color[0]||G.state.rt.depth||G.state.ds.dt||G.state.ds.dw||G.state.ds.st||
       S.depth_input||S.feedback_flags||!S.read_snapshots_allowed||!S.in_pass)return;
    for(unsigned i=0;i<2;i++){unsigned l=vs->attr[i][0];if(l>=16||G.state.attributes[l].stream||G.state.attributes[l].fmt==NVN_FORMAT_NONE)return;}
    GhostRequest q;if(!ghost_request_read(&q))return;
    if(ghost.pass!=G.pass_count||ghost.epoch!=S.epoch||ghost.serial!=q.serial){ghost_clear();ghost.pass=G.pass_count;ghost.epoch=S.epoch;ghost.serial=q.serial;}
    uint64_t texture=0;for(unsigned i=0;i<ps->h.nb;i++)if(ps->b[i].kind==4&&ps->b[i].slot>=0&&ps->b[i].slot<128){texture=G.state.tex[1][ps->b[i].slot];break;}
    if(!texture)return;
    unsigned k=0;while(k<ghost.nt&&ghost.texture[k]!=texture)k++;
    if(k==ghost.nt&&ghost.nt<128){ghost.texture[k]=texture;ghost.count[k]=0;ghost.nt++;}if(k<ghost.nt)ghost.count[k]++;
    Range u=G.state.uniform[0][0];uint8_t vertices[80];float matrix[16],r[4];
    if(u.size<64||!ghost_cpu_read((Range){u.addr,64},matrix)||!ghost_cpu_read((Range){G.state.vbuf[0].address,80},vertices))return;
    FramebufferKey *fb=&S.framebuffer_keys[S.active_pass];float vp[4]={G.state.viewport.x,G.state.viewport.y,G.state.viewport.width,G.state.viewport.height};
    if(!nvn_vk_ui_ghost_rect(vertices,matrix,vp,fb->width,fb->height,atomic_load(&G.window_origin)==NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT,r)||
       !nvn_vk_ui_ghost_matches(r,q.grid,q.origin[0],q.origin[1]))return;
    float area=(r[2]-r[0])*(r[3]-r[1]);if(ghost.n&&area>ghost.area*1.03f)return;
    GhostCandidate *c=calloc(1,sizeof(*c));if(!c)return;
    c->state=G.state;c->draw=*a;c->tex=texture;memcpy(c->rect,r,sizeof r);
    if(!ghost_buffer_add(c,(Range){G.state.vbuf[0].address,80})||!ghost_shader_capture(c,vs)||!ghost_shader_capture(c,ps)){free(c);return;}
    if(ghost.n&&area<ghost.area*.97f){for(unsigned i=0;i<ghost.n;i++)free(ghost.c[i]);ghost.n=0;}
    if(ghost.n==4){free(ghost.c[0]);memmove(ghost.c,ghost.c+1,3*sizeof ghost.c[0]);ghost.n=3;}
    ghost.area=area;ghost.c[ghost.n++]=c;
}
static void ghost_flush(void){
    if(ghost.busy||!ghost.n)return;
    GhostRequest q;if(!ghost_request_read(&q)||q.serial!=ghost.serial||ghost.pass!=G.pass_count||ghost.epoch!=S.epoch){ghost_clear();return;}
    int best=-1;unsigned count=UINT32_MAX;
    for(unsigned i=0;i<ghost.n;i++){unsigned n=0;for(unsigned k=0;k<ghost.nt;k++)if(ghost.texture[k]==ghost.c[i]->tex)n=ghost.count[k];
        if(n<count&&ghost_textures_valid(ghost.c[i])){best=i;count=n;}}
    if(best<0){ghost_clear();return;}
    GhostCandidate *c=ghost.c[best];ReplayState saved=G.state;
    /* A pass may end halfway through preparing the following game's draw.
     * Preserve its descriptor/alias bookkeeping as well as ReplayState. In
     * particular, replay must not consume its pending aligned writebacks. */
    AlignedBinding aligned[2*LIMIT_BINDINGS];memcpy(aligned,S.aligned,sizeof aligned);int naligned=S.naligned;
    ImageUse image_uses[2*LIMIT_BINDINGS];memcpy(image_uses,S.image_uses,sizeof image_uses);unsigned nimage_uses=S.nimage_uses;
    uint32_t storage_bias[6][LIMIT_BINDINGS];memcpy(storage_bias,S.storage_bias,sizeof storage_bias);
    uint64_t null_bindings[6];memcpy(null_bindings,S.null_bindings,sizeof null_bindings);
    int snapshots=S.read_snapshots_allowed,depth_sampled=S.depth_sampled,draw_store=G.draw_store,xition=G.xition_vertex,side_effects=G.pass_side_effects;
    VkPipelineCreateFlags feedback=S.feedback_flags;
    int trace=G.trace_frame,capture=G.capture_feedback;
    int plans=G.disable_binding_plans,driver=G.disable_driver_table_reuse,fixed=G.diag_fixed_desc,no_query=G.diag_no_occlusion,reuse=G.disable_pass_reuse;
    occlusion_end();ghost.busy=1;ghost.replaying=c;G.disable_binding_plans=G.disable_driver_table_reuse=G.diag_no_occlusion=1;G.diag_fixed_desc=0;G.disable_pass_reuse=0;
    S.naligned=0;G.trace_frame=G.capture_feedback=0;
    G.state=c->state;
    FramebufferKey *fb=&S.framebuffer_keys[S.active_pass];float width=fb->width,height=fb->height;
    G.state.scissor=(VkRect2D){{0,0},{fb->width,fb->height}};
    if(q.dim){
        /* The original quad supplies coverage. Multiplying the destination by
         * .18 is the iOS black overlay at alpha .82, while preserving alpha. */
        G.state.color.blend_en|=1;G.state.blend[0]=(blend_st){.src_rgb=NVN_BLEND_FUNC_ZERO,.dst_rgb=NVN_BLEND_FUNC_CONSTANT_COLOR,
            .src_a=NVN_BLEND_FUNC_ZERO,.dst_a=NVN_BLEND_FUNC_ONE,.eq_rgb=NVN_BLEND_EQUATION_ADD,.eq_a=NVN_BLEND_EQUATION_ADD};
        G.state.blend_color[0]=G.state.blend_color[1]=G.state.blend_color[2]=.18f;G.state.blend_color[3]=1;
        draw(&c->draw,NULL,1,0);G.state=c->state;G.state.scissor=(VkRect2D){{0,0},{(uint32_t)width,(uint32_t)height}};
    }
    G.state.viewport.x+=(q.dest[0]-(c->rect[0]+c->rect[2])*.5f)*width;
    G.state.viewport.y+=(q.dest[1]-(c->rect[1]+c->rect[3])*.5f)*height;
    draw(&c->draw,NULL,1,0);
    G.state=saved;G.disable_binding_plans=plans;G.disable_driver_table_reuse=driver;G.diag_fixed_desc=fixed;G.diag_no_occlusion=no_query;G.disable_pass_reuse=reuse;
    memcpy(S.aligned,aligned,sizeof aligned);S.naligned=naligned;memcpy(S.image_uses,image_uses,sizeof image_uses);S.nimage_uses=nimage_uses;
    memcpy(S.storage_bias,storage_bias,sizeof storage_bias);memcpy(S.null_bindings,null_bindings,sizeof null_bindings);
    S.read_snapshots_allowed=snapshots;S.depth_sampled=depth_sampled;S.feedback_flags=feedback;
    G.draw_store=draw_store;G.xition_vertex=xition;G.pass_side_effects=side_effects;G.trace_frame=trace;G.capture_feedback=capture;
    memset(G.driver_cache,0,sizeof G.driver_cache);for(unsigned i=0;i<2;i++)G.binding_revision[i]++;
    nvn_vk_command_state_reset(&S.command_state);ghost.replaying=NULL;ghost.busy=0;
    pthread_mutex_lock(&ghost_lock);if(ghost_request.serial==q.serial&&atomic_load(&ghost_on)){
        memcpy(ghost_found,c->rect,sizeof ghost_found);ghost_found_serial=q.serial;ghost_found_valid=1;
    }pthread_mutex_unlock(&ghost_lock);
    static unsigned logged;if(!logged++)fprintf(stderr,"[nvn-vulkan] UI_GHOST original_draw=1 immutable_buffers=1 candidate_count=%u texture_uses=%u dim=%d\n",ghost.n,count,q.dim);
    ghost_clear();
}
