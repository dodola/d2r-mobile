/* Actual app library, isolated temporary files and production HLE bindings.
 * No original game, real save, Android Activity or guest UI is executed. */
#include "../../host/hle.h"
#include "../services_platform.h"
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"Custom consumer fixture failed line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static void put(const char *p,const char *s){FILE *f=fopen(p,"w");CHECK(f);CHECK(fputs(s,f)>=0);CHECK(!fclose(f));}
static void path(char out[1400],const char *root,const char *rest){CHECK(snprintf(out,1400,"%s/%s",root,rest)<1400);}
static void dirs(const char *root,const char *rest){char p[1400];path(p,root,rest);CHECK(!mkdir(p,0700));}
static int has(const char *p,const char *text){char buf[8192];FILE *f=fopen(p,"r");if(!f)return 0;size_t n=fread(buf,1,sizeof buf-1,f);fclose(f);buf[n]=0;return strstr(buf,text)!=NULL;}
static void *binding(void *lib,const char *name){const hle_force_ent *a=dlsym(lib,"__start_dm_force"),*b=dlsym(lib,"__stop_dm_force");CHECK(a&&b);for(;a<b;a++)if(!strcmp(a->name,name))return a->fn;CHECK(0);return NULL;}
static void overlay(const char *root,const char *relative,int variant){
    dirs(root,relative);char p[1400],base[1400];path(base,root,relative);dirs(base,"data");
    const char *files[]={"ctrl_touch","touch_strings.tsv","data/.build.config","data/00-00000007.data"};
    for(unsigned i=0;i<4;i++){path(p,base,files[i]);put(p,"fixture-only\n");}
    if(variant){path(p,base,"features.txt");put(p,"item_notes\nfixture payload list\n");}
}
int main(int argc,char **argv){
    CHECK(argc==3);int mode=atoi(argv[2]);CHECK(mode>=-1&&mode<=1);
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){fprintf(stderr,"dlopen %s\n",dlerror());return 1;}
#define FN(type,name) __typeof__((type)0) name=(type)dlsym(lib,#name);CHECK(name)
    FN(void (*)(const char *),dm_custom_init);
    FN(int (*)(const char *),dm_custom_get);
    FN(const char *(*)(void),dm_android_custom_startup_locale);
    FN(int (*)(const char *,char *,size_t),dm_android_custom_overlay);
    FN(int (*)(module_t *),dm_android_custom_attach);
    FN(int (*)(void),dm_android_title_custom_settings);
    FN(void (*)(uint64_t,const char *,int,uint64_t),gate_register);
    FN(int (*)(const dm_android_platform_config *),dm_android_platform_bind);
    FN(uint64_t (*)(void),dm_android_platform_language_code);
    FN(void (*)(const char *),dm_lootfilter_seed);
    Result (*mount_save)(const char *,const void *)=binding(lib,"_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
    void (*unmount)(const char *)=binding(lib,"_ZN2nn2fs7UnmountEPKc");
    uint64_t (*desired_language)(void)=binding(lib,"_ZN2nn2oe18GetDesiredLanguageEv");
    char root[]="/data/local/tmp/d2r-custom-consumers-XXXXXX",p[1400],save[1400],profile[1400],out[1400];CHECK(mkdtemp(root));
    if(mode>=0){path(p,root,"sw_custom.txt");char text[160];snprintf(text,sizeof text,"lang=%d\nauto_buff=%d\nitem_notes=%d\ntouch_swap_above=%d\n",mode,mode,mode,mode);put(p,text);}
    CHECK(!setenv("DM_LANG","fr-FR",1));dm_custom_init(root);
    CHECK(dm_custom_get("auto_buff")== (mode<0?1:mode));CHECK(dm_custom_get("item_notes")== (mode<0?1:mode));
    const char *locale=dm_android_custom_startup_locale();CHECK(mode<0?!locale:locale&&!strcmp(locale,mode?"en":"zh-Hans"));
    if(locale)CHECK(!setenv("DM_LANG",locale,1));
    dirs(root,"profile");path(profile,root,"profile");dm_android_platform_config config={.mode=DM_ANDROID_HOST_SHELL,.profile_directory=profile,.locale=locale?locale:"fr-FR",.locale_source=locale?"Custom.lang":"fixture-system"};
    CHECK(!dm_android_platform_bind(&config));char code[8]={0};uint64_t value=desired_language();memcpy(code,&value,8);CHECK(!strcmp(code,mode<0?"fr":mode?"en":"zh-Hans"));CHECK(value==dm_android_platform_language_code());
    overlay(root,"romfs-overlay",0);dirs(root,"romfs-overlay-variants");overlay(root,"romfs-overlay-variants/item_notes",1);
    CHECK(dm_android_custom_overlay(root,out,sizeof out));CHECK(strstr(out,"romfs-overlay-variants/item_notes")!=NULL ? mode!=0 : mode==0);
    path(p,root,"romfs-overlay-variants/item_notes/data/00-00000007.data");CHECK(!unlink(p));CHECK(dm_android_custom_overlay(root,out,sizeof out));path(p,root,"romfs-overlay");CHECK(!strcmp(out,p));
    path(p,root,"romfs-overlay/data/.build.config");CHECK(!unlink(p));CHECK(!dm_android_custom_overlay(root,out,sizeof out)&&!out[0]);
    dirs(root,"save-unverified");path(save,root,"save-unverified");dirs(save,"Settings");path(p,save,"Settings/Settings.json");put(p,"{ \"Language\": 0, \"VoiceLanguage\": 0, \"user\": 77 }");
    CHECK(!setenv("DM_SAVE",save,1));CHECK(!dm_android_title_custom_settings());CHECK(!mount_save("unverified",NULL));unmount("unverified");CHECK(has(p,"\"Language\": 0"));path(p,save,"Settings/DmLite.fltr");CHECK(access(p,F_OK));
    module_t *mods=dlsym(lib,"g_mod");int *count=dlsym(lib,"g_nmod");CHECK(mods&&count);*count=1;strcpy(mods[0].name,"main");strcpy(mods[0].build_id,"db0507f36b84200ac05f75d76ed5026f");mods[0].size=0x3220000;mods[0].base=calloc(1,mods[0].size);CHECK(mods[0].base);
    gate_register(0x1000,"obs",OBS_UIMSG,0);gate_register(0x2000,"obs",OBS_TICK,0);gate_register(0x3000,"obs",OBS_C2S,0);gate_register(0x8100,"svc",SVC_HOST_C2S_DELIVER,0);CHECK(dm_android_custom_attach(mods)&&dm_android_title_custom_settings());
    dirs(root,"external-settings");path(out,root,"external-settings");path(p,out,"Settings.json");put(p,"{ \"Language\": 8, \"VoiceLanguage\": 8, \"sentinel\": 91 }");
    dirs(root,"save-linked");path(save,root,"save-linked");path(p,save,"Settings");CHECK(!symlink(out,p));
    dm_lootfilter_seed(save);CHECK(!setenv("DM_SAVE",save,1));CHECK(!mount_save("linked",NULL));unmount("linked");
    path(p,out,"Settings.json");CHECK(has(p,"\"Language\": 8")&&has(p,"\"sentinel\": 91"));path(p,out,"DmLite.fltr");CHECK(access(p,F_OK));
    dirs(root,"save-verified");path(save,root,"save-verified");dirs(save,"Settings");
    path(p,save,"Settings/Settings.json");put(p,mode==0?"{ \"Language\": 0, \"VoiceLanguage\": 0, \"user\": 77, \"AutoMap Open On Load\": 0 }":"{ \"Language\": 8, \"VoiceLanguage\": 8, \"user\": 77 }");
    path(p,save,"Settings/DmLite.fltr");put(p,"user custom rule\n");path(p,save,"Settings/UserNamed.fltr");put(p,"user named rule\n");path(p,save,"Settings/DmHeavy.fltr");CHECK(!symlink("UserNamed.fltr",p));
    /* Seed preserves a link rather than following/replacing it. The Android
     * transaction layer deliberately refuses links, so remove this isolated
     * adversarial fixture before exercising a regular save mount. */
    dm_lootfilter_seed(save);struct stat st;CHECK(!lstat(p,&st)&&S_ISLNK(st.st_mode));CHECK(!unlink(p));put(p,"user heavy rule\n");
    path(p,save,"Settings/DmLiteBow.fltr");CHECK(!unlink(p));path(p,save,"Settings/DmHeavyBow.fltr");CHECK(!unlink(p));
    CHECK(!setenv("DM_SAVE",save,1));CHECK(!mount_save("verified",NULL));unmount("verified");
    path(p,save,"Settings/Settings.json");CHECK(has(p,mode==1?"\"Language\": 0":"\"Language\": 8"));CHECK(has(p,mode==1?"\"VoiceLanguage\": 0":"\"VoiceLanguage\": 8"));CHECK(has(p,"\"user\": 77"));
    CHECK(has(p,"\"AutoMap Open On Load\": 1"));
    path(p,save,"Settings/DmLite.fltr");CHECK(has(p,"user custom rule"));path(p,save,"Settings/DmLiteBow.fltr");CHECK(has(p,"\"name\": \"DmLiteBow\"")&&!has(p,"HIDE ammo"));path(p,save,"Settings/DmHeavyBow.fltr");CHECK(has(p,"SHOW socketed or ethereal")&&!has(p,"HIDE ammo"));
    path(p,save,"Settings/DmHeavy.fltr");CHECK(has(p,"user heavy rule"));path(p,save,"Settings/UserNamed.fltr");CHECK(has(p,"user named rule"));
    path(p,save,"Settings/DmLiteBow.fltr");put(p,"");dm_lootfilter_seed(save);CHECK(!stat(p,&st)&&!st.st_size);CHECK(!mount_save("repeat",NULL));unmount("repeat");CHECK(!stat(p,&st)&&!st.st_size);
    /* Missing Settings.json gets valid minimal JSON on explicit English,
     * while an unapplied/default language never creates one. */
    dirs(root,"save-fresh");path(save,root,"save-fresh");CHECK(!setenv("DM_SAVE",save,1));CHECK(!mount_save("fresh",NULL));unmount("fresh");path(p,save,"Settings/Settings.json");
    if(mode>=0){CHECK(has(p,mode?"\"Language\": 0":"\"Language\": 8")&&has(p,mode?"\"VoiceLanguage\": 0":"\"VoiceLanguage\": 8"));}else CHECK(!has(p,"\"Language\"")&&!has(p,"\"VoiceLanguage\""));
    CHECK(has(p,"\"AutoMap Open On Load\": 1")&&!has(p,",}"));
    printf("ANDROID_D2R_CUSTOM_CONSUMERS_OK checks=%d mode=%d desired_language=1 original_mount_binding=1 preserve_existing_rules=1 overlay_selection=1 fallback=1 synthetic_memory=1 isolated_files=1 original_game_not_claimed=1\n",checks,mode);return 0;
}
