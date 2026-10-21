/* Per-draw GPU timing diagnostic (DM_DRAW_TIMING, default off).
 *
 * One BOTTOM_OF_PIPE timestamp is written after every draw and one right after each
 * vkCmdBeginRenderPass. The time of a draw is the delta to the previous timestamp of the SAME
 * pass, so inter-pass barriers and copies are never charged to a draw. The first draw of a pass
 * also carries nothing from the load op because the pass-start stamp follows the begin.
 *
 * The stamps serialise draws that would normally overlap (vertex of draw N+1 with fragment of
 * draw N), so the sum is larger than the undisturbed pass time. Use the numbers to RANK draw
 * classes, not as absolute costs; compare the attributed total with PASS_TIME_TOTAL.
 *
 * Included by nvn_vk.c after the G/S definitions; nothing here runs without DM_DRAW_TIMING. */

typedef struct { char vs[13],ps[13];uint8_t n,depth,blend;uint32_t w,h;uint64_t draws,count,inst,vparea;double ms; } DrawAcc;
typedef struct { double ms;DrawTime d; } DrawTop;
#define DT_ACC_MAX 512
#define DT_TOP_MAX 24
static DrawAcc dt_acc[DT_ACC_MAX];
static uint32_t dt_nacc;
static DrawTop dt_top[DT_TOP_MAX];
static uint32_t dt_ntop;
static uint64_t dt_draws_total,dt_dropped,dt_passes_total;
static double dt_ms_total;
static uint32_t dt_cur_w,dt_cur_h;
static uint8_t dt_cur_n,dt_cur_depth;

static void dt_copy_key(char out[13],const char *key) {
    size_t n=key?strlen(key):0;if(n>12)n=12;
    if(n)memcpy(out,key,n);out[n]=0;
}
/* Recorder side. Called right after vkCmdBeginRenderPass. */
static void dt_mark_pass(uint32_t width,uint32_t height,uint32_t colors,int has_depth) {
    if(!S.draw_ts||!S.dt)return;
    dt_cur_w=width;dt_cur_h=height;dt_cur_n=(uint8_t)colors;dt_cur_depth=has_depth?1:0;S.dt_pass++;
    if(S.ndt>=DT_CAP){dt_dropped++;return;}
    vkCmdWriteTimestamp(S.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,S.draw_ts,S.ndt);
    DrawTime *e=&S.dt[S.ndt++];memset(e,0,sizeof *e);
    e->kind=0;e->pass=(uint16_t)S.dt_pass;e->w=width;e->h=height;e->n=(uint8_t)colors;e->depth=(uint8_t)(has_depth?1:0);
}
/* Called right after the draw command was recorded (inside the render pass). */
static void dt_mark_draw(const char *vs,const char *ps,uint32_t count,uint32_t instances) {
    if(!S.draw_ts||!S.dt||!S.in_pass)return;
    if(S.ndt>=DT_CAP){dt_dropped++;return;}
    vkCmdWriteTimestamp(S.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,S.draw_ts,S.ndt);
    DrawTime *e=&S.dt[S.ndt++];memset(e,0,sizeof *e);
    e->kind=1;e->pass=(uint16_t)S.dt_pass;e->w=dt_cur_w;e->h=dt_cur_h;e->n=dt_cur_n;e->depth=dt_cur_depth;
    dt_copy_key(e->vs,vs);dt_copy_key(e->ps,ps);
    e->count=count;e->instances=instances;e->blend=(uint8_t)(G.state.color.blend_en?1:0);
    e->dt=(uint8_t)(G.state.ds.dt?1:0);e->dw=(uint8_t)(G.state.ds.dw?1:0);
    e->vpw=(uint32_t)(G.state.viewport.width>0?G.state.viewport.width:0);e->vph=(uint32_t)(G.state.viewport.height>0?G.state.viewport.height:0);
}
static void dt_top_add(double ms,const DrawTime *d) {
    uint32_t k;
    if(dt_ntop<DT_TOP_MAX)k=dt_ntop++;
    else if(ms>dt_top[DT_TOP_MAX-1].ms)k=DT_TOP_MAX-1;
    else return;
    dt_top[k].ms=ms;dt_top[k].d=*d;
    while(k>0&&dt_top[k].ms>dt_top[k-1].ms){DrawTop t=dt_top[k];dt_top[k]=dt_top[k-1];dt_top[k-1]=t;k--;}
}
/* Completion side: the submission's fence has signalled, results are final. */
static void draw_timing_collect(VkSubmission *sub) {
    if(!sub->draw_ts||!sub->dt||!sub->ndt)return;
    uint32_t n=sub->ndt;uint64_t *ticks=malloc(sizeof(*ticks)*n);if(!ticks){sub->ndt=0;return;}
    VkResult r=vkGetQueryPoolResults(G.device,sub->draw_ts,0,n,sizeof(*ticks)*n,ticks,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);
    if(r==VK_SUCCESS) {
        uint64_t mask=G.timestamp_bits>=64?UINT64_MAX:((1ULL<<G.timestamp_bits)-1);
        double period=G.properties.limits.timestampPeriod/1e6;
        for(uint32_t i=0;i<n;i++) {
            const DrawTime *e=&sub->dt[i];
            if(e->kind==0){dt_passes_total++;continue;}
            if(!i||sub->dt[i-1].pass!=e->pass)continue;
            double ms=(double)((ticks[i]-ticks[i-1])&mask)*period;
            dt_ms_total+=ms;dt_draws_total++;
            uint32_t k=0;
            while(k<dt_nacc&&!(!strcmp(dt_acc[k].vs,e->vs)&&!strcmp(dt_acc[k].ps,e->ps)&&dt_acc[k].w==e->w&&dt_acc[k].h==e->h&&
                  dt_acc[k].n==e->n&&dt_acc[k].depth==e->depth&&dt_acc[k].blend==e->blend))k++;
            if(k==dt_nacc){if(k==DT_ACC_MAX){dt_dropped++;continue;}
                memset(&dt_acc[k],0,sizeof dt_acc[k]);memcpy(dt_acc[k].vs,e->vs,13);memcpy(dt_acc[k].ps,e->ps,13);
                dt_acc[k].w=e->w;dt_acc[k].h=e->h;dt_acc[k].n=e->n;dt_acc[k].depth=e->depth;dt_acc[k].blend=e->blend;dt_nacc++;}
            dt_acc[k].ms+=ms;dt_acc[k].draws++;dt_acc[k].count+=e->count;dt_acc[k].inst+=e->instances;
            dt_acc[k].vparea+=(uint64_t)e->vpw*e->vph;
            dt_top_add(ms,e);
        }
    }
    free(ticks);sub->ndt=0;sub->dt_pass=0;
}
static int dt_acc_cmp(const void *a,const void *b) {
    double x=((const DrawAcc *)a)->ms,y=((const DrawAcc *)b)->ms;return x<y?1:x>y?-1:0;
}
static void dt_print_group(const char *tag,unsigned rank,const DrawAcc *a) {
    fprintf(stderr,"[nvn-vulkan] %s rank=%u vs=%s ps=%s size=%ux%u colors=%u depth=%u blend=%u draws=%llu ms=%.3f avg_us=%.1f avg_count=%.0f avg_inst=%.1f avg_vp_kpx=%.1f\n",
        tag,rank,a->vs[0]?a->vs:"-",a->ps[0]?a->ps:"-",a->w,a->h,a->n,a->depth,a->blend,(unsigned long long)a->draws,a->ms,
        a->draws?a->ms*1000.0/(double)a->draws:0.0,a->draws?(double)a->count/(double)a->draws:0.0,
        a->draws?(double)a->inst/(double)a->draws:0.0,a->draws?(double)a->vparea/(double)a->draws/1000.0:0.0);
}
/* Window report, then reset. frames = frames in the window. */
static void draw_timing_report(uint64_t frame,uint64_t frames) {
    if(!G.draw_timing)return;
    fprintf(stderr,"[nvn-vulkan] DRAW_TIME_TOTAL frame=%llu frames=%llu passes=%llu draws=%llu attributed_ms=%.3f groups=%u dropped=%llu\n",
        (unsigned long long)frame,(unsigned long long)frames,(unsigned long long)dt_passes_total,(unsigned long long)dt_draws_total,dt_ms_total,dt_nacc,(unsigned long long)dt_dropped);
    qsort(dt_acc,dt_nacc,sizeof dt_acc[0],dt_acc_cmp);
    for(uint32_t k=0;k<dt_nacc&&k<30;k++)dt_print_group("DRAW_TIME_GROUP",k,&dt_acc[k]);
    /* Same data folded by pixel shader only, ignoring pass size. */
    static DrawAcc byps[DT_ACC_MAX];uint32_t nps=0;
    for(uint32_t k=0;k<dt_nacc;k++){uint32_t j=0;while(j<nps&&strcmp(byps[j].ps,dt_acc[k].ps))j++;
        if(j==nps){memset(&byps[j],0,sizeof byps[j]);memcpy(byps[j].ps,dt_acc[k].ps,13);nps++;}
        byps[j].ms+=dt_acc[k].ms;byps[j].draws+=dt_acc[k].draws;byps[j].count+=dt_acc[k].count;byps[j].inst+=dt_acc[k].inst;byps[j].vparea+=dt_acc[k].vparea;}
    qsort(byps,nps,sizeof byps[0],dt_acc_cmp);
    for(uint32_t k=0;k<nps&&k<20;k++)dt_print_group("DRAW_TIME_PS",k,&byps[k]);
    for(uint32_t k=0;k<dt_ntop;k++){const DrawTime *d=&dt_top[k].d;
        fprintf(stderr,"[nvn-vulkan] DRAW_TIME_TOP rank=%u ms=%.3f vs=%s ps=%s pass=%u size=%ux%u colors=%u depth=%u blend=%u ztest=%u zwrite=%u count=%u inst=%u vp=%ux%u\n",
            k,dt_top[k].ms,d->vs,d->ps,d->pass,d->w,d->h,d->n,d->depth,d->blend,d->dt,d->dw,d->count,d->instances,d->vpw,d->vph);}
    dt_nacc=0;dt_ntop=0;dt_draws_total=dt_dropped=dt_passes_total=0;dt_ms_total=0;
}
