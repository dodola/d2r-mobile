/* 管线守卫跨进程重启、完整键区分、驱动隔离、瞬时错误与恢复测试，无需 GPU。
 * Pipeline guard tests across process restarts, distinct complete keys, drivers, transient errors and recovery; no GPU required. */
#include <assert.h>
#include <sys/wait.h>
#include <pthread.h>
#include "../nvn/nvn_vk_pipeline_store.h"
typedef struct {struct {uint32_t compute,state;} key;char shader[2][17];} StablePso;
static int pipeline_stable_valid(const StablePso *s){return s->shader[0][0]=='a';}
#include "../nvn/nvn_vk_pipe_guard.h"
static StablePso key={.key={0,42},.shader={"aaaaaaaaaaaaaaaa","bbbbbbbbbbbbbbbb"}};
static void child(const char *exe,const char *dir,const char *mode) {
    pid_t pid=fork();assert(pid>=0);if(!pid){execl(exe,exe,dir,mode,NULL);_exit(99);}
    int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));
}
int main(int argc,char **argv) {
    if(argc==1){
        char dir[]="/tmp/dm-pipe-guard-XXXXXX";assert(mkdtemp(dir));
        child(argv[0],dir,"crash");child(argv[0],dir,"once");child(argv[0],dir,"blocked");
        child(argv[0],dir,"other-driver");child(argv[0],dir,"recover");child(argv[0],dir,"clean");
        child(argv[0],dir,"transient");child(argv[0],dir,"clean");
        child(argv[0],dir,"invalid");child(argv[0],dir,"reset");
        child(argv[0],dir,"crash");child(argv[0],dir,"gap");child(argv[0],dir,"clean");
        child(argv[0],dir,"crash");child(argv[0],dir,"once");child(argv[0],dir,"blocked");
        printf("PIPE_GUARD PASS restart_threshold full_key identity transient recovery reset\n");return 0;
    }
    DmPipelineIdentity id={.vendor=0x5143,.driver=!strcmp(argv[2],"other-driver")?2:1,.options=17};
    if(!strcmp(argv[2],"reset")){char p[1400];snprintf(p,sizeof p,"%s/gfx-pipe-reset",argv[1]);FILE *f=fopen(p,"wb");assert(f);fclose(f);}
    pipe_guard_init(argv[1],&id);assert(PG.enabled);
    if(!strcmp(argv[2],"crash")){assert(!PG.count);pipe_guard_begin(&key);_exit(0);}
    if(!strcmp(argv[2],"gap")){assert(PG.count==1&&!pipe_guard_quarantined(&key));}
    if(!strcmp(argv[2],"once")){assert(PG.count==1&&PG.failures[0].crashes==1&&!pipe_guard_quarantined(&key));pipe_guard_begin(&key);_exit(0);}
    if(!strcmp(argv[2],"blocked")){assert(pipe_guard_quarantined(&key)&&!PG.degrade);StablePso other=key;other.key.state++;assert(!pipe_guard_quarantined(&other));}
    if(!strcmp(argv[2],"recover")){assert(pipe_guard_quarantined(&key));pipe_guard_end(&key,0);assert(!pipe_guard_quarantined(&key));}
    if(!strcmp(argv[2],"transient")){for(int i=1;i<=4;i++){pipe_guard_begin(&key);pipe_guard_end(&key,-i);assert(!pipe_guard_quarantined(&key));}}
    if(!strcmp(argv[2],"invalid")){pipe_guard_begin(&key);pipe_guard_end(&key,-1000012000);assert(pipe_guard_quarantined(&key));}
    if(!strcmp(argv[2],"clean")||!strcmp(argv[2],"other-driver")||!strcmp(argv[2],"reset"))assert(!PG.count&&!pipe_guard_quarantined(&key));
    return 0;
}
