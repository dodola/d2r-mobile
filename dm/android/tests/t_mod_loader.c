#include "../d2r_mod_loader.c"
#include <assert.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void write_file(const char *path,const char *text){FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs(text,f)>=0);CHECK(!fclose(f));}
static void directory(const char *path){CHECK(!mkdir(path,0700));}
static char root[4096],base[4096],active[4096],package[4096],data[4096],descriptor[4096];
static void reset(char overlay[8192],char save[4096],int debug){snprintf(overlay,8192,"%s",base);snprintf(save,4096,"%s/%s",root,debug?"debug/save":"save");}
static void manifest(const char *base_hash,const char *config_hash,const char *archive_hash){
    char text[1024];snprintf(text,sizeof text,"d2r-mod-v1\nbuild db0507f36b84200ac05f75d76ed5026f\nbase %s\nfile %s 3 data/.build.config\nfile %s 3 data/00-00000008.data\nend\n",base_hash,config_hash,archive_hash);write_file(descriptor,text);
}
int main(int argc,char **argv){
    /* Optional real prepared package verification, without original game execution. */
    char temp[]="/tmp/d2r-mod-test-XXXXXX";CHECK(mkdtemp(temp));snprintf(root,sizeof root,"%s",temp);
    snprintf(base,sizeof base,"%s/base",root);directory(base);char base_data[4096];CHECK(mod_path(base_data,sizeof base_data,base,"data"));directory(base_data);
    char p[4096];CHECK(mod_path(p,sizeof p,base_data,".build.config"));write_file(p,"abc");
    char actual[41];uint64_t bytes=0;CHECK(mod_hash(p,actual,&bytes));CHECK(bytes==3&&!strcmp(actual,"a9993e364706816aba3e25717850c26c9cd0d89d"));
    /* File hashing must match one-shot SHA-1 across padding and read blocks. */
    unsigned char content[65539];for(unsigned i=0;i<sizeof content;i++)content[i]=(unsigned char)(i*31);
    const size_t sizes[]={0,55,56,63,64,65,32767,32768,32769,sizeof content};
    char vector[4096];CHECK(mod_path(vector,sizeof vector,root,"hash-vector"));
    for(unsigned i=0;i<sizeof sizes/sizeof *sizes;i++){
        FILE *f=fopen(vector,"wb");CHECK(f);CHECK(fwrite(content,1,sizes[i],f)==sizes[i]);CHECK(!fclose(f));
        unsigned char raw[20];char expected[41];dm_sha1(content,sizes[i],raw);
        for(unsigned j=0;j<20;j++)snprintf(expected+j*2,3,"%02x",raw[j]);
        char observed[41];CHECK(mod_hash(vector,observed,&bytes));CHECK(bytes==sizes[i]&&!strcmp(observed,expected));
    }
    char mods[4096];CHECK(mod_path(mods,sizeof mods,root,"mods"));directory(mods);CHECK(mod_path(active,sizeof active,mods,"active.txt"));
    CHECK(mod_path(package,sizeof package,mods,"sample"));directory(package);char overlay_dir[4096];CHECK(mod_path(overlay_dir,sizeof overlay_dir,package,"overlay"));directory(overlay_dir);
    CHECK(mod_path(data,sizeof data,overlay_dir,"data"));directory(data);CHECK(mod_path(p,sizeof p,data,".build.config"));write_file(p,"abc");CHECK(mod_path(p,sizeof p,data,"00-00000008.data"));write_file(p,"abc");
    CHECK(mod_path(descriptor,sizeof descriptor,package,"package.txt"));manifest(actual,actual,actual);
    char overlay[8192],save[4096];reset(overlay,save,0);CHECK(!dm_android_mod_configure(root,overlay,sizeof overlay,save,sizeof save));CHECK(!strcmp(overlay,base));
    write_file(active,"\n");CHECK(!dm_android_mod_configure(root,overlay,sizeof overlay,save,sizeof save));
    write_file(active,"sample\n");CHECK(!dm_android_mod_configure(root,overlay,sizeof overlay,save,sizeof save));CHECK(strstr(overlay,"/sample/overlay:")&&strstr(save,"/sample/save"));
    reset(overlay,save,1);CHECK(!dm_android_mod_configure(root,overlay,sizeof overlay,save,sizeof save));CHECK(strstr(save,"/sample/debug-save"));
    const char *invalid[]={"../sample\n","sample\nextra\n","sample","SAMPLE\n","sample/other\n"};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){write_file(active,invalid[i]);reset(overlay,save,0);CHECK(dm_android_mod_configure(root,overlay,sizeof overlay,save,sizeof save)==EINVAL);CHECK(!strcmp(overlay,base));}
    write_file(active,"sample\n");manifest("0000000000000000000000000000000000000000",actual,actual);reset(overlay,save,0);CHECK(dm_android_mod_configure(root,overlay,sizeof overlay,save,sizeof save)==EINVAL);manifest(actual,actual,actual);
    CHECK(mod_path(p,sizeof p,data,"00-00000008.data"));write_file(p,"bad");CHECK(dm_android_mod_configure(root,overlay,sizeof overlay,save,sizeof save)==EINVAL);write_file(p,"abc");
    CHECK(!unlink(p));CHECK(!symlink("/etc/passwd",p));CHECK(dm_android_mod_configure(root,overlay,sizeof overlay,save,sizeof save)==EINVAL);CHECK(!unlink(p));write_file(p,"abc");
    char small[5]="base";CHECK(dm_android_mod_configure(root,small,sizeof small,save,sizeof save)!=0);
    if(argc==3){
        /* argv[1] is a root containing mods/<id>; argv[2] is its base overlay. */
        snprintf(overlay,sizeof overlay,"%s",argv[2]);snprintf(save,sizeof save,"%s/save",argv[1]);
        CHECK(!dm_android_mod_configure(argv[1],overlay,sizeof overlay,save,sizeof save));
    }
    printf("{\"passed\":true,\"checks\":%u,\"original_game_executed\":false}\n",checks);return 0;
}
