/* Real portable core and FS, production native queue/attempt source, mock
 * complete-listing/readback provider. Everything is under a random tmp root. */
#define main legacy_cloud_queue_main
#include "t_cloud_android_device.c"
#undef main

typedef struct {
    char remote[1400],cache[PATH_MAX],ack_parent[1400];
    _Atomic uint64_t attempt;
    _Atomic unsigned refreshes,publishes,cancels,tombs;
    _Atomic int refresh_error,publish_error,readback_error,disable,ack_error,hold,held;
} mock_store;
static const char provider_a[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char provider_b[]="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static unsigned entries(const char *folder) {
    DIR *d=opendir(folder);if(!d){CHECK(errno==ENOENT);return 0;}unsigned n=0;struct dirent *e;
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,".."))++n;
    CHECK(!closedir(d));return n;
}
static void equal_tree(const char *a,const char *b) {
    CHECK(entries(a)==entries(b));DIR *d=opendir(a);CHECK(d);struct dirent *e;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char x[1400],y[1400];struct stat sx,sy;path(x,a,e->d_name);path(y,b,e->d_name);
        CHECK(!lstat(x,&sx)&&!lstat(y,&sy));CHECK((sx.st_mode&S_IFMT)==(sy.st_mode&S_IFMT));
        if(S_ISDIR(sx.st_mode))equal_tree(x,y);else {CHECK(S_ISREG(sx.st_mode));equal_files(x,y);}
    }
    CHECK(!closedir(d));
}
static void stash_file(const char *p,uint16_t items,uint32_t gold) {
    unsigned char b[68]={0};uint32_t length=sizeof b;
    memcpy(b,"\x55\xaa\x55\xaa",4);memcpy(b+12,&gold,4);memcpy(b+16,&length,4);
    memcpy(b+64,"JM",2);memcpy(b+66,&items,2);FILE *f=fopen(p,"wb");CHECK(f);
    CHECK(fwrite(b,1,sizeof b,f)==sizeof b);CHECK(!fclose(f));
}
static int is_tomb(const char *pack) {
    unsigned char h[24];FILE *f=fopen(pack,"rb");CHECK(f);CHECK(fread(h,1,sizeof h,f)==sizeof h);CHECK(!fclose(f));return h[16]==1;
}
static void callback_status(void) {
    dm_android_cloud_status s;CHECK(!dm_android_cloud_status_get(&s));CHECK(s.configured&&s.busy);
}
static int mock_refresh(const char *cache,uint64_t attempt,void *opaque) {
    mock_store *m=opaque;callback_status();CHECK(attempt>atomic_load(&m->attempt));CHECK(!entries(cache));
    atomic_store(&m->attempt,attempt);strcpy(m->cache,cache);atomic_fetch_add(&m->refreshes,1);
    if(atomic_load(&m->refresh_error)) {char partial[1400];path(partial,cache,"incomplete-listing");file(partial,"partial");return EIO;}
    CHECK(!fixture_remove(cache));CHECK(!fixture_clone(m->remote,cache));return 0;
}
static int mock_publish(const char *cache,uint64_t attempt,void *opaque) {
    mock_store *m=opaque;callback_status();CHECK(attempt==atomic_load(&m->attempt));CHECK(!strcmp(cache,m->cache));
    atomic_fetch_add(&m->publishes,1);
    if(atomic_load(&m->hold)) {
        unsigned before=atomic_load(&m->cancels);atomic_store(&m->held,1);
        while(atomic_load(&m->cancels)==before)usleep(1000);
        return ECANCELED;
    }
    if(atomic_load(&m->publish_error))return EIO;
    CHECK(!fixture_remove(m->remote));CHECK(!fixture_clone(cache,m->remote));
    char pack[1400];path(pack,cache,"CloudTest.pack");if(exists(pack)&&is_tomb(pack))atomic_fetch_add(&m->tombs,1);
    if(atomic_load(&m->readback_error))return EIO;
    if(atomic_exchange(&m->disable,0)) {atomic_store(&setting,0);dm_android_cloud_message("OptionsPanelMessage","AcceptChanges");}
    if(atomic_exchange(&m->ack_error,0))fail_flush(m->ack_parent,1);
    return 0;
}
static void mock_cancel(uint64_t attempt,void *opaque) {
    mock_store *m=opaque;CHECK(attempt==atomic_load(&m->attempt));atomic_fetch_add(&m->cancels,1);
}
static const dm_android_cloud_provider_ops mock_ops={mock_refresh,mock_publish,mock_cancel};
static void remember_ack(const char *copy) {CHECK(!fixture_remove(copy));CHECK(!fixture_clone(g_ack,copy));}
static void check_ack(const char *copy) {equal_tree(g_ack,copy);}
static void wait_error(int error) {
    for(unsigned i=0;i<3000;i++) {dm_android_cloud_status s;int rc=dm_android_cloud_status_get(&s);
        if(!rc&&!s.busy&&s.last_error==error)return;usleep(1000);}
    CHECK(0);
}
static void wait_held(mock_store *m) {for(unsigned i=0;i<3000;i++){if(atomic_load(&m->held))return;usleep(1000);}CHECK(0);}
static void count_inbox(const char *save,unsigned expected) {char p[1400];path(p,save,"_cloudinbox");CHECK(entries(p)==expected);}
static void conflict_pack(const char *remote,const char *unit,char hex,const char *source) {
    char folders[1400],folder[1400],pack[1400],leaf[70];path(folders,remote,"conflicts");CHECK(!fixture_directory(folders));
    path(folder,folders,unit);CHECK(!fixture_directory(folder));memset(leaf,hex,64);strcpy(leaf+64,".pack");path(pack,folder,leaf);copy_file(source,pack);
}
/* A causal generation can preserve the underlying save's millisecond stamp.
 * Exercise the real checked core through both runtime and startup paths. */
static void same_stamp_remote(const char *root,const char *save,const char *d2s,mock_store *m) {
    char remote_pack[1400],previous[1400],scratch[1400],scratchchar[1400],scratchfile[1400],scratchcloud[1400],scratchbak[1400],scratchstate[1400];
    path(remote_pack,m->remote,"CloudTest.pack");path(previous,root,"same-stamp-before.pack");copy_file(remote_pack,previous);
    path(scratch,root,"same-stamp-save");CHECK(!mkdir(scratch,0700));path(scratchchar,scratch,"CloudTest");CHECK(!mkdir(scratchchar,0700));
    path(scratchfile,scratchchar,"CloudTest.d2s");file(scratchfile,"same-stamp-remote");
    struct stat st;CHECK(!stat(d2s,&st));struct timespec ts[2]={st.st_atim,st.st_mtim};CHECK(!utimensat(AT_FDCWD,scratchfile,ts,0));
    path(scratchcloud,root,"same-stamp-cloud");path(scratchbak,root,"same-stamp-bak");path(scratchstate,root,"same-stamp-state");
    CHECK(!mkdir(scratchcloud,0700)&&!mkdir(scratchbak,0700));CHECK(fixture_cloud_sync(scratch,scratchcloud,scratchbak,scratchstate,1,wall_ms(),NULL)>=0);
    char replacement[1400];path(replacement,scratchcloud,"CloudTest.pack");copy_file(replacement,remote_pack);
    char a[4096],b[4096];CHECK(bytes(previous,a)>24&&bytes(remote_pack,b)>24);CHECK(!memcmp(a+8,b+8,8));
    dm_android_cloud_commit(save);await_idle();content(d2s,"save-six");pack_content(remote_pack,"same-stamp-remote");
    dm_android_cloud_shutdown();CHECK(!dm_android_cloud_boot(save));content(d2s,"same-stamp-remote");pack_content(remote_pack,"same-stamp-remote");
}

int main(int argc,char **argv) {
    alarm(60);CHECK(argc==2);library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!library){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
    char root[]="/data/local/tmp/d2r-cloud-provider-XXXXXX",save[1400],cache[1400],cache2[1400],character[1400],d2s[1400],stash[1400],pack[1400],base[1400],ackcopy[1400];
    CHECK(mkdtemp(root));path(save,root,"save");path(cache,root,"cache");path(cache2,root,"cache2");CHECK(!mkdir(save,0700)&&!mkdir(cache,0700)&&!mkdir(cache2,0700));
    path(character,save,"CloudTest");CHECK(!mkdir(character,0700));path(d2s,character,"CloudTest.d2s");file(d2s,"save-one");
    path(stash,save,"SharedStashSoftCoreV2.d2i");stash_file(stash,1,100);path(ackcopy,root,"ack-copy");
    mock_store a={0},b={0};path(a.remote,root,"remote-a");path(b.remote,root,"remote-b");CHECK(!mkdir(a.remote,0700)&&!mkdir(b.remote,0700));
    strcpy(a.ack_parent,root);strcpy(b.ack_parent,root);path(pack,a.remote,"CloudTest.pack");
    CHECK(!setenv("DM_ANDROID_CLOUD_DIR",a.remote,1));CHECK(!dm_android_cloud_set_provider(NULL,NULL,NULL,NULL));CHECK(!dm_android_cloud_boot(save));
    CHECK(!dm_android_cloud_enabled()&&!g_configured&&!exists(pack));CHECK(!unsetenv("DM_ANDROID_CLOUD_DIR"));
    CHECK(dm_android_cloud_set_provider("bad",cache,&mock_ops,&a)==EINVAL);
    CHECK(dm_android_cloud_set_provider(provider_a,"relative",&mock_ops,&a)==EINVAL);
    CHECK(dm_android_cloud_set_provider(provider_a,cache,NULL,&a)==EINVAL);
    CHECK(!dm_android_cloud_set_provider(provider_a,cache,&mock_ops,&a));CHECK(!dm_android_cloud_boot(save));
    CHECK(atomic_load(&a.refreshes)==1&&atomic_load(&a.publishes)==1);pack_content(pack,"save-one");
    CHECK(dm_android_cloud_set_provider(provider_b,cache,&mock_ops,&b)==EALREADY);
    CHECK(snprintf(base,sizeof base,"%s.stash_base",g_state)<(int)sizeof base);CHECK(exists(base));
    dm_android_cloud_status out,before;memset(&out,0xa5,sizeof out);before=out;pthread_mutex_lock(&g_mu);
    CHECK(dm_android_cloud_status_get(&out)==EBUSY);pthread_mutex_unlock(&g_mu);CHECK(!memcmp(&out,&before,sizeof out));CHECK(dm_android_cloud_status_get(NULL)==EINVAL);
    remember_ack(ackcopy);unsigned publishes=atomic_load(&a.publishes);atomic_store(&a.refresh_error,1);
    file(d2s,"save-two");dm_android_cloud_commit(save);CHECK(!dm_android_cloud_flush_wait(3));CHECK(exists(g_active));check_ack(ackcopy);CHECK(atomic_load(&a.publishes)==publishes);
    atomic_store(&a.refresh_error,0);await_idle();CHECK(!exists(g_active));pack_content(pack,"save-two");

    remember_ack(ackcopy);file(d2s,"save-three");stash_file(stash,1,200);atomic_store(&a.publish_error,1);
    dm_android_cloud_commit(save);CHECK(!dm_android_cloud_flush_wait(3));check_ack(ackcopy);CHECK(exists(g_active));pack_content(pack,"save-two");
    atomic_store(&a.publish_error,0);atomic_store(&a.readback_error,1);CHECK(!dm_android_cloud_flush_wait(3));check_ack(ackcopy);CHECK(exists(g_active));pack_content(pack,"save-three");
    atomic_store(&a.readback_error,0);await_idle();CHECK(!exists(g_active));pack_content(pack,"save-three");

    remember_ack(ackcopy);file(d2s,"save-four");unsigned cancels=atomic_load(&a.cancels);atomic_store(&a.disable,1);
    dm_android_cloud_commit(save);(void)dm_android_cloud_flush_wait(3);wait_error(ECANCELED);CHECK(atomic_load(&a.cancels)==cancels+1);CHECK(exists(g_active));check_ack(ackcopy);
    atomic_store(&setting,1);dm_android_cloud_message("OptionsPanelMessage","AcceptChanges");await_idle();pack_content(pack,"save-four");

    remember_ack(ackcopy);file(d2s,"save-five");atomic_store(&a.ack_error,1);int failures=atomic_load(&failed_fsync);
    dm_android_cloud_commit(save);CHECK(!dm_android_cloud_flush_wait(3));CHECK(atomic_load(&failed_fsync)>=failures+2);CHECK(exists(g_active));check_ack(ackcopy);
    atomic_store(&fail_fsync,0);await_idle();pack_content(pack,"save-five");

    remember_ack(ackcopy);file(d2s,"save-held");atomic_store(&a.hold,1);dm_android_cloud_commit(save);dm_android_cloud_flush(1);wait_held(&a);
    cancels=atomic_load(&a.cancels);dm_android_cloud_shutdown();CHECK(atomic_load(&a.cancels)==cancels+1);CHECK(exists(g_active));check_ack(ackcopy);
    atomic_store(&a.hold,0);CHECK(!dm_android_cloud_boot(save));pack_content(pack,"save-held");

    CHECK(!unlink(d2s)&&!rmdir(character));atomic_store(&a.publish_error,1);dm_android_cloud_commit(save);CHECK(!dm_android_cloud_flush_wait(3));CHECK(exists(g_active));
    dm_android_cloud_shutdown();CHECK(exists(g_active));unsigned tombs=atomic_load(&a.tombs);atomic_store(&a.publish_error,0);
    CHECK(!dm_android_cloud_boot(save));CHECK(!exists(d2s)&&!exists(g_active)&&!exists(g_pending));CHECK(atomic_load(&a.tombs)==tombs+2);CHECK(is_tomb(pack));

    CHECK(!mkdir(character,0700));file(d2s,"save-six");atomic_store(&a.publish_error,1);dm_android_cloud_commit(save);CHECK(!dm_android_cloud_flush_wait(3));dm_android_cloud_shutdown();
    char active_a[1400],ack_a[1400];strcpy(active_a,g_active);strcpy(ack_a,g_ack);CHECK(exists(active_a));
    CHECK(!dm_android_cloud_set_provider(provider_b,cache,&mock_ops,&b));CHECK(!dm_android_cloud_boot(save));CHECK(atomic_load(&b.refreshes)==1);CHECK(exists(active_a));CHECK(strcmp(g_ack,ack_a));
    char pack_b[1400];path(pack_b,b.remote,"CloudTest.pack");pack_content(pack_b,"save-six");dm_android_cloud_shutdown();
    atomic_store(&a.publish_error,0);CHECK(!dm_android_cloud_set_provider(provider_a,cache2,&mock_ops,&a));CHECK(!dm_android_cloud_boot(save));
    CHECK(!strcmp(g_ack,ack_a)&&!exists(active_a));CHECK(!strncmp(a.cache,cache2,strlen(cache2)));pack_content(pack,"save-six");

    same_stamp_remote(root,save,d2s,&a);
    /* Concurrent immutable heads are imported only before mounting the save.
     * The seen journal belongs to the acknowledged state generation. */
    char stashpack[1400];path(stashpack,a.remote,"_stash.pack");conflict_pack(a.remote,"CloudTest",'c',pack);conflict_pack(a.remote,"_stash",'d',stashpack);
    char seen[1400];path(seen,g_ack,"conflicts.seen");CHECK(!exists(seen));unsigned backups=entries(g_bak);
    dm_android_cloud_commit(save);await_idle();CHECK(!exists(seen));CHECK(entries(g_bak)==backups);count_inbox(save,0);
    dm_android_cloud_shutdown();CHECK(!dm_android_cloud_boot(save));CHECK(exists(seen));CHECK(entries(g_bak)==backups+1);count_inbox(save,1);
    char seen_expected[144];snprintf(seen_expected,sizeof seen_expected,"CloudTest/%s\n", "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    CHECK(conflict_seen(seen,seen_expected)==1);snprintf(seen_expected,sizeof seen_expected,"_stash/%s\n","dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");CHECK(conflict_seen(seen,seen_expected)==1);
    backups=entries(g_bak);dm_android_cloud_shutdown();CHECK(!dm_android_cloud_boot(save));CHECK(entries(g_bak)==backups);count_inbox(save,1);
    remember_ack(ackcopy);conflict_pack(a.remote,"CloudTest",'e',pack);atomic_store(&a.publish_error,1);dm_android_cloud_shutdown();
    CHECK(dm_android_cloud_boot(save)==EIO);check_ack(ackcopy);dm_android_cloud_shutdown();atomic_store(&a.publish_error,0);
    CHECK(!dm_android_cloud_boot(save));snprintf(seen_expected,sizeof seen_expected,"CloudTest/%s\n","eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");CHECK(conflict_seen(seen,seen_expected)==1);count_inbox(save,1);

    dm_android_cloud_shutdown();CHECK(!g_dm_save_commit_hook);CHECK(!fixture_remove(root));
    printf("PASS cloud provider transaction checks=%u (mock provider; real portable core/FS; no remote-server E2E)\n",atomic_load(&checks));return 0;
}
