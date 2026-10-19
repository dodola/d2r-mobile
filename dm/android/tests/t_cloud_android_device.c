/* Production queue source + actual library pack/FS implementation. Uses only
 * mkdtemp fixtures; no Activity, provider account or real player save. */
#include <dlfcn.h>
#include <stdlib.h>
#include <stdatomic.h>
static _Atomic int setting=1,ingame=1;
#define dm_custom_get fixture_custom_get
#define dm_ingame_now fixture_ingame
#define dm_android_fs_clone_tree fixture_clone
#define dm_android_fs_commit_snapshot fixture_commit
#define dm_android_fs_remove_tree fixture_remove
#define dm_android_fs_ensure_directory fixture_directory
#define dm_android_fs_sync_tree fixture_sync
#define dm_cloud_sync_checked fixture_cloud_sync
#define dm_cloud_inbox_add fixture_inbox_add
#define dm_cloud_bak_pack fixture_bak_pack
#define dm_fs_snapshot_committed fixture_snapshot_committed
#include "../cloud_android.c"
#undef dm_custom_get
#undef dm_ingame_now
static void *library;
static _Atomic unsigned checks;
static _Atomic int fail_fsync,fail_after,failed_fsync;
static char fail_path[1400];
#include <sys/syscall.h>
#define CHECK(e) do{atomic_fetch_add(&checks,1);if(!(e)){fprintf(stderr,"Cloud fixture failed line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
int fixture_custom_get(const char *key){CHECK(!strcmp(key,"icloud"));return atomic_load(&setting);}
int fixture_ingame(void){return atomic_load(&ingame);}
void (*g_dm_save_commit_hook)(const char *);
#define PROXY2(name,real) int name(const char *a,const char *b){int (*fn)(const char *,const char *)=dlsym(library,real);CHECK(fn);return fn(a,b);}
#define PROXY1(name,real) int name(const char *a){int (*fn)(const char *)=dlsym(library,real);CHECK(fn);return fn(a);}
PROXY2(fixture_clone,"dm_android_fs_clone_tree")
PROXY2(fixture_commit,"dm_android_fs_commit_snapshot")
PROXY1(fixture_remove,"dm_android_fs_remove_tree")
PROXY1(fixture_directory,"dm_android_fs_ensure_directory")
PROXY1(fixture_sync,"dm_android_fs_sync_tree")
int fixture_inbox_add(const char *a,const char *b,const char *c) {
    int (*fn)(const char *,const char *,const char *)=dlsym(library,"dm_cloud_inbox_add");CHECK(fn);return fn(a,b,c);
}
int fixture_bak_pack(const char *a,const char *b,const char *c,const char *d) {
    int (*fn)(const char *,const char *,const char *,const char *)=dlsym(library,"dm_cloud_bak_pack");CHECK(fn);return fn(a,b,c,d);
}
/* Export only this symbol from the probe. Production library fsync calls are
 * interrupted solely for a file/parent inside this fixture's random root. */
int fsync(int fd) {
    if(atomic_load(&fail_fsync)) {
        char link[64],target[PATH_MAX];snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,target,sizeof target-1);
        if(n>=0){target[n]=0;if(!strcmp(target,fail_path)) {
            if(atomic_load(&fail_after)>0)atomic_fetch_sub(&fail_after,1);
            else {atomic_fetch_add(&failed_fsync,1);errno=EIO;return -1;}
        }}
    }
    return (int)syscall(__NR_fsync,fd);
}
static pthread_mutex_t lock_test_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lock_test_cv=PTHREAD_COND_INITIALIZER;
static int lock_test_active,fs_held,enable_waiting;
static const char *lock_test_save;
static void production_snapshot(const char *p,void (*consume)(const char *)) {
    void (*fn)(const char *,void (*)(const char *))=dlsym(library,"dm_fs_snapshot_committed");CHECK(fn);fn(p,consume);
}
void fixture_snapshot_committed(const char *p,void (*consume)(const char *)) {
    pthread_mutex_lock(&lock_test_mu);
    if(lock_test_active){enable_waiting=1;pthread_cond_broadcast(&lock_test_cv);}
    pthread_mutex_unlock(&lock_test_mu);production_snapshot(p,consume);
}
static void concurrent_commit(const char *p) {
    /* This callback holds the real FS mutex, exactly as the post-Commit hook
     * does. Enabling must wait for it without owning the cloud queue mutex. */
    pthread_mutex_lock(&lock_test_mu);fs_held=1;pthread_cond_broadcast(&lock_test_cv);
    while(!enable_waiting)pthread_cond_wait(&lock_test_cv,&lock_test_mu);
    pthread_mutex_unlock(&lock_test_mu);dm_android_cloud_commit(p);
}
static void *commit_thread(void *unused){(void)unused;production_snapshot(lock_test_save,concurrent_commit);return NULL;}
static void *enable_thread(void *unused){(void)unused;atomic_store(&setting,1);dm_android_cloud_message("OptionsPanelMessage","AcceptChanges");return NULL;}
static void concurrent_enable(const char *save) {
    lock_test_save=save;lock_test_active=1;pthread_t commit,enable;
    CHECK(!pthread_create(&commit,NULL,commit_thread,NULL));
    pthread_mutex_lock(&lock_test_mu);while(!fs_held)pthread_cond_wait(&lock_test_cv,&lock_test_mu);pthread_mutex_unlock(&lock_test_mu);
    CHECK(!pthread_create(&enable,NULL,enable_thread,NULL));
    CHECK(!pthread_join(commit,NULL));CHECK(!pthread_join(enable,NULL));
    CHECK(fs_held&&enable_waiting);lock_test_active=0;
}
int fixture_cloud_sync(const char *a,const char *b,const char *c,const char *d,int e,uint64_t f,int (*g)(const char *)){
    int (*fn)(const char *,const char *,const char *,const char *,int,uint64_t,int (*)(const char *))=dlsym(library,"dm_cloud_sync_checked");CHECK(fn);return fn(a,b,c,d,e,f,g);
}
static void file(const char *p,const char *text){FILE *f=fopen(p,"w");CHECK(f);CHECK(fputs(text,f)>=0);CHECK(!fclose(f));}
static int exists(const char *p){struct stat st;return !stat(p,&st);}
static void path(char *out,const char *root,const char *leaf){CHECK(snprintf(out,1400,"%s/%s",root,leaf)<1400);}
static void await_idle(void){CHECK(dm_android_cloud_flush_wait(3));}
static void content(const char *p,const char *text){char b[100];FILE *f=fopen(p,"r");CHECK(f);size_t n=fread(b,1,sizeof b-1,f);b[n]=0;CHECK(!fclose(f));CHECK(!strcmp(b,text));}
static size_t bytes(const char *p,char b[4096]){FILE *f=fopen(p,"rb");CHECK(f);size_t n=fread(b,1,4096,f);CHECK(!ferror(f)&&n<4096);CHECK(!fclose(f));return n;}
static void equal_files(const char *a,const char *b){char x[4096],y[4096];size_t n=bytes(a,x),m=bytes(b,y);CHECK(n==m&&!memcmp(x,y,n));}
static void copy_file(const char *a,const char *b){char x[4096];size_t n=bytes(a,x);FILE *f=fopen(b,"wb");CHECK(f);CHECK(fwrite(x,1,n,f)==n);CHECK(!fclose(f));}
static const char *expected_pack;
static void pack_file(const char *rel,const uint8_t *data,uint32_t n,void *context){(void)context;CHECK(!strcmp(rel,"CloudTest/CloudTest.d2s"));CHECK(n==strlen(expected_pack)&&!memcmp(data,expected_pack,n));}
static void pack_content(const char *pack,const char *text){int (*fn)(const char *,void (*)(const char *,const uint8_t *,uint32_t,void *),void *)=dlsym(library,"dm_cloud_pack_each");CHECK(fn);expected_pack=text;CHECK(fn(pack,pack_file,NULL)==1);}
static void fail_flush(const char *path,unsigned skip){strcpy(fail_path,path);atomic_store(&fail_after,(int)skip);atomic_store(&fail_fsync,1);}
int main(int argc,char **argv){
    alarm(30);
    CHECK(argc==2);library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!library){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
    char root[]="/data/local/tmp/d2r-cloud-XXXXXX",save[1400],cloud[1400],character[1400],d2s[1400],pack[1400],offline[1400],pending[1400],active[1400];CHECK(mkdtemp(root));
    path(save,root,"save");path(cloud,root,"cloud");path(offline,root,"cloud.offline");CHECK(!mkdir(save,0700));
    path(character,save,"CloudTest");CHECK(!mkdir(character,0700));path(d2s,character,"CloudTest.d2s");file(d2s,"save-one");
    path(pack,cloud,"CloudTest.pack");path(pending,root,"save.cloudpending");path(active,root,"save.cloudactive");
    CHECK(dm_android_cloud_set_directory("relative")==EINVAL);CHECK(!dm_android_cloud_set_directory(cloud));
    /* Disabled at boot neither creates the provider nor uploads anything. */
    atomic_store(&setting,0);CHECK(!dm_android_cloud_boot(save));strcpy(pending,g_pending);strcpy(active,g_active);CHECK(!dm_android_cloud_enabled());CHECK(!exists(cloud));
    dm_android_cloud_commit(save);CHECK(!exists(pending));CHECK(dm_android_cloud_flush_wait(.01));
    /* Enabling captures the latest committed tree; in-game commits defer. */
    atomic_store(&setting,1);dm_android_cloud_message("OptionsPanelMessage","AcceptChanges");CHECK(dm_android_cloud_enabled());
    CHECK(exists(pending));CHECK(!exists(pack));dm_android_cloud_message("FrontEndNavigation","ToFrontEnd");await_idle();CHECK(exists(pack));CHECK(!exists(active));
    /* A failed transfer is observable and retains its full durable tree. */
    CHECK(!rename(cloud,offline));file(cloud,"provider-unavailable");file(d2s,"save-two");dm_android_cloud_commit(save);
    CHECK(!dm_android_cloud_flush_wait(3));CHECK(exists(active));path(character,active,"CloudTest/CloudTest.d2s");content(character,"save-two");
    CHECK(!unlink(cloud));CHECK(!rename(offline,cloud));await_idle();CHECK(!exists(active));
    /* Corrupt cloud data is an error, rather than a successful zero-action
     * sync causing the newest local snapshot to be acknowledged and erased. */
    file(pack,"broken pack");file(d2s,"save-three");dm_android_cloud_commit(save);CHECK(!dm_android_cloud_flush_wait(3));CHECK(exists(active));
    CHECK(!unlink(pack));await_idle();CHECK(!exists(active));
    /* Disabled commits cannot publish; re-enable replaces the older pending
     * generation with the actual latest local save before retrying. */
    file(d2s,"save-four");dm_android_cloud_commit(save);CHECK(exists(pending));atomic_store(&setting,0);dm_android_cloud_message("OptionsPanelMessage","AcceptChanges");
    file(d2s,"save-five");dm_android_cloud_commit(save);CHECK(dm_android_cloud_flush_wait(.01));
    concurrent_enable(save);path(character,pending,"CloudTest/CloudTest.d2s");content(character,"save-five");await_idle();
    /* Fail after the new pack is visible, then simulate a crash/provider
     * rollback to its previous durable bytes. State must not advance first.
     * A second failed flush of the matching replacement covers the no-op
     * retry branch too. All fsync calls are from the actual shared library. */
    char old_pack[1400],old_state[1400];path(old_pack,root,"previous.pack");path(old_state,root,"previous.state");
    copy_file(pack,old_pack);copy_file(g_state,old_state);file(d2s,"save-six");dm_android_cloud_commit(save);
    fail_flush(pack,1);CHECK(!dm_android_cloud_flush_wait(3));CHECK(atomic_load(&failed_fsync)>0);CHECK(exists(active));
    equal_files(g_state,old_state);atomic_store(&fail_fsync,0);pack_content(pack,"save-six");
    fail_flush(cloud,1);CHECK(!dm_android_cloud_flush_wait(3));CHECK(exists(active));equal_files(g_state,old_state);
    atomic_store(&fail_fsync,0);copy_file(old_pack,pack);await_idle();CHECK(!exists(active));pack_content(pack,"save-six");
    /* A present but unreadable/corrupt state is not a first-sync state. Do
     * not acknowledge the durable snapshot or replace either local/cloud. */
    copy_file(g_state,old_state);CHECK(!unlink(g_state));CHECK(!mkdir(g_state,0700));
    file(d2s,"save-state-error");dm_android_cloud_commit(save);CHECK(!dm_android_cloud_flush_wait(3));CHECK(exists(active));pack_content(pack,"save-six");
    CHECK(!rmdir(g_state));copy_file(old_state,g_state);file(g_state,"invalid state\n");
    CHECK(!dm_android_cloud_flush_wait(3));CHECK(exists(active));pack_content(pack,"save-six");
    copy_file(old_state,g_state);await_idle();CHECK(!exists(active));pack_content(pack,"save-state-error");
    /* Queue survives shutdown failure and is recovered before startup's
     * missing-character restore, including an empty deletion snapshot. */
    CHECK(!rename(cloud,offline));file(cloud,"offline");CHECK(!unlink(d2s));path(character,save,"CloudTest");CHECK(!rmdir(character));dm_android_cloud_commit(save);
    CHECK(!dm_android_cloud_flush_wait(3));dm_android_cloud_shutdown();CHECK(exists(active));
    CHECK(!unlink(cloud));CHECK(!rename(offline,cloud));CHECK(!dm_android_cloud_boot(save));CHECK(!exists(d2s));CHECK(!exists(active));CHECK(!exists(pending));
    FILE *f=fopen(pack,"rb");CHECK(f);unsigned char header[24];CHECK(fread(header,1,sizeof header,f)==sizeof header);CHECK(!fclose(f));CHECK(header[16]==1);
    /* A deletion's failed pack fsync must likewise survive a restart without
     * treating the old provider pack as a newer remote save to restore. */
    path(character,save,"CloudTest");CHECK(!mkdir(character,0700));file(d2s,"save-seven");dm_android_cloud_commit(save);await_idle();
    copy_file(pack,old_pack);copy_file(g_state,old_state);CHECK(!unlink(d2s));CHECK(!rmdir(character));dm_android_cloud_commit(save);
    fail_flush(pack,1);CHECK(!dm_android_cloud_flush_wait(3));CHECK(exists(active));equal_files(g_state,old_state);
    dm_android_cloud_shutdown();CHECK(exists(active));atomic_store(&fail_fsync,0);copy_file(old_pack,pack);
    CHECK(!dm_android_cloud_boot(save));CHECK(!exists(d2s));CHECK(!exists(active)&&!exists(pending));
    f=fopen(pack,"rb");CHECK(f);CHECK(fread(header,1,sizeof header,f)==sizeof header);CHECK(!fclose(f));CHECK(header[16]==1);
    dm_android_cloud_shutdown();CHECK(!g_dm_save_commit_hook);CHECK(!fixture_remove(root));
    printf("PASS cloud durable queue checks=%u (local fixture; no remote-provider E2E)\n",atomic_load(&checks));return 0;
}
