/* Real shared drop rules, queue, settings Apply/Discard and Android consumer.
 * Synthetic item/callee memory and a temporary settings directory only. */
#include "../../host/dm_custom.c"
#include "../../host/dm_drophaptic.c"
#include "../d2r_haptics_android.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(value) do { checks++; assert(value); } while (0)
static uint32_t item_code;
const char *dm_save_commitdir(void){return NULL;}
int dm_lang_zh(void){return 1;}
static uint32_t get_code(uint64_t unit){(void)unit;return item_code;}
static int is_type(uint64_t unit,int type){(void)unit;(void)type;return 0;}
static int get_stat(uint64_t unit,int stat,int layer){(void)unit;(void)stat;(void)layer;return 0;}
static void trampoline(uint8_t *base,size_t offset,void *function){
    uint32_t ins[]={0x58000050,0xd61f0200};memcpy(base+offset,ins,8);memcpy(base+offset+8,&function,8);
    __builtin___clear_cache((char *)base+offset,(char *)base+offset+16);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);assert(!mprotect(base+(offset&~(page-1)),page,PROT_READ|PROT_EXEC));
}
static void apply(int value){atomic_store(&g_pend[key_index("drop_haptic")],value);dm_custom_on_uimsg("OptionsPanelMessage","AcceptChanges",NULL);}
static void code(const char *text){memcpy(&item_code,text,4);}
int main(int argc,char **argv){
    char dir[]="/data/local/tmp/d2r-haptics-XXXXXX";assert(mkdtemp(dir));dm_custom_init(dir);
    if(argc>1)setenv("DM_DROP_HAPTIC",argv[1],1);else unsetenv("DM_DROP_HAPTIC");
    uint8_t *base=mmap(NULL,0x600000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(base!=MAP_FAILED);
    trampoline(base,0x570260,get_code);trampoline(base,0x574bc0,is_type);trampoline(base,0x5b9290,get_stat);
    uint64_t unit[8]={4},data[4]={0};unit[1]=99;unit[2]=(uintptr_t)data;code("r30 ");
    CHECK(dm_android_drop_haptic_poll(0)==-1);CHECK(dm_custom_get("drop_haptic")==1);
    if(argc>1){
        apply(argv[1][0]=='0');dm_drop_haptic_land(base,(uintptr_t)unit);
        CHECK(dm_drop_haptic_enabled()==(argv[1][0]!='0'));
        CHECK(dm_android_drop_haptic_poll(1)==(argv[1][0]=='0'?-1:1));
        CHECK(dm_android_drop_haptic_poll(1)==(argv[1][0]=='0'?-1:0));
        printf("ANDROID_D2R_HAPTIC_OVERRIDE_OK checks=%u forced=%s\n",checks,argv[1]);
    }else{
        const char *codes[]={"r30 ","r24 ","r21 "};
        for(int i=0;i<3;i++){code(codes[i]);dm_drop_haptic_land(base,(uintptr_t)unit);
            CHECK(dm_android_drop_haptic_poll(1)==i+1);CHECK(dm_android_drop_haptic_poll(1)==0);}
        for(int i=2;i>=0;i--){code(codes[i]);dm_drop_haptic_land(base,(uintptr_t)unit);}
        CHECK(dm_android_drop_haptic_poll(1)==1);CHECK(dm_android_drop_haptic_poll(1)==0);
        code("r30 ");dm_drop_haptic_land(base,(uintptr_t)unit);CHECK(dm_android_drop_haptic_poll(0)==-1);
        CHECK(dm_android_drop_haptic_poll(1)==0);
        atomic_store(&g_pend[key_index("drop_haptic")],0);CHECK(dm_drop_haptic_enabled()==1);
        dm_custom_on_uimsg("OptionsPanelMessage","CancelChanges",NULL);CHECK(dm_drop_haptic_enabled()==1);
        dm_drop_haptic_land(base,(uintptr_t)unit);apply(0);CHECK(dm_android_drop_haptic_poll(1)==-1);
        dm_drop_haptic_land(base,(uintptr_t)unit);apply(1);CHECK(dm_android_drop_haptic_poll(1)==0);
        code("rin ");dm_drop_haptic_land(base,(uintptr_t)unit);CHECK(dm_android_drop_haptic_poll(1)==0);
        data[0]=7;dm_drop_haptic_land(base,(uintptr_t)unit);CHECK(dm_android_drop_haptic_poll(1)==2);data[0]=0;
        unit[0]=1;code("r30 ");dm_drop_haptic_land(base,(uintptr_t)unit);CHECK(dm_android_drop_haptic_poll(1)==0);unit[0]=4;
        dm_drop_haptic_land(NULL,(uintptr_t)unit);dm_drop_haptic_land(base,0);CHECK(dm_android_drop_haptic_poll(1)==0);
        apply(0);dm_custom_on_uimsg("OptionsPanelMessage","ResetToDefaults",NULL);CHECK(dm_drop_haptic_enabled()==0);
        dm_custom_on_uimsg("OptionsPanelMessage","AcceptChanges",NULL);CHECK(dm_drop_haptic_enabled()==1);
        dm_drop_haptic_land(base,(uintptr_t)unit);CHECK(dm_android_drop_haptic_poll(1)==1);
        CHECK(dm_android_drop_haptic_poll(1)==0);
        printf("ANDROID_D2R_HAPTICS_OK checks=%u shared_rules=1 strongest=1 exactly_once=1 inactive_drain=1 apply_discard=1 defaults=1 synthetic_items=1\n",checks);
    }
    char path[256];snprintf(path,sizeof path,"%s/sw_custom.txt",dir);unlink(path);rmdir(dir);munmap(base,0x600000);
    return 0;
}
