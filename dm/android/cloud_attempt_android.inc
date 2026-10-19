/* Included by cloud_android.c so the legacy queue fixture also exercises
 * this exact transaction boundary. No provider I/O runs under g_mu. */
#include <dirent.h>
#include <stdlib.h>
#include <sys/syscall.h>
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1u<<1)
#endif
int dm_cloud_inbox_add(const char *,const char *,const char *);
int dm_cloud_bak_pack(const char *,const char *,const char *,const char *);

static int provider_hex(const char *text) {
    if(!text||strlen(text)!=64)return 0;
    for(unsigned i=0;i<64;i++)if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return 0;
    return 1;
}
static int attempt_path(char out[PATH_MAX],const char *parent,const char *leaf) {
    if(snprintf(out,PATH_MAX,"%s/%s",parent,leaf)>=PATH_MAX){errno=ENAMETOOLONG;return -1;}
    return 0;
}
static int provider_result(int result) {if(!result)return 0;errno=result>0?result:EIO;return -1;}
static int provider_cancelled(void) {
    pthread_mutex_lock(&g_mu);int cancelled=g_stop;pthread_mutex_unlock(&g_mu);
    if(cancelled||!policy()){errno=ECANCELED;return 1;}return 0;
}
static int provider_unit(const char *name) {
    size_t n=strlen(name);if(!n||n>63||name[0]=='.'||strstr(name,"..")||(name[0]=='_'&&strcmp(name,"_stash")))return 0;
    for(const unsigned char *p=(const unsigned char *)name;*p;p++)if(*p<32||*p==127||*p=='/'||*p=='\\')return 0;
    return 1;
}
static int conflict_seen(const char *seen,const char *record) {
    FILE *f=fopen(seen,"r");if(!f)return errno==ENOENT?0:-1;
    char line[144];int found=0;
    while(fgets(line,sizeof line,f))if(!strcmp(line,record)){found=1;break;}
    if(ferror(f))found=-1;if(fclose(f))found=-1;return found;
}
static int provider_conflicts(const char *save,const char *cache,const char *ackdir,const char *state) {
    char conflicts[PATH_MAX],seen[PATH_MAX];
    if(attempt_path(conflicts,cache,"conflicts")||attempt_path(seen,ackdir,"conflicts.seen"))return -1;
    DIR *units=opendir(conflicts);if(!units)return errno==ENOENT?0:-1;
    struct dirent *unit;int rc=0,error=0;
    for(;;) {
        errno=0;unit=readdir(units);if(!unit){if(errno){rc=-1;error=errno;}break;}
        if(!strcmp(unit->d_name,".")||!strcmp(unit->d_name,".."))continue;
        char folder[PATH_MAX];struct stat st;
        if(!provider_unit(unit->d_name)||attempt_path(folder,conflicts,unit->d_name)||lstat(folder,&st)||!S_ISDIR(st.st_mode)){rc=-1;error=EINVAL;break;}
        DIR *versions=opendir(folder);if(!versions){rc=-1;error=errno;break;}
        struct dirent *entry;
        for(;;) {
            errno=0;entry=readdir(versions);if(!entry){if(errno){rc=-1;error=errno;}break;}
            if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
            char id[65],pack[PATH_MAX],record[144];size_t n=strlen(entry->d_name);
            if(n!=69||strcmp(entry->d_name+64,".pack")){rc=-1;error=EINVAL;break;}
            memcpy(id,entry->d_name,64);id[64]=0;
            if(!provider_hex(id)||attempt_path(pack,folder,entry->d_name)||lstat(pack,&st)||!S_ISREG(st.st_mode)){rc=-1;error=EINVAL;break;}
            snprintf(record,sizeof record,"%s/%s\n",unit->d_name,id);
            int found=conflict_seen(seen,record);if(found<0){rc=-1;error=errno;break;}if(found)continue;
            int imported=!strcmp(unit->d_name,"_stash")?dm_cloud_inbox_add(save,state,pack):
                dm_cloud_bak_pack(g_bak,unit->d_name,pack,"云端冲突版本");
            if(imported){rc=-1;error=errno?errno:EIO;break;}
            FILE *f=fopen(seen,"a");if(!f){rc=-1;error=errno;break;}
            int bad=fputs(record,f)<0;if(fclose(f))bad=1;
            if(bad){rc=-1;error=errno?errno:EIO;break;}
        }
        if(closedir(versions)&&!rc){rc=-1;error=errno;}if(rc)break;
    }
    if(closedir(units)&&!rc){rc=-1;error=errno;}if(rc)errno=error;return rc;
}
/* Stage beside ack even when the disposable cache is on another filesystem.
 * Exchange the entire state/base/seen generation, then persist the parent.
 * A failed parent fsync restores the previous acknowledged bytes. */
static int provider_ack(const char *state_dir,uint64_t attempt) {
    char stage[PATH_MAX];struct stat st;
    if(snprintf(stage,sizeof stage,"%s.new-%llu",g_ack,(unsigned long long)attempt)>=(int)sizeof stage){errno=ENAMETOOLONG;return -1;}
    if(dm_android_fs_remove_tree(stage)&&errno!=ENOENT)return -1;
    if(dm_android_fs_clone_tree(state_dir,stage))return -1;
    int exists=!lstat(g_ack,&st),rc=-1,error=0;
    if(!exists&&errno!=ENOENT)goto done;
    if(exists&&!S_ISDIR(st.st_mode)){errno=EINVAL;goto done;}
    if(provider_cancelled())goto done;
    if(exists)rc=(int)syscall(__NR_renameat2,AT_FDCWD,stage,AT_FDCWD,g_ack,RENAME_EXCHANGE);
    else rc=rename(stage,g_ack);
    if(rc)goto done;
    if(sync_parent(g_ack)) {
        error=errno;
        int rollback=exists?(int)syscall(__NR_renameat2,AT_FDCWD,stage,AT_FDCWD,g_ack,RENAME_EXCHANGE):rename(g_ack,stage);
        if(rollback){LOG("Android cloud: ack rollback failed; retain both complete generations errno=%d",errno);errno=error;return -1;}
        (void)sync_parent(g_ack);errno=error;rc=-1;goto done;
    }
    rc=0;
done:
    error=errno;if(!rc)remove_snapshot(stage);else if(directory(stage))remove_snapshot(stage);
    errno=error;return rc;
}
static int provider_sync_one(const char *snapshot,int allow_down) {
    char provider_dir[PATH_MAX],attempt_dir[PATH_MAX],cache[PATH_MAX],state_dir[PATH_MAX],state[PATH_MAX],leaf[64];
    pthread_mutex_lock(&g_mu);uint64_t attempt=++g_attempt;pthread_mutex_unlock(&g_mu);
    int rc=-1,error=0;struct stat st;
    if(make_dirs(g_cache_root)||attempt_path(provider_dir,g_cache_root,g_provider_id)||make_dirs(provider_dir))goto done;
    snprintf(leaf,sizeof leaf,"attempt-%llu",(unsigned long long)attempt);
    if(attempt_path(attempt_dir,provider_dir,leaf))goto done;
    if(dm_android_fs_remove_tree(attempt_dir)&&errno!=ENOENT)goto done;
    if(make_dirs(attempt_dir)||attempt_path(cache,attempt_dir,"packs")||make_dirs(cache)||
       attempt_path(state_dir,attempt_dir,"ack")||attempt_path(state,state_dir,"state"))goto done;
    if(!lstat(g_ack,&st)) {if(!S_ISDIR(st.st_mode)){errno=EINVAL;goto done;}if(dm_android_fs_clone_tree(g_ack,state_dir))goto done;}
    else if(errno!=ENOENT||make_dirs(state_dir))goto done;
    if(provider_cancelled())goto done;
    if(provider_result(g_provider_ops.refresh(cache,attempt,g_provider_opaque)))goto done;
    if(provider_cancelled())goto done;
    if(allow_down&&provider_conflicts(snapshot,cache,state_dir,state))goto done;
    int actions=dm_cloud_sync_checked(snapshot,cache,g_bak,state,allow_down,wall_ms(),NULL);
    if(actions<0||dm_android_fs_sync_tree(cache)||dm_android_fs_sync_tree(state_dir))goto done;
    if(provider_cancelled())goto done;
    if(provider_result(g_provider_ops.publish(cache,attempt,g_provider_opaque)))goto done;
    if(provider_cancelled())goto done;
    if(provider_ack(state_dir,attempt))goto done;
    rc=0;
done:
    error=rc?(errno?errno:EIO):0;
    pthread_mutex_lock(&g_mu);g_last_error=error;if(!rc)++g_successful_attempts;pthread_mutex_unlock(&g_mu);
    if(!rc)remove_snapshot(attempt_dir);
    else LOG("Android cloud: provider attempt %llu failed errno=%d; ack and queue retained",(unsigned long long)attempt,error);
    errno=error;return rc;
}
