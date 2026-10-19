/* Android nanosecond-mtime and sidecar commit behavior. All files belong to
 * a fresh fixture-only directory; no player save or game process is used. */
#include "../../host/dm_rift.c"
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

void (*g_dm_pre_commit_hook[5])(const char *);
static uint64_t commits;
static char workdir[256];
uint64_t dm_fs_commit_count(void) {return commits;}
const char *dm_save_workdir(void) {return workdir;}
static void write_text(const char *path,const char *text) {FILE *file=fopen(path,"w");assert(file);assert(fputs(text,file)>=0 && !fclose(file));}
static void set_time(const char *path,long ns) {struct timespec times[2]={{123456789,ns},{123456789,ns}};assert(!utimensat(AT_FDCWD,path,times,0));}
static int tier(const char *path) {FILE *file=fopen(path,"r");assert(file);int value=0;assert(fscanf(file,"tier %d",&value)==1);fclose(file);return value;}
int main(void) {
    char root[]="/data/local/tmp/d2r-rift-commit-XXXXXX";assert(mkdtemp(root));
    char committed[256],savedir[300],workchar[300],original[340],working[340],sidecar[340],temporary[350];
    snprintf(committed,sizeof committed,"%s/save",root);snprintf(workdir,sizeof workdir,"%s/save.work",root);
    snprintf(savedir,sizeof savedir,"%s/Fixture",committed);snprintf(workchar,sizeof workchar,"%s/Fixture",workdir);
    assert(!mkdir(committed,0700) && !mkdir(workdir,0700) && !mkdir(savedir,0700) && !mkdir(workchar,0700));
    snprintf(original,sizeof original,"%s/Fixture.d2s",savedir);snprintf(working,sizeof working,"%s/Fixture.d2s",workchar);
    snprintf(sidecar,sizeof sidecar,"%s/Fixture.sw_rift",workchar);snprintf(temporary,sizeof temporary,"%s.tmp",sidecar);
    write_text(original,"save");write_text(working,"save");set_time(original,10);set_time(working,10);
    strcpy(K.who,"Fixture");R.tier=9;R.best=8;R.fast[8]=55;rec_save();
    assert(g_dm_pre_commit_hook[3]==rec_pre_commit);rec_pre_commit(workdir);
    assert(RS.dirty && !RS.wrote && access(sidecar,F_OK)); /* Unchanged clone must wait. */
    set_time(working,11);rec_pre_commit(workdir); /* One nanosecond is a real rewrite. */
    assert(!RS.dirty && RS.wrote==1 && tier(sidecar)==9);
    assert(!unlink(sidecar));rec_pre_commit(workdir); /* Refused commit, work clone lost sidecar. */
    assert(!RS.dirty && RS.wrote==1 && tier(sidecar)==9);
    commits=1;set_time(original,11);rec_pre_commit(workdir);assert(!RS.dirty && !RS.wrote);
    R.tier=12;rec_save();R.tier=99;rec_load();assert(R.tier==12); /* Pending snapshot wins on re-entry. */
    rec_pre_commit(workdir);assert(RS.dirty && !RS.wrote && tier(sidecar)==9);
    write_text(working,"save-new-size");set_time(working,11);rec_pre_commit(workdir);
    assert(!RS.dirty && RS.wrote==2 && tier(sidecar)==12); /* Same mtime, changed length. */
    commits=2;write_text(original,"save-new-size");set_time(original,11);rec_pre_commit(workdir);
    R.tier=15;rec_save();set_time(working,12);assert(!mkdir(temporary,0700));
    rec_pre_commit(workdir);assert(RS.dirty && !RS.wrote && tier(sidecar)==12); /* Failed temp write retries. */
    assert(!rmdir(temporary));rec_pre_commit(workdir);assert(!RS.dirty && RS.wrote==3 && tier(sidecar)==15);
    commits=3;rec_pre_commit(workdir);assert(!RS.wrote);rec_load();assert(R.tier==15 && R.best==8 && R.fast[8]==55);
    assert(!unlink(original));R.tier=18;rec_save();rec_pre_commit(workdir);
    assert(!RS.dirty && RS.wrote==4 && tier(sidecar)==18); /* First save of a new character. */
    assert(!unlink(working) && !unlink(sidecar));
    assert(!rmdir(savedir) && !rmdir(workchar) && !rmdir(committed) && !rmdir(workdir) && !rmdir(root));
    puts("ANDROID_D2R_RIFT_COMMIT_OK unchanged_clone=1 nanosecond_rewrite=1 size_rewrite=1 rejected_commit_retry=1 write_failure_retry=1 pending_snapshot=1 roundtrip=1 new_character=1");
    return 0;
}
