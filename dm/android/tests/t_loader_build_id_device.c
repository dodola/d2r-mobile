/* Standalone Android loader metadata regression; no guest execution or saves.
 * Build with function/data sections and --gc-sections to isolate read_layout. */
#include "../../host/dm.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#undef die
#define die(...) _exit(42)
#include "../../host/loader.c"

static void check(const char *dir, const char *header, const char *expected, int rejected) {
    char path[1024];
    snprintf(path,sizeof path,"%s/main.fixups.tsv",dir);
    FILE *f=fopen(path,"w"); assert(f);
    fputs(header,f);
    fputs("# ANDROID_APP_SLOT\t0x2\n# PATCH_OFF\t0x10000\n"
          "# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n"
          "# TEXT_END\t0x4000\n# RO_OFF\t0x4000\n# DATA_OFF\t0x8000\n"
          "# KEY_K18_IDX\t0\n# KEY_KTLS_IDX\t1\n# KEY_KTLR_IDX\t2\n"
          "# KEY_KGATE_IDX\t3\n# KEY_KRES0_IDX\t4\n# KEY_KRES1_IDX\t5\n"
          "# KEY_KASPECT_IDX\t6\n# KEY_KASPECTUI_IDX\t7\n",f);
    assert(!fclose(f));
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        module_t m={0};
        strcpy(m.name,"main"); strcpy(m.build_id,"stale");
        read_layout(&m,dir);
        _exit(!strcmp(m.build_id,expected) ? 0 : 1);
    }
    int status=0; assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==(rejected?42:0));
}

int main(void) {
    char dir[]="/data/local/tmp/dm-build-id-XXXXXX",path[1024];
    assert(mkdtemp(dir));
    check(dir,"# BUILD_ID\tdb0507f36b84200ac05f75d76ed5026f\n","db0507f36b84200ac05f75d76ed5026f",0);
    check(dir,"# BUILD_ID\t2c7f979610e9911979982dc07bfc5702\r\n","2c7f979610e9911979982dc07bfc5702",0);
    check(dir,"","",0); /* Legacy metadata cannot retain a prior module ID. */
    check(dir,"# BUILD_ID\tdb0507f36b84200ac05f75d76ed5026\n","",1);
    check(dir,"# BUILD_ID\tdb0507f36b84200ac05f75d76ed5026ff\n","",1);
    check(dir,"# BUILD_ID\tdb0507f36b84200ac05f75d76ed5026g\n","",1);
    check(dir,"# BUILD_ID\tDB0507F36B84200AC05F75D76ED5026F\n","",1);
    check(dir,"# BUILD_ID\tdb0507f36b84200ac05f75d76ed5026f extra\n","",1);
    check(dir,"# BUILD_ID\tdb0507f36b84200ac05f75d76ed5026f\n# BUILD_ID\tdb0507f36b84200ac05f75d76ed5026f\n","",1);
    snprintf(path,sizeof path,"%s/main.fixups.tsv",dir);
    assert(!unlink(path)); assert(!rmdir(dir));
    puts("ANDROID_LOADER_BUILD_ID_OK cases=9 exact/legacy/CRLF/malformed/duplicate");
    return 0;
}
