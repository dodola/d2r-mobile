/* Execute the reviewed original BitKnit2 decoder in an isolated shell process.
 * The compressed fixture is extracted from the user's game, outside Git. */
#include "../../host/hle.h"
#include <errno.h>
#include <sys/stat.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) die("Android Granny fixture failed line=%d",__LINE__); } while (0)
enum { CTX_BYTES=0x3f18, CHUNK=0x2000 };
extern int dm_android_granny_call(int,uint64_t *);
extern void guest_thread_init(void);
extern void crash_thread_init(void);
extern void dm_android_loader_probe_allow_unused_force(void);
int dm_android_title_host_call(int n,uint64_t *r) { return dm_android_granny_call(n,r); }

static void setup(uint8_t *context,uint8_t *output,uint32_t length,void (*init)(uint64_t,uint64_t)) {
    memset(context,0xa5,CTX_BYTES);
    uint64_t start=(uintptr_t)output,one=0x100000001ull,end=start+length;
    for (int i=0;i<3;++i) memcpy(context+8*i,&start,8);
    memcpy(context+0x18,&end,8);
    for (int i=0;i<4;++i) memcpy(context+0x20+8*i,&one,8);
    uint64_t v48=0x0001000000000001ull,z=0;
    uint32_t value=0x10000; memcpy(context+0x48,&v48,8); memcpy(context+0x50,&value,4);
    value=1; memcpy(context+0x40,&value,4);
    init((uintptr_t)context+0x88,0);
    value=0xfac688; memcpy(context+0x44,&value,4);
    memcpy(context+0x58,&z,8); memcpy(context+0x80,&z,8);
}
int main(int argc,char **argv) {
    if (argc!=4) return 2;
    g_t0_ns=now_ns(); crash_thread_init();
    CHECK(!setenv("DM_GRANNY_PRE","0",1));
    CHECK(!dm_android_granny_call(0x83,NULL));
    uint64_t regs[31]={0}; CHECK(!dm_android_granny_call(0x83,regs));
    const char *names[]={"main","sdk","subsdk0"};
    dm_android_loader_probe_allow_unused_force(); load_modules(argv[1],names,3); guest_thread_init();
    module_t *main=&g_mod[0]; CHECK(!strcmp(main->build_id,"db0507f36b84200ac05f75d76ed5026f"));
    CHECK(!dm_android_granny_call(0x82,regs));
    char saved=main->build_id[0]; main->build_id[0]='0';
    CHECK(!dm_android_granny_call(0x83,regs)); main->build_id[0]=saved;
    FILE *input=fopen(argv[2],"rb"); CHECK(input);
    uint32_t header[4]; CHECK(fread(header,1,sizeof header,input)==sizeof header && header[0]==0x47525042);
    uint32_t clen=header[1],dlen=header[2];
    CHECK(clen && dlen && clen<=64*1024*1024 && dlen<=64*1024*1024);
    uint8_t *compressed=malloc(clen); CHECK(compressed);
    CHECK(fread(compressed,1,clen,input)==clen && fgetc(input)==EOF); CHECK(!fclose(input));
    void (*init)(uint64_t,uint64_t)=(void *)((uintptr_t)main->base+0x11f4c70);
    uint64_t (*decode)(uint64_t,uint64_t,uint64_t)=(void *)((uintptr_t)main->base+0x11f4af0);
    uint8_t *reference=aligned_alloc(64,(CTX_BYTES+63)&~63u), *context=aligned_alloc(64,(CTX_BYTES+63)&~63u);
    uint8_t *expected=malloc(dlen), *actual=malloc(dlen); CHECK(reference && context && expected && actual);
    unsigned chunks=0; uint32_t produced=0, final_state=0;
    for (int path=0;path<2;++path) {
        memset(expected,0,dlen); memset(actual,0,dlen);
        setup(reference,expected,dlen,init); setup(context,actual,dlen,init);
        for (uint32_t pos=0;pos<clen;pos+=CHUNK) {
            uint32_t length=clen-pos<CHUNK?clen-pos:CHUNK;
            uint64_t result=decode((uintptr_t)reference,(uintptr_t)compressed+pos,length);
            memset(regs,0,sizeof regs); regs[0]=(uintptr_t)context; regs[1]=(uintptr_t)compressed+pos; regs[2]=length;
            regs[path?25:24]=clen; regs[path?26:28]=pos;
            CHECK(dm_android_granny_call(path?0x84:0x83,regs)); CHECK(regs[0]==(uint32_t)result && (result&1));
            CHECK(!memcmp(reference+0x20,context+0x20,CTX_BYTES-0x20));
            for (int i=0;i<4;++i) {
                uint64_t a,b; memcpy(&a,reference+8*i,8); memcpy(&b,context+8*i,8);
                CHECK(a-(uintptr_t)expected==b-(uintptr_t)actual);
            }
            CHECK(!memcmp(expected,actual,dlen)); ++chunks;
        }
        uint64_t end;
        memcpy(&end,context+8,8); memcpy(&final_state,context+0x48,4);
        /* The original segment reader finalizes the decoder after this
         * streaming loop. Check the bytes already produced by the loop;
         * its pending tail/state must still equal the original context. */
        CHECK(end>(uintptr_t)actual && end<=(uintptr_t)actual+dlen);
        produced=(uint32_t)(end-(uintptr_t)actual);
    }
    FILE *out=fopen(argv[3],"w"); CHECK(out);
    fprintf(out,"{\"passed\":true,\"checks\":%u,\"paths\":2,\"chunks\":%u,\"compressed_bytes\":%u,\"decoded_capacity\":%u,\"streaming_decoded_bytes\":%u,\"pending_state\":%u,\"original_decoder_executed\":true,\"context_and_output_equal\":true,\"candidate_predecode_enabled\":false,\"app_save_data_used\":false,\"full_game_verified\":false}\n",checks,chunks,clen,dlen,produced,final_state);
    CHECK(!fclose(out)); return 0;
}
