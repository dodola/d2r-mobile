/* Calls the actual production .so with synthetic original-item callees.
 * No original-game process, save directory, UI or vibrator is used. */
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t item_code;
static uint32_t get_code(uint64_t unit){(void)unit;return item_code;}
static int is_type(uint64_t unit,int type){(void)unit;(void)type;return 0;}
static int get_stat(uint64_t unit,int stat,int layer){(void)unit;(void)stat;(void)layer;return 0;}
static void trampoline(uint8_t *base,size_t offset,void *function){
    uint32_t ins[]={0x58000050,0xd61f0200};memcpy(base+offset,ins,8);memcpy(base+offset+8,&function,8);
    __builtin___clear_cache((char *)base+offset,(char *)base+offset+16);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);assert(!mprotect(base+(offset&~(page-1)),page,PROT_READ|PROT_EXEC));
}
int main(int argc,char **argv){
    assert(argc==3);unsetenv("DM_DROP_HAPTIC");
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
    int (*poll)(int)=dlsym(lib,"dm_android_drop_haptic_poll");
    void (*land)(uint8_t *,uint64_t)=dlsym(lib,"dm_drop_haptic_land");
    void (*init)(const char *)=dlsym(lib,"dm_custom_init");
    assert(poll&&land&&init&&dlsym(lib,"Java_org_d2r_binaryprobe_GameHaptics_nativePoll"));
    char dir[]="/data/local/tmp/d2r-haptics-so-XXXXXX";assert(mkdtemp(dir));
    char path[256];snprintf(path,sizeof path,"%s/sw_custom.txt",dir);
    FILE *file=fopen(path,"w");assert(file);assert(fprintf(file,"drop_haptic=%c\n",argv[2][0])>0&&!fclose(file));init(dir);
    uint8_t *base=mmap(NULL,0x600000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(base!=MAP_FAILED);
    trampoline(base,0x570260,get_code);trampoline(base,0x574bc0,is_type);trampoline(base,0x5b9290,get_stat);
    uint64_t unit[8]={4},data[4]={0};unit[1]=99;unit[2]=(uintptr_t)data;
    unsigned checks=0;int enabled=argv[2][0]!='0';
    const char *codes[]={"r30 ","r24 ","r21 "};
    for(int i=0;i<3;i++){
        memcpy(&item_code,codes[i],4);land(base,(uintptr_t)unit);
        checks++;assert(poll(1)==(enabled?i+1:-1));checks++;assert(poll(1)==(enabled?0:-1));
    }
    for(int i=2;i>=0;i--){memcpy(&item_code,codes[i],4);land(base,(uintptr_t)unit);}
    checks++;assert(poll(1)==(enabled?1:-1));checks++;assert(poll(1)==(enabled?0:-1));
    land(base,(uintptr_t)unit);checks++;assert(poll(0)==-1);checks++;assert(poll(1)==(enabled?0:-1));
    printf("ANDROID_D2R_HAPTICS_HOST_OK checks=%u enabled=%d real_so=1 jni_export=1 shared_rules=1 strongest=1 exactly_once=1 inactive_drain=1 synthetic_items=1\n",checks,enabled);
    unlink(path);rmdir(dir);munmap(base,0x600000);dlclose(lib);return 0;
}
