/* Adapted from vickwv/android-integration coverage table (f569f68..da548c2).
 * New evidence namespace: format-aware, pass-local, subresource and slot keys. */
#define COV_MAX 4096
#define COV_SLOTS 8192
#define COV_GREEN 1
#define COV_RED 0
/* 列：pk / w / h / format / is_depth / verdict / samples / uncovered / total */
#define COV_COLUMNS "pk\tw\th\tformat\tis_depth\tverdict\tsamples\tuncovered\ttotal"
typedef struct {
    uint64_t key;             /* 归一化 PassKey 哈希 | normalized PassKey hash */
    uint32_t w,h,format;      /* 附件实际尺寸 + Vulkan 格式（PassKey 里没有尺寸） | extent + format */
    uint8_t is_depth,verdict; /* COV_GREEN / COV_RED（一旦判红不再变绿）| sticky red */
    uint32_t samples;
    uint64_t uncovered,total;
} CovRec;
static char g_cov_path[1400];static uint32_t g_cov_identity;
static CovRec *g_cov; static unsigned g_cov_n; static int g_cov_dirty;
static uint32_t g_cov_slot[COV_SLOTS];
static int g_cov_quarantined;  /* 冲突后本局禁用；运行中开关不得把它重新打开 | set on COV_CONFLICT; the runtime switch must not re-enable it */
static unsigned g_cov_min_samples=8;   /* 一条 green 至少要被独立探针命中几次才敢用（DM_COV_MIN_SAMPLES）；8 次是为了过滤「某个场景碰巧全覆盖」——实测有过 6 次全绿、第 10 次漏 1 像素的键 | samples before a green may be trusted: one key looked green 6 times and leaked a pixel on the 10th */
static int g_cov_loaded;
static uint64_t g_cov_green_bytes,g_cov_red_bytes,g_cov_miss_bytes,g_cov_weak_bytes;   /* 查表决策记账 | decision accounting, windowed */
static uint64_t g_cov_green_n,g_cov_red_n,g_cov_miss_n,g_cov_weak_n;
static uint64_t cov_key_of(const PassKey *pk,uint32_t w,uint32_t h,uint32_t format,int is_depth,uint32_t slot,uint32_t mip,uint32_t layer) {
    PassKey c=*pk;
    for(uint32_t i=0;i<c.count&&i<9;i++){
        c.attachments[i].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;c.attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        c.attachments[i].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;c.attachments[i].stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;}
    uint64_t x=1469598103934665603ULL;const uint8_t *b=(const uint8_t *)&c;
    for(size_t i=0;i<sizeof c;i++){x^=b[i];x*=1099511628211ULL;}
    const uint64_t tail[6]={(uint64_t)w<<32|h,(uint64_t)format,(uint64_t)(is_depth?1:0),slot,mip,layer};
    const uint8_t *t=(const uint8_t *)tail;
    for(size_t i=0;i<sizeof tail;i++){x^=t[i];x*=1099511628211ULL;}
    return x?x:1;
}
static CovRec *cov_lookup(uint64_t key,uint32_t w,uint32_t h,uint32_t format,int is_depth,int create) {
    uint64_t x=key^((uint64_t)w*0x9e3779b97f4a7c15ULL)^((uint64_t)h*0xc2b2ae3d27d4eb4fULL)
              ^((uint64_t)format*0x165667b19e3779f9ULL)^(uint64_t)(is_depth?1:0);
    uint32_t s=(uint32_t)(x^(x>>29))&(COV_SLOTS-1);
    for(uint32_t p=0;p<64;p++){
        uint32_t i=(s+p)&(COV_SLOTS-1),idx=g_cov_slot[i];
        if(!idx){
            if(!create||g_cov_n==COV_MAX)return NULL;
            if(!g_cov){g_cov=calloc(COV_MAX,sizeof(*g_cov));if(!g_cov)return NULL;}
            CovRec *r=&g_cov[g_cov_n];memset(r,0,sizeof *r);
            r->key=key;r->w=w;r->h=h;r->format=format;r->is_depth=(uint8_t)(is_depth!=0);r->verdict=COV_GREEN;
            g_cov_slot[i]=++g_cov_n;return r;
        }
        CovRec *r=&g_cov[idx-1];
        if(r->key==key&&r->w==w&&r->h==h&&r->format==format&&r->is_depth==(uint8_t)(is_depth!=0))return r;
    }
    return NULL;   /* 64 次探测都满：放弃这条（保守路径会用 LOAD）| table full at this cluster */
}
/* 探针结果入表。规则：只要有过一个未覆盖像素就判红且不再回绿——800x450 漏 26 个像素就会出
   花点（附十五），按像素比例给容差是在赌运气。 | Any uncovered pixel latches RED: 26 leaked
   pixels on an 800x450 buffer produced visible speckles, so ratio-based tolerances are gambling. */
static void cov_flush(void);
static void cov_note(uint64_t key,uint32_t w,uint32_t h,uint32_t format,int is_depth,uint64_t uncovered,uint64_t total) {
    CovRec *r=cov_lookup(key,w,h,format,is_depth,1); if(!r)return;
    int was_green = (r->verdict==COV_GREEN && r->samples>0);
    r->samples++; r->uncovered+=uncovered; r->total+=total;
    if(uncovered && r->verdict!=COV_RED){
        r->verdict=COV_RED;
        if(was_green && g_cov_table){
            fprintf(stderr,"[nvn-vulkan] COV_CONFLICT pk=%016llx rt=%ux%u fmt=%u d=%d：表里 green，探针实测漏 %llu/%llu → 改判 red 并停用本局查表（宁可慢，不许花屏）\n",
                (unsigned long long)key,w,h,format,is_depth,(unsigned long long)uncovered,(unsigned long long)total);
            g_cov_table=0;
            g_cov_quarantined=1;
        }
        fprintf(stderr,"[nvn-vulkan] COV_RED pk=%016llx rt=%ux%u fmt=%u d=%d uncovered=%llu/%llu 该键保持 LOAD\n",
            (unsigned long long)key,w,h,format,is_depth,(unsigned long long)uncovered,(unsigned long long)total);
    }
    g_cov_dirty=1;
    /* 判红/冲突是重要证据，立即落盘；否则要等 300 帧的周期写盘，进程被杀就丢了这次「这键不能用」的结论。 */
    if(uncovered) cov_flush();
}
/* 一行 TSV → CovRec。列顺序见 COV_COLUMNS；缺列按 0 处理，verdict 只认首字母 g/G 为 green，
   其他一律当红（保守）。返回 0 = 不是数据行（注释/表头/坏行）。 */
static int cov_parse_line(char *line,CovRec *r) {
    while(*line==' '||*line=='\t')line++;
    if(!*line||*line=='#'||*line=='\n'||*line=='\r')return 0;
    char *f[9]={0};int nf=0;
    for(char *p=line;;){
        f[nf++]=p;char *t=strchr(p,'\t');if(!t)break;*t=0;p=t+1;if(nf==9)break;
    }
    if(nf!=9)return 0;
    if(strlen(f[0])!=16||strspn(f[0],"0123456789abcdefABCDEF")!=16)return 0;
    for(int i=1;i<9;i++)if(i!=5){size_t n=strcspn(f[i],"\r\n");if(!n||strspn(f[i],"0123456789")!=n)return 0;}
    memset(r,0,sizeof *r);
    r->key=strtoull(f[0],NULL,16);
    r->w=(uint32_t)strtoul(f[1],NULL,10);
    r->h=(uint32_t)strtoul(f[2],NULL,10);
    r->format=(uint32_t)strtoul(f[3],NULL,10);
    r->is_depth=(uint8_t)(strtoul(f[4],NULL,10)!=0);
    r->verdict=(uint8_t)((f[5][0]=='g'||f[5][0]=='G')?COV_GREEN:COV_RED);
    if(nf>6)r->samples=(uint32_t)strtoul(f[6],NULL,10);
    if(nf>7)r->uncovered=strtoull(f[7],NULL,10);
    if(nf>8)r->total=strtoull(f[8],NULL,10);
    return r->key&&r->w&&r->h&&r->is_depth<=1&&r->samples&&r->total&&r->uncovered<=r->total;
}
static void cov_read_file(const char *path) {
    FILE *f=fopen(path,"r"); if(!f)return;
    char line[512],expected[128];unsigned rows=0,added=0;CovRec r;
    snprintf(expected,sizeof expected,"# d2r-coverage-v2 identity=%08x\n",g_cov_identity);
    if(!fgets(line,sizeof line,f)||strcmp(line,expected)){fclose(f);return;}
    while(fgets(line,sizeof line,f)){
        if(!cov_parse_line(line,&r))continue;
        rows++;
        CovRec *e=cov_lookup(r.key,r.w,r.h,r.format,r.is_depth,1); if(!e)continue;
        if(!e->samples){ *e=r; added++; }
        else{ e->samples+=r.samples; e->uncovered+=r.uncovered; e->total+=r.total;
              if(r.verdict==COV_RED)e->verdict=COV_RED; }
    }
    fclose(f);
    fprintf(stderr,"[nvn-vulkan] 覆盖率表读入 %s：%u 行（新增 %u，累计 %u）\n",path,rows,added,g_cov_n);
}
static void cov_load(void) {
    if(g_cov_loaded)return;g_cov_loaded=1;
    const char *root=getenv("DM_FRAME_DIR");if(!root)return;
    uint8_t encoded[NVN_VK_CACHE_KEY_BYTES];nvn_vk_cache_key_encode(encoded,&G.pipeline_cache_key);
    g_cov_identity=crc32(0,encoded,sizeof encoded);
    g_cov_identity=crc32(g_cov_identity,(const uint8_t *)&PW.identity.shader_crc,sizeof PW.identity.shader_crc);
    if(snprintf(g_cov_path,sizeof g_cov_path,"%s/pass-coverage-v2-%08x.tsv",root,g_cov_identity)>=(int)sizeof g_cov_path)return;
    cov_read_file(g_cov_path);
    fprintf(stderr,"[nvn-vulkan] COV_TABLE version=2 entries=%u enabled=%d old_evidence_ignored=1\n",g_cov_n,g_cov_table);
}
static void cov_flush(void) {
    if(!g_cov_dirty||!g_cov||!g_cov_n||!g_cov_path[0])return;
    const char *path=g_cov_path;
    char tmp[sizeof g_cov_path+8]; snprintf(tmp,sizeof tmp,"%s.tmp",path);
    FILE *f=fopen(tmp,"w"); if(!f)return;
    int ok=fprintf(f,"# d2r-coverage-v2 identity=%08x\n",g_cov_identity)>0;
    ok=ok&&fprintf(f,"# d2r-mobile 附件覆盖率表 | attachment coverage table\n"
                     "# green = 探针实测该通道首绘画满整个附件，颜色附件可以 loadOp=DONT_CARE\n"
                     "# red   = 测到过未覆盖像素，或没测过（保守）；这类键一律 LOAD\n"
                     "# green 至少要有 DM_COV_MIN_SAMPLES（缺省 8）次独立采样；表里有 green 但探针又测到漏像素时，\n"
                     "# 本局立即改判 red 并停用查表（COV_CONFLICT），下一次运行从这里读到的是 red。\n"
                     "%s\n",COV_COLUMNS)>0;
    for(unsigned i=0;ok&&i<g_cov_n;i++){
        CovRec *r=&g_cov[i];
        if(fprintf(f,"%016llx\t%u\t%u\t%u\t%d\t%s\t%u\t%llu\t%llu\n",
            (unsigned long long)r->key,r->w,r->h,r->format,r->is_depth?1:0,
            r->verdict==COV_GREEN?"green":"red",r->samples,
            (unsigned long long)r->uncovered,(unsigned long long)r->total)<0)ok=0;
    }
    if(fclose(f)!=0)ok=0;
    if(ok&&rename(tmp,path)==0){ g_cov_dirty=0; }
    else fprintf(stderr,"[nvn-vulkan] 覆盖率表写入失败 %s\n",path);
}

static struct {uint64_t load,store,clear,skip,passes;} cov_traffic;
#include "nvn_vk_coverage_readback.inc"
static void cov_probe_complete(VkSubmission *sub) {
    for(unsigned i=0;i<sub->ncoverage;i++){
        VBuffer *b=sub->coverage[i].buffer;if(sub->coverage[i].texture)die("coverage readback missing",i);
        if(b->cached){VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=b->m,.size=VK_WHOLE_SIZE};
            CHECK(vkInvalidateMappedMemoryRanges(G.device,1,&range));}
        uint64_t total=(uint64_t)sub->coverage[i].w*sub->coverage[i].h;
        uint64_t uncovered=nvn_cov_uncovered(b->map,total,sub->coverage[i].sentinel);
        fprintf(stderr,"[nvn-vulkan] PASS_SENTINEL_PROBE version=2 pass=%llu slot=%u rt=%ux%u fmt=%u mip=%u layer=%u pk=%016llx d=0 uncovered=%llu/%llu selftest=%d\n",
            (unsigned long long)sub->coverage[i].pass,sub->coverage[i].slot,sub->coverage[i].w,sub->coverage[i].h,
            sub->coverage[i].format,sub->coverage[i].mip,sub->coverage[i].layer,(unsigned long long)sub->coverage[i].key,
            (unsigned long long)uncovered,(unsigned long long)total,g_cov_selftest);
        if(g_cov_selftest){if(uncovered!=total){g_cov_table=0;g_cov_quarantined=1;die("coverage positive control failed",i);}}
        else cov_note(sub->coverage[i].key,sub->coverage[i].w,sub->coverage[i].h,sub->coverage[i].format,0,uncovered,total);
    }
    sub->ncoverage=0;sub->coverage_bytes=0;
}
static void cov_frame_report(uint64_t frame) {
    if(frame%60)return;
    if(g_cov_probe||g_cov_table||g_cov_traffic_on)
        fprintf(stderr,"[nvn-vulkan] RENDER_TRAFFIC frame=%llu attachment_ops=%llu logical_load=%llu logical_store=%llu logical_clear=%llu logical_skip=%llu green_n=%llu red_n=%llu miss_n=%llu weak_n=%llu green_bytes=%llu red_bytes=%llu miss_bytes=%llu weak_bytes=%llu physical_counters=0\n",
            (unsigned long long)frame,(unsigned long long)cov_traffic.passes,(unsigned long long)cov_traffic.load,
            (unsigned long long)cov_traffic.store,(unsigned long long)cov_traffic.clear,(unsigned long long)cov_traffic.skip,
            (unsigned long long)g_cov_green_n,(unsigned long long)g_cov_red_n,(unsigned long long)g_cov_miss_n,(unsigned long long)g_cov_weak_n,
            (unsigned long long)g_cov_green_bytes,(unsigned long long)g_cov_red_bytes,(unsigned long long)g_cov_miss_bytes,(unsigned long long)g_cov_weak_bytes);
    if(g_cov_traffic_on)fprintf(stderr,"[nvn-vulkan] RENDER_PASS_CACHE frame=%llu enabled=%d hits=%llu creates=%llu\n",
        (unsigned long long)frame,!G.disable_global_pass_cache,(unsigned long long)G.render_pass_hits,(unsigned long long)G.render_pass_creates);
    memset(&cov_traffic,0,sizeof cov_traffic);
    g_cov_green_bytes=g_cov_red_bytes=g_cov_miss_bytes=g_cov_weak_bytes=0;
    g_cov_green_n=g_cov_red_n=g_cov_miss_n=g_cov_weak_n=0;
    cov_flush();
    const char *path=getenv("DM_COV_TABLE_CONTROL");if(!path||g_cov_quarantined)return;
    FILE *f=fopen(path,"r");if(!f)return;int value=fgetc(f);int tail=fgetc(f);fclose(f);
    if((value=='0'||value=='1')&&(tail==EOF||tail=='\n'))g_cov_table=value=='1';
}

/* Keep the existing clear folding and proven store liveness. Table decisions
 * apply only to ordinary LOAD color attachments, with a separate slot/mip/layer
 * identity. No old seed file can authorize a discard in this version. */
static void cov_prepare_pass(VkAttachmentDescription *attachments,VkClearValue *clears,
    VTexture **targets,const VkImageSubresourceRange *ranges,const uint32_t *widths,const uint32_t *heights,
    uint32_t count,uint32_t width,uint32_t height) {
    if(!g_cov_table&&!g_cov_probe&&!g_cov_store_skip&&!g_cov_traffic_on)return;
    if(g_cov_frame!=G.present_count){g_cov_frame=G.present_count;g_cov_frame_bytes=0;}
    int safe=!S.feedback_flags&&!S.depth_input&&!G.diag_loadstore&&!G.diag_shape&&!G.diag_skip_draws&&!G.skip_ps[0];
    PassKey base=S.pass_key;
    for(uint32_t i=0;i<count;i++){
        uint64_t bytes=(uint64_t)width*height*(targets[i]->bpp?targets[i]->bpp:4);
        int depth=!!(aspect(targets[i]->format)&VK_IMAGE_ASPECT_DEPTH_BIT);
        if(safe&&((depth&&(g_cov_store_skip&1))||(!depth&&(g_cov_store_skip&2))))
            attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkClearValue sentinel;uint32_t word=0;
        int eligible=safe&&!depth&&widths[i]==width&&heights[i]==height&&
            attachments[i].loadOp==VK_ATTACHMENT_LOAD_OP_LOAD&&nvn_cov_sentinel(targets[i]->format,&sentinel,&word);
        for(uint32_t j=0;j<count;j++)if(j!=i&&targets[j]->image==targets[i]->image&&
            ranges[j].baseMipLevel==ranges[i].baseMipLevel&&ranges[j].baseArrayLayer==ranges[i].baseArrayLayer)eligible=0;
        uint64_t key=eligible?cov_key_of(&base,width,height,targets[i]->format,0,i,ranges[i].baseMipLevel,ranges[i].baseArrayLayer):0;
        CovRec *r=eligible?cov_lookup(key,width,height,targets[i]->format,0,0):NULL;
        if(eligible&&g_cov_table&&!g_cov_quarantined){
            if(!r){g_cov_miss_bytes+=bytes;g_cov_miss_n++;}
            else if(r->verdict==COV_RED||r->uncovered){g_cov_red_bytes+=bytes;g_cov_red_n++;}
            else if(r->samples<g_cov_min_samples){g_cov_weak_bytes+=bytes;g_cov_weak_n++;}
            else {attachments[i].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;g_cov_green_bytes+=bytes;g_cov_green_n++;}
        }
        /* Rotate the sampled pass and attachment. Hard frame and submission
         * limits include every buffer allocated, including positive controls. */
        if(eligible&&g_cov_probe&&S.ncoverage<16&&bytes<=g_cov_budget-g_cov_frame_bytes&&
            G.pass_count%count==i&&(G.pass_count+G.present_count*7)%16==0){
            unsigned q=S.ncoverage++;memset(&S.coverage[q],0,sizeof S.coverage[q]);
            S.coverage[q].texture=targets[i];S.coverage[q].buffer=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            S.coverage[q].pass=G.pass_count;S.coverage[q].serial=targets[i]->serial;S.coverage[q].key=key;
            S.coverage[q].w=width;S.coverage[q].h=height;S.coverage[q].format=targets[i]->format;S.coverage[q].slot=i;
            S.coverage[q].mip=ranges[i].baseMipLevel;S.coverage[q].layer=ranges[i].baseArrayLayer;
            S.coverage[q].sentinel=word;S.coverage[q].layout=attachments[i].finalLayout;
            g_cov_frame_bytes+=bytes;S.coverage_bytes+=bytes;
            attachments[i].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;clears[i]=sentinel;
        }
        cov_traffic.passes++;
        if(attachments[i].loadOp==VK_ATTACHMENT_LOAD_OP_LOAD)cov_traffic.load+=bytes;
        else if(attachments[i].loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR)cov_traffic.clear+=bytes;
        else cov_traffic.skip+=bytes;
        if(attachments[i].storeOp==VK_ATTACHMENT_STORE_OP_STORE)cov_traffic.store+=bytes;else cov_traffic.skip+=bytes;
    }
    memcpy(S.pass_key.attachments,attachments,count*sizeof *attachments);
}
