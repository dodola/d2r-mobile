/* Real app dispatcher + independently encoded zlib streams and synthetic
 * guest callees. No title resource, save file or game startup is required. */
#include "../../host/dm.h"
#include <assert.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <zlib.h>

static int (*host_call)(int,uint64_t *);
static uint64_t seen[4];
static unsigned deliveries, systems, overrides;
static uint32_t override_result;
static void deliver(uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    seen[0]=a;seen[1]=b;seen[2]=c;seen[3]=d;++deliveries;
}
static void system_message(uint64_t a,uint64_t b) {seen[0]=a;seen[1]=b;++systems;}
static uint32_t override(void *self,void *out,uint32_t cap,const void *in,uint32_t n,uint32_t mode) {
    assert(self && out && in && cap && n && mode==1);++overrides;return override_result;
}
static void trampoline(uint8_t *base,size_t offset,void *function) {
    uint32_t ins[]={0x58000050,0xd61f0200}; /* ldr x16,.+8; br x16 */
    memcpy(base+offset,ins,sizeof ins);memcpy(base+offset+8,&function,sizeof function);
    __builtin___clear_cache((char *)base+offset,(char *)base+offset+16);
}
static uint64_t decode(const void *input,size_t n,void *output,uint32_t cap,int flags,int stream,void *object) {
    uint64_t regs[31],saved[31];
    for(int i=0;i<31;i++)regs[i]=0x8765000000000000ull+i;
    regs[0]=(uintptr_t)output;regs[1]=cap;regs[2]=(uintptr_t)input;regs[3]=n;
    regs[4]=flags;regs[5]=stream;regs[6]=(uintptr_t)object;memcpy(saved,regs,sizeof regs);
    assert(host_call(0x80,regs));assert(!memcmp(saved+1,regs+1,sizeof regs-sizeof *regs));return regs[0];
}
static void *worker(void *opaque) {
    uintptr_t seed=(uintptr_t)opaque;uint8_t input[8192],encoded[8300],decoded[8300];
    for(unsigned pass=0;pass<32;pass++) {
        size_t n=pass?17+pass*223:0;
        for(size_t i=0;i<n;i++)input[i]=(uint8_t)((i*37)^seed^pass);
        uLongf length=sizeof encoded;
        assert(compress2(encoded,&length,input,n,(pass%9)+1)==Z_OK);
        memset(decoded,0xcc,sizeof decoded);
        assert(decode(encoded,length,decoded,sizeof decoded,0,pass&1,NULL)==0);
        assert(!memcmp(input,decoded,n) && decoded[n]==0xcc);
    }
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!library){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
    module_t *modules=dlsym(library,"g_mod");int *count=dlsym(library,"g_nmod");
    int (*attach)(void)=dlsym(library,"dm_android_d2r_host_attach");
    void (*register_gate)(uint64_t,const char *,int,uint64_t)=dlsym(library,"gate_register");
    host_call=dlsym(library,"dm_android_title_host_call");
    assert(modules && count && attach && register_gate && host_call);
    uint64_t regs[31]={0},saved[31];assert(!host_call(0x80,regs));
    *count=1;strcpy(modules[0].name,"main");modules[0].size=0x2f10000;
    modules[0].base=mmap(NULL,modules[0].size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(modules[0].base!=MAP_FAILED);
    trampoline(modules[0].base,0x632510,deliver);trampoline(modules[0].base,0x6310b0,system_message);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    assert(!mprotect(modules[0].base+(0x6310b0&~(page-1)),page*2,PROT_READ|PROT_EXEC));
    strcpy(modules[0].build_id,"db0507f36b84200ac05f75d76ed5026f");assert(attach());
    assert(!host_call(0x80,regs)); /* Identity alone does not authorize a gate. */
    for(int n=0x80;n<=0x82;n++)register_gate(0x1000+n*4,"svc",n,0);
    for(int i=0;i<31;i++)regs[i]=0xaabbcc0000000000ull+i;
    memcpy(saved,regs,sizeof regs);assert(host_call(0x81,regs));
    assert(deliveries==1 && !memcmp(seen,saved,sizeof seen) && !memcmp(regs,saved,sizeof regs));
    assert(host_call(0x82,regs));assert(systems==1 && seen[0]==regs[0] && seen[1]==regs[1]);
    assert(!memcmp(regs,saved,sizeof regs) && !host_call(0x86,regs));

    uint8_t input[256],encoded[400],decoded[300],flagged[401];
    for(unsigned i=0;i<sizeof input;i++)input[i]=(uint8_t)i;
    uLongf n=sizeof encoded;assert(compress2(encoded,&n,input,sizeof input,6)==Z_OK);
    flagged[0]='Z';memcpy(flagged+1,encoded,n);
    assert(decode(encoded,n,decoded,256,0,0,NULL)==0 && !memcmp(input,decoded,256));
    assert(decode(flagged,n+1,decoded,256,1,1,NULL)==0 && !memcmp(input,decoded,256));
    flagged[0]='N';assert(decode(flagged,n+1,decoded,256,1,0,NULL)==8);
    flagged[0]='Z';assert(decode(flagged,1,decoded,256,1,0,NULL)==8);
    assert(decode(encoded,n,decoded,255,0,0,NULL)==4);
    encoded[n-1]^=1;assert(decode(encoded,n,decoded,256,0,0,NULL)==1);encoded[n-1]^=1;
    void *vtable[3]={NULL,NULL,(void *)override};void **object=vtable;
    override_result=0x100;memset(decoded,0xcc,sizeof decoded);
    assert(decode(encoded,n,decoded,256,0,0,&object)==0 && decoded[0]==0xcc && overrides==1);
    override_result=1;assert(decode(encoded,n,decoded,256,0,1,&object)==0 && !memcmp(input,decoded,256) && overrides==2);
    pthread_t workers[4];
    for(uintptr_t i=0;i<4;i++)assert(!pthread_create(&workers[i],NULL,worker,(void *)(i+1)));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(workers[i],NULL));
    puts("ANDROID_D2R_NATIVE_HOST_OK zlib_streams=128 edge_cases=8 c2s=2 gate_guards=3 registers_preserved=1");
    return 0;
}
