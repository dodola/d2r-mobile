/* Android 自定义页的持久化与状态；沿用画质、附件优化和 MOD 的既有文件格式。
 * Android Custom persistence/status, using the existing quality, attachment and MOD formats. */
#include "render_scale.h"
#include "d2r_map_reveal.h"
#include "../host/dm_i18n.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

static char settings_root[1024],mod_ids[128][49];
static int mod_count=1,settings_error;
static _Atomic unsigned action_clicks;
static int valid_mod_id(const char *id) {
    size_t n=strlen(id);if(!n||n>48)return 0;
    for(size_t i=0;i<n;i++)if(!((id[i]>='a'&&id[i]<='z')||(id[i]>='0'&&id[i]<='9')||id[i]=='_'||id[i]=='-'))return 0;
    return 1;
}
void dm_android_custom_settings_clear_error(void){settings_error=0;}
static int mod_compare(const void *a,const void *b){return strcmp(a,b);}
void dm_android_custom_settings_init(const char *dir) {
    if(!dir)return;strlcpy(settings_root,dir,sizeof settings_root);mod_count=1;memset(mod_ids,0,sizeof mod_ids);
    char path[1200];struct stat root_stat;snprintf(path,sizeof path,"%s/mods",dir);if(lstat(path,&root_stat)||!S_ISDIR(root_stat.st_mode))return;DIR *d=opendir(path);if(!d)return;
    struct dirent *entry;while((entry=readdir(d))&&mod_count<128) {
        if(!valid_mod_id(entry->d_name))continue;
        struct stat st;snprintf(path,sizeof path,"%s/mods/%s",dir,entry->d_name);
        if(lstat(path,&st)||!S_ISDIR(st.st_mode))continue;
        snprintf(path,sizeof path,"%s/mods/%s/package.txt",dir,entry->d_name);
        if(lstat(path,&st)||!S_ISREG(st.st_mode))continue;
        strlcpy(mod_ids[mod_count++],entry->d_name,sizeof mod_ids[0]);
    }
    closedir(d);qsort(mod_ids+1,mod_count-1,sizeof mod_ids[0],mod_compare);
}
int dm_android_custom_mod_count(void){return mod_count;}
static int settings_read(const char *leaf,char *text,size_t capacity) {
    char path[1200];snprintf(path,sizeof path,"%s/%s",settings_root,leaf);
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(uint64_t)st.st_size>=capacity){close(fd);return -1;}
    ssize_t n=read(fd,text,capacity-1);char extra;int ok=n>=0&&read(fd,&extra,1)==0;close(fd);
    if(!ok)return -1;text[n]=0;return 1;
}
void dm_android_custom_settings_load(int *mode,int *clear,int *store,int *mod) {
    char text[128];unsigned long long seq;int flags;float scale;*mode=0;*mod=0;
    if(settings_read("diagnostics/render-scale.txt",text,sizeof text)>0&&sscanf(text,"%f",&scale)==1&&scale>=0.70f&&scale<=1.0f)
        *mode=scale>=0.99f?1:scale>=0.89f?2:3;
    if(settings_read("diagnostics/attachment-mode.txt",text,sizeof text)>0&&sscanf(text,"%llu %d",&seq,&flags)==2&&flags>=0&&flags<=3){*clear=(flags&1)!=0;*store=(flags&2)!=0;}
    if(settings_read("mods/active.txt",text,sizeof text)>0){text[strcspn(text,"\r\n")]=0;for(int i=0;i<mod_count;i++)if(!strcmp(text,mod_ids[i]))*mod=i;}
}
static int settings_write(const char *leaf,const char *text) {
    char path[1200],temp[1216];snprintf(path,sizeof path,"%s/%s",settings_root,leaf);snprintf(temp,sizeof temp,"%s.custom-tmp",path);
    int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return 0;
    size_t n=strlen(text);int ok=write(fd,text,n)==(ssize_t)n&&fsync(fd)==0;if(close(fd))ok=0;
    if(ok)ok=rename(temp,path)==0;if(!ok)unlink(temp);return ok;
}
int dm_android_custom_settings_apply(int mode,int clear,int store,int mod) {
    if(mode<0||mode>3||mod<0||mod>=mod_count||!settings_root[0])return 0;
    const char *leaves[]={"diagnostics/render-scale.txt","diagnostics/attachment-mode.txt","mods/active.txt"};
    const char *scales[]={"auto\n","1.0\n","0.9\n","0.75\n"};
    char previous[3][128],next[3][128];int existed[3];
    for(int i=0;i<3;i++){existed[i]=settings_read(leaves[i],previous[i],sizeof previous[i]);if(existed[i]<0){settings_error=1;return 0;}}
    unsigned long long sequence=0;int old_flags=0;if(existed[1])sscanf(previous[1],"%llu %d",&sequence,&old_flags);
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);unsigned long long clock=(unsigned long long)now.tv_sec*1000000000ull+now.tv_nsec;
    if(sequence>=9223372036854775807ull){settings_error=1;return 0;}
    snprintf(next[0],sizeof next[0],"%s",scales[mode]);
    snprintf(next[1],sizeof next[1],"%llu %d\n",clock>sequence?clock:sequence+1,(clear?1:0)|(store?2:0));
    snprintf(next[2],sizeof next[2],"%s\n",mod_ids[mod]);
    char path[1200];struct stat st;snprintf(path,sizeof path,"%s/diagnostics",settings_root);if((mkdir(path,0700)&&errno!=EEXIST)||lstat(path,&st)||!S_ISDIR(st.st_mode)){settings_error=1;return 0;}
    snprintf(path,sizeof path,"%s/mods",settings_root);if((mkdir(path,0700)&&errno!=EEXIST)||lstat(path,&st)||!S_ISDIR(st.st_mode)){settings_error=1;return 0;}
    for(int i=0;i<3;i++)if(!settings_write(leaves[i],next[i])) {
        for(int j=0;j<i;j++){if(existed[j])settings_write(leaves[j],previous[j]);else{snprintf(path,sizeof path,"%s/%s",settings_root,leaves[j]);unlink(path);}}
        settings_error=1;return 0;
    }
    int rc=mode==0?dm_android_render_scale_auto():dm_android_render_scale_request(mode==1?1.0f:mode==2?0.9f:0.75f);
    settings_error=rc!=0;return rc==0;
}
void dm_android_custom_status(int mod,int selected,char *out,size_t capacity) {
    if(settings_error){snprintf(out,capacity,"%s",dm_ui("dmCustomSaveError"));return;}
    if(mod){snprintf(out,capacity,dm_ui("dmCustomModStatus"),selected>=0&&selected<mod_count&&selected?mod_ids[selected]:dm_ui("dmCustomModNone"));return;}
    dm_android_map_status s;dm_android_map_status_get(&s);
    const char *key=s.state==DM_MAP_WAITING?"dmCustomMapWaiting":s.state==DM_MAP_QUEUED||s.state==DM_MAP_RUNNING?"dmCustomMapBusy":s.state==DM_MAP_DONE?"dmCustomMapDone":s.state==DM_MAP_FAILED||s.state==DM_MAP_CANCELLED?"dmCustomMapFailed":s.game?"dmCustomMapReady":"dmCustomMapUnavailable";
    snprintf(out,capacity,dm_ui(key),s.done,s.total);
}
