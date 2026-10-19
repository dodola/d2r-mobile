/* Runs the production JNI focus/worker code with a synthetic save adapter.
 * No Activity, game assets, original guest callee or app save is touched. */
#define dm_host_shutdown fixture_host_shutdown
#include "../app_jni.c"
#undef dm_host_shutdown

static unsigned checks;
static _Atomic unsigned began, ended;
static _Atomic int mock_in_game=1, mock_frozen;
static _Atomic int mock_shutdown_called;
static unsigned background_roundtrips;
static double maximum_callback_ms;
#define CHECK(e) do { ++checks; if (!(e)) die("Lifecycle fixture failed line=%d",__LINE__); } while(0)
void dm_ui_lifecycle(int background) { (void)background; }
void fixture_host_shutdown(const char *why) { (void)why; atomic_store(&mock_shutdown_called,1); }
static int mock_game(void *opaque) { (void)opaque; return atomic_load(&mock_in_game); }
static int mock_request(void *opaque) { (void)opaque; return atomic_load(&mock_in_game); }
static int mock_begin(void *opaque) {
    (void)opaque; atomic_fetch_add(&began,1); atomic_store(&mock_frozen,1); return 1;
}
static void mock_end(void *opaque) {
    (void)opaque; atomic_fetch_add(&ended,1); atomic_store(&mock_frozen,0);
}
static void event(int value) {
    if (value==1) ++background_roundtrips;
    uint64_t start=now_ns();
    CHECK(!Java_org_d2r_binaryprobe_GameActivity_nativeLifecycle(NULL,NULL,value));
    double elapsed=(now_ns()-start)/1000000.0;
    if (elapsed>maximum_callback_ms) maximum_callback_ms=elapsed;
    CHECK(elapsed<100); /* Cannot contain the 2.5-second commit wait. */
}
static void await_begin(unsigned before) {
    uint64_t deadline=now_ns()+1000000000;
    while (now_ns()<deadline && atomic_load(&began)<=before) usleep(1000);
    CHECK(atomic_load(&began)>before && atomic_load(&mock_frozen));
}
static uint64_t epoch_now(void) {
    pthread_mutex_lock(&state_lock); uint64_t epoch=lifecycle_epoch; pthread_mutex_unlock(&state_lock); return epoch;
}
static void await_done(uint64_t epoch) {
    uint64_t deadline=now_ns()+1000000000;
    for (;;) {
        pthread_mutex_lock(&state_lock); int done=background_done_epoch>=epoch; pthread_mutex_unlock(&state_lock);
        if (done) break;
        CHECK(now_ns()<deadline); usleep(1000);
    }
}
static void clock_advances(void) {
    uint64_t before=guest_ns(); usleep(15000); CHECK(guest_ns()>before+5000000);
}
int main(int argc,char **argv) {
    if (argc!=3 || argv[1][0]!='/') return 2;
    g_t0_ns=now_ns();
    CHECK(snprintf(port_root,sizeof port_root,"%s/lifecycle-%d",argv[1],getpid())<(int)sizeof port_root);
    CHECK(!dm_android_fs_ensure_directory(port_root));
    char save[PATH_MAX]; path(save,sizeof save,port_root,"save");
    CHECK(!dm_android_fs_ensure_directory(save)); CHECK(!setenv("DM_SAVE",save,1));
    dm_android_save_adapter adapter={.title="lifecycle-fixture",.is_in_game=mock_game,
        .request_save=mock_request,.begin_save_wait=mock_begin,.end_save_wait=mock_end};
    CHECK(!dm_android_save_bind(&adapter)); atomic_store(&started,1);
    event(0); unsigned before=atomic_load(&began); event(1); uint64_t old=epoch_now(); await_begin(before);
    event(2); CHECK(!atomic_load(&mock_frozen)); await_done(old); clock_advances();

    /* Reusing Background=3 must not let an older cancelled job freeze the
     * newer foreground/background cycle while its own save still waits. */
    for (unsigned round=0;round<8;++round) {
        before=atomic_load(&began); event(1); old=epoch_now(); await_begin(before);
        event(2); before=atomic_load(&began); event(1); uint64_t current=epoch_now();
        CHECK(current>old); await_begin(before); clock_advances();
        pthread_mutex_lock(&state_lock); CHECK(background_done_epoch<current); pthread_mutex_unlock(&state_lock);
        event(2); CHECK(!atomic_load(&mock_frozen)); await_done(current); clock_advances();
    }
    uint64_t stale=dm_android_save_epoch(); dm_android_save_cancel();
    before=atomic_load(&began); uint64_t commits=99; int froze=99; double elapsed=99;
    uint64_t start=now_ns();
    CHECK(!dm_android_save_wait_current(stale,2.5,&commits,&froze,&elapsed));
    CHECK(!commits && !froze && !elapsed && atomic_load(&began)==before && now_ns()-start<100000000);

    /* Outside a run background finishes and freezes the clock; foreground
     * releases it before a delayed Surface job can activate the window. */
    atomic_store(&mock_in_game,0); event(1); old=epoch_now(); await_done(old);
    uint64_t frozen_time=guest_ns(); usleep(15000); CHECK(guest_ns()==frozen_time);
    event(2); clock_advances();
    pthread_mutex_lock(&focus_lock);
    CHECK(surface_focus_locked(1)==1 && atomic_load(&lifecycle_focus)==1);
    pthread_mutex_unlock(&focus_lock);
    event(1);
    pthread_mutex_lock(&focus_lock);
    CHECK(surface_focus_locked(1)==3 && atomic_load(&lifecycle_focus)==3);
    pthread_mutex_unlock(&focus_lock);
    old=epoch_now(); await_done(old); event(2); clock_advances();

    /* Capture the terminal shutdown call instead of exiting the fixture.
     * onDestroy rolls back an in-flight save then runs a final bounded save
     * on the worker, while its JNI/UI callback returns immediately. */
    atomic_store(&mock_in_game,1); before=atomic_load(&began); event(1); await_begin(before);
    before=atomic_load(&began); start=now_ns();
    Java_org_d2r_binaryprobe_GameActivity_nativeShutdown(NULL,NULL);
    double shutdown_ms=(now_ns()-start)/1000000.0;
    CHECK(shutdown_ms<100 && shutdown_requested); await_begin(before);
    uint64_t deadline=now_ns()+4000000000ull;
    while (!atomic_load(&mock_shutdown_called) && now_ns()<deadline) usleep(1000);
    CHECK(atomic_load(&mock_shutdown_called) && !atomic_load(&mock_frozen));
    Java_org_d2r_binaryprobe_GameActivity_nativeShutdown(NULL,NULL); /* idempotent */
    FILE *out=fopen(argv[2],"w"); CHECK(out);
    fprintf(out,"{\"passed\":true,\"checks\":%u,\"fixture_only\":true,\"production_focus_worker\":true,\"max_callback_ms\":%.3f,\"shutdown_callback_ms\":%.3f,\"background_roundtrips\":%u,\"cancelled_queue_rejected\":true,\"clock_aba_checked\":true,\"surface_intent_checked\":true,\"final_destroy_save_requested\":true,\"original_game_save_executed\":false,\"app_save_data_used\":false}\n",checks,maximum_callback_ms,shutdown_ms,background_roundtrips);
    CHECK(!fclose(out)); return 0;
}
