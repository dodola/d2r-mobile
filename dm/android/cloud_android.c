#include "cloud_android.h"
#include "services_fs.h"
#include "lifecycle_android.h"
#include "../host/dm.h"
#include "../host/dm_custom.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int dm_cloud_sync_checked(const char *,const char *,const char *,const char *,int,uint64_t,int (*)(const char *));
extern void (*g_dm_save_commit_hook)(const char *);
extern void dm_fs_snapshot_committed(const char *,void (*)(const char *));
static pthread_mutex_t g_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv=PTHREAD_COND_INITIALIZER;
static char g_save[PATH_MAX],g_cloud[PATH_MAX],g_bak[PATH_MAX],g_state[PATH_MAX];
static char g_pending[PATH_MAX],g_active[PATH_MAX];
static char g_provider_id[65],g_cache_root[PATH_MAX],g_ack[PATH_MAX];
static dm_android_cloud_provider_ops g_provider_ops;
static void *g_provider_opaque;
static int g_explicit_transport,g_provider,g_last_error;
static uint64_t g_attempt,g_successful_attempts;
static pthread_t g_worker;
static int g_worker_started,g_stop,g_busy,g_configured;
static int g_pending_ready,g_active_ready,g_deferred,g_flush,g_retry_blocked,g_previous_policy;

static uint64_t wall_ms(void) {
    struct timespec ts;if(clock_gettime(CLOCK_REALTIME,&ts))return 0;
    return (uint64_t)ts.tv_sec*1000ull+(uint64_t)ts.tv_nsec/1000000ull;
}
static int policy(void) {return dm_custom_get("icloud")!=0;}
static int make_dirs(const char *path) {return dm_android_fs_ensure_directory(path);}
static int directory(const char *path) {struct stat st;return !lstat(path,&st)&&S_ISDIR(st.st_mode);}
static int sync_parent(const char *path) {
    char parent[PATH_MAX];strcpy(parent,path);char *slash=strrchr(parent,'/');if(!slash)return -1;
    if(slash==parent)slash[1]=0;else *slash=0;
    int fd=open(parent,O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return -1;
    int rc=fsync(fd),error=errno;close(fd);errno=error;return rc;
}
static int sync_state(void) {
    int fd=open(g_state,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    int rc=fsync(fd),error=errno;close(fd);errno=error;
    return rc?rc:sync_parent(g_state);
}
static int canonical(const char *path,char out[PATH_MAX]) {
    if(realpath(path,out))return 1;
    /* A new provider's final directory may not exist yet. Resolve its
     * existing parent before checking nesting against the save tree. */
    char copy[PATH_MAX],parent[PATH_MAX];strcpy(copy,path);
    size_t n=strlen(copy);while(n>1&&copy[n-1]=='/')copy[--n]=0;
    char *slash=strrchr(copy,'/');if(!slash||!slash[1])return 0;
    char leaf[NAME_MAX+1];if(strlen(slash+1)>NAME_MAX)return 0;strcpy(leaf,slash+1);
    if(slash==copy)slash[1]=0;else *slash=0;
    if(!realpath(copy,parent))return 0;
    return snprintf(out,PATH_MAX,"%s%s%s",parent,strcmp(parent,"/")?"/":"",leaf)<PATH_MAX;
}
static int contains_path(const char *a,const char *b) {
    size_t n=strlen(a);return !strcmp(a,b)||(!strncmp(a,b,n)&&(n==1||b[n]=='/'));
}
static int same_path(const char *a,const char *b) {
    char aa[PATH_MAX],bb[PATH_MAX];
    if(!canonical(a,aa)||!canonical(b,bb))return !strcmp(a,b);
    return contains_path(aa,bb)||contains_path(bb,aa);
}
static void remove_snapshot(const char *path) {
    if(dm_android_fs_remove_tree(path)&&errno!=ENOENT)LOG("Android cloud: snapshot cleanup errno=%d",errno);
}
#include "cloud_attempt_android.inc"
/* A complete tree is published atomically. The durable directory itself is
 * the queue record, including an empty snapshot representing a deletion. */
static int publish_snapshot(const char *source) {
    if(directory(g_pending))return dm_android_fs_commit_snapshot(source,g_pending);
    char tmp[PATH_MAX];if(snprintf(tmp,sizeof tmp,"%s.new",g_pending)>=(int)sizeof tmp){errno=ENAMETOOLONG;return -1;}
    remove_snapshot(tmp);
    if(dm_android_fs_clone_tree(source,tmp))return -1;
    if(rename(tmp,g_pending))return -1;
    return sync_parent(g_pending);
}
static int sync_one(const char *snapshot,int allow_down) {
    if(!policy()){errno=ECANCELED;return -1;}
    if(g_provider)return provider_sync_one(snapshot,allow_down);
    if(make_dirs(g_cloud))return -1;
    int actions=dm_cloud_sync_checked(snapshot,g_cloud,g_bak,g_state,allow_down,wall_ms(),NULL);
    if(actions<0 || dm_android_fs_sync_tree(g_cloud) || sync_state()) {
        LOG("Android cloud: sync failed; durable snapshot retained errno=%d",errno);return -1;
    }
    LOG("Android cloud: %s sync actions=%d",allow_down?"startup":"upload",actions);return 0;
}
static void *cloud_worker(void *unused) {
    (void)unused;
    for(;;) {
        pthread_mutex_lock(&g_mu);
        while(!g_stop && ((!g_pending_ready&&!g_active_ready)||g_retry_blocked||!policy()||(g_deferred&&!g_flush)))
            pthread_cond_wait(&g_cv,&g_mu);
        if((g_stop&&(!g_pending_ready&&!g_active_ready)) || (g_stop&&(!policy()||g_retry_blocked))) {
            pthread_mutex_unlock(&g_mu);break;
        }
        /* A newer committed snapshot includes all characters/deletions and
         * supersedes a failed older snapshot. Never retry a stale generation. */
        if(g_pending_ready) {
            if(g_active_ready)remove_snapshot(g_active);
            if(rename(g_pending,g_active) || sync_parent(g_active)) {
                g_active_ready=directory(g_active);g_pending_ready=directory(g_pending);
                g_retry_blocked=1;pthread_cond_broadcast(&g_cv);
                int stop=g_stop;pthread_mutex_unlock(&g_mu);if(stop)break;continue;
            }
            g_pending_ready=0;g_active_ready=1;
        }
        g_flush=0;g_deferred=0;g_busy=1;
        pthread_mutex_unlock(&g_mu);
        int rc=sync_one(g_active,0);
        int sync_error=rc?(errno?errno:EIO):0;
        pthread_mutex_lock(&g_mu);
        g_last_error=sync_error;
        if(!rc){remove_snapshot(g_active);sync_parent(g_active);g_active_ready=0;}
        else g_retry_blocked=1;
        g_busy=0;int stop=g_stop;pthread_cond_broadcast(&g_cv);pthread_mutex_unlock(&g_mu);
        if(stop)break; /* Shutdown gets one bounded attempt; failure stays on disk. */
    }
    return NULL;
}
int dm_android_cloud_set_directory(const char *path) {
    if(!path||path[0]!='/'||strlen(path)>=sizeof g_cloud)return EINVAL;
    pthread_mutex_lock(&g_mu);
    if(g_configured||g_worker_started){pthread_mutex_unlock(&g_mu);return EALREADY;}
    strcpy(g_cloud,path);g_explicit_transport=1;g_provider=0;
    memset(&g_provider_ops,0,sizeof g_provider_ops);g_provider_opaque=NULL;
    pthread_mutex_unlock(&g_mu);return 0;
}
int dm_android_cloud_set_provider(const char *id,const char *cache,
                                  const dm_android_cloud_provider_ops *ops,void *opaque) {
    if(id||cache||ops||opaque) {
        if(!id||!cache||!ops||!ops->refresh||!ops->publish||!provider_hex(id)||
           cache[0]!='/'||strlen(cache)>=sizeof g_cache_root)return EINVAL;
    }
    pthread_mutex_lock(&g_mu);
    if(g_configured||g_worker_started){pthread_mutex_unlock(&g_mu);return EALREADY;}
    g_explicit_transport=1;g_provider=id!=NULL;g_cloud[0]=0;g_provider_id[0]=0;g_cache_root[0]=0;
    memset(&g_provider_ops,0,sizeof g_provider_ops);g_provider_opaque=NULL;
    if(id){strcpy(g_provider_id,id);strcpy(g_cache_root,cache);g_provider_ops=*ops;g_provider_opaque=opaque;}
    pthread_mutex_unlock(&g_mu);return 0;
}
int dm_android_cloud_status_get(dm_android_cloud_status *out) {
    if(!out)return EINVAL;
    int rc=pthread_mutex_trylock(&g_mu);if(rc)return rc;
    *out=(dm_android_cloud_status){g_configured,g_configured&&policy(),g_busy,
        g_pending_ready,g_active_ready,g_last_error,g_attempt,g_successful_attempts};
    pthread_mutex_unlock(&g_mu);return 0;
}
int dm_android_cloud_enabled(void) {
    pthread_mutex_lock(&g_mu);int enabled=g_configured&&policy();pthread_mutex_unlock(&g_mu);return enabled;
}
void dm_android_cloud_commit(const char *commit_dir) {
    if(!commit_dir)return;
    pthread_mutex_lock(&g_mu);
    if(!g_configured||!g_worker_started||g_stop||!policy()){pthread_mutex_unlock(&g_mu);return;}
    if(publish_snapshot(commit_dir)) {LOG("Android cloud: snapshot failed errno=%d",errno);g_pending_ready=directory(g_pending);g_retry_blocked=1;pthread_cond_broadcast(&g_cv);pthread_mutex_unlock(&g_mu);return;}
    g_pending_ready=1;g_deferred=dm_ingame_now();g_retry_blocked=0;
    pthread_cond_broadcast(&g_cv);pthread_mutex_unlock(&g_mu);
}
int dm_android_cloud_boot(const char *save) {
    if(!save||save[0]!='/'||strlen(save)>=sizeof g_save)return EINVAL;
    pthread_mutex_lock(&g_mu);
    if(g_configured){pthread_mutex_unlock(&g_mu);return 0;}
    const char *configured=g_provider?g_cache_root:g_cloud[0]?g_cloud:
        g_explicit_transport?NULL:getenv("DM_ANDROID_CLOUD_DIR");
    if(!configured||!*configured){pthread_mutex_unlock(&g_mu);return 0;}
    if(configured[0]!='/'||strlen(configured)>=sizeof g_cloud||same_path(configured,save)) {pthread_mutex_unlock(&g_mu);return EINVAL;}
    if(!g_provider&&configured!=g_cloud)strcpy(g_cloud,configured);strcpy(g_save,save);
#define SETPATH(dst,suffix) if(snprintf(dst,sizeof dst,"%s" suffix,g_save)>=(int)sizeof dst){pthread_mutex_unlock(&g_mu);return ENAMETOOLONG;}
    SETPATH(g_bak,".cloudbak");
    char provider_path[PATH_MAX];const char *identity=canonical(g_cloud,provider_path)?provider_path:g_cloud;
    uint64_t provider=0xcbf29ce484222325ull;
    for(const unsigned char *c=(const unsigned char *)identity;*c;c++){provider^=*c;provider*=0x100000001b3ull;}
    char prefix[PATH_MAX];
    int pn=g_provider?snprintf(prefix,sizeof prefix,"%s.cloud-%s",g_save,g_provider_id):
        snprintf(prefix,sizeof prefix,"%s.cloud-%016llx",g_save,(unsigned long long)provider);
    if(pn>=(int)sizeof prefix){pthread_mutex_unlock(&g_mu);return ENAMETOOLONG;}
#define PROVIDERPATH(dst,suffix) if(snprintf(dst,sizeof dst,"%s" suffix,prefix)>=(int)sizeof dst){pthread_mutex_unlock(&g_mu);return ENAMETOOLONG;}
    PROVIDERPATH(g_state,".state");PROVIDERPATH(g_pending,".pending");PROVIDERPATH(g_active,".active");
    PROVIDERPATH(g_ack,".ack");
    if(g_provider && snprintf(g_state,sizeof g_state,"%s/state",g_ack)>=(int)sizeof g_state){pthread_mutex_unlock(&g_mu);return ENAMETOOLONG;}
#undef PROVIDERPATH
#undef SETPATH
    if(make_dirs(g_bak)){int er=errno;pthread_mutex_unlock(&g_mu);return er;}
    g_pending_ready=directory(g_pending);g_active_ready=directory(g_active);
    g_stop=0;g_flush=0;g_deferred=0;g_retry_blocked=0;g_busy=1;
    g_previous_policy=policy();g_configured=1;pthread_mutex_unlock(&g_mu);
    /* Recover pending deliberate deletions before startup is allowed to
     * restore missing characters from the cloud. The guest is not mounted. */
    int rc=0;
    if(policy()) {
        const char *recovery=g_pending_ready?g_pending:g_active_ready?g_active:NULL;
        if(recovery && sync_one(recovery,0))rc=EIO;
        if(!rc && sync_one(g_save,1))rc=EIO;
        if(!rc){remove_snapshot(g_pending);remove_snapshot(g_active);g_pending_ready=g_active_ready=0;}
    }
    pthread_mutex_lock(&g_mu);g_busy=0;g_retry_blocked=rc!=0;g_last_error=rc?errno:0;
    int error=pthread_create(&g_worker,NULL,cloud_worker,NULL);
    if(error){g_configured=0;pthread_mutex_unlock(&g_mu);return error;}
    g_worker_started=1;g_dm_save_commit_hook=dm_android_cloud_commit;
    pthread_mutex_unlock(&g_mu);return rc;
}
void dm_android_cloud_flush(int background) {
    pthread_mutex_lock(&g_mu);
    if(g_configured&&policy()){g_flush=1;g_retry_blocked=0;pthread_cond_broadcast(&g_cv);}
    pthread_mutex_unlock(&g_mu);(void)background;
}
int dm_android_cloud_flush_wait(double seconds) {
    if(seconds<0)seconds=0;if(seconds>60)seconds=60;
    dm_android_cloud_flush(1);
    struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);
    uint64_t ns=(uint64_t)(seconds*1e9);ts.tv_sec+=(time_t)(ns/1000000000ull);ts.tv_nsec+=(long)(ns%1000000000ull);
    if(ts.tv_nsec>=1000000000L){ts.tv_sec++;ts.tv_nsec-=1000000000L;}
    pthread_mutex_lock(&g_mu);int done=1;
    while(g_configured&&policy()&&(g_pending_ready||g_active_ready||g_busy)) {
        if(g_retry_blocked){done=0;break;}
        int rc=pthread_cond_timedwait(&g_cv,&g_mu,&ts);if(rc){done=0;break;}
    }
    pthread_mutex_unlock(&g_mu);return done;
}
void dm_android_cloud_foreground(void) {
    pthread_mutex_lock(&g_mu);g_flush=0;g_retry_blocked=0;
    pthread_cond_broadcast(&g_cv);pthread_mutex_unlock(&g_mu);
}
void dm_android_cloud_message(const char *category,const char *action) {
    if(!category||!action)return;
    if(!strcmp(category,"FrontEndNavigation")&&!strcmp(action,"ToFrontEnd"))dm_android_cloud_flush(0);
    else if(!strcmp(category,"OptionsPanelMessage")&&!strcmp(action,"AcceptChanges")) {
        /* Capture the current committed generation when enabling, so an old
         * pre-disable snapshot cannot overwrite a later local save. */
        pthread_mutex_lock(&g_mu);int on=policy();int ready=g_configured&&!g_stop&&on&&!g_previous_policy;g_previous_policy=on;pthread_mutex_unlock(&g_mu);
        if(ready)dm_fs_snapshot_committed(g_save,dm_android_cloud_commit);
        else if(!on){pthread_mutex_lock(&g_mu);g_flush=0;
            void (*cancel)(uint64_t,void *)=g_busy?g_provider_ops.cancel:NULL;
            uint64_t attempt=g_attempt;void *opaque=g_provider_opaque;
            pthread_cond_broadcast(&g_cv);pthread_mutex_unlock(&g_mu);if(cancel&&attempt)cancel(attempt,opaque);}
    }
}
void dm_android_cloud_shutdown(void) {
    pthread_mutex_lock(&g_mu);if(!g_worker_started){pthread_mutex_unlock(&g_mu);return;}
    g_stop=1;void (*cancel)(uint64_t,void *)=g_busy?g_provider_ops.cancel:NULL;
    uint64_t attempt=g_attempt;void *opaque=g_provider_opaque;
    pthread_cond_broadcast(&g_cv);pthread_mutex_unlock(&g_mu);
    if(cancel&&attempt)cancel(attempt,opaque);
    pthread_join(g_worker,NULL);
    pthread_mutex_lock(&g_mu);g_worker_started=0;g_configured=0;g_busy=0;g_dm_save_commit_hook=NULL;
    pthread_mutex_unlock(&g_mu);
}
