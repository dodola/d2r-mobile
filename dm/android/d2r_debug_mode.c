/* Opt-in Android QA console. Original call contracts come from host/diag.c:
 * DM_TEST_GIVE / DM_TEST_GOTO_LEVEL. No guest call runs on an I/O/UI thread. */
#include "d2r_debug_mode.h"
#include "d2r_save_adapter.h"
#include "d2r_host_modules.h"
#include "services_platform.h"
#include "../host/dm_hud.h"
#include "../host/dm_rift.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define DEBUG_LINE 256
#define D2R_BUILD_ID "db0507f36b84200ac05f75d76ed5026f"
static struct {
    module_t *image;
    int enabled, attached, ready;
    char directory[PATH_MAX], inbox[PATH_MAX], claimed[PATH_MAX], status[PATH_MAX];
    char request[41], outcome[32], detail[160];
    uint64_t game, runs, world, token, next_poll, deadline;
    uint32_t player_id, target, area, tick, made, picked;
} dbg;

static int path_join(char *out,size_t cap,const char *root,const char *leaf) {
    int n=snprintf(out,cap,"%s/%s",root,leaf);return n>0 && (size_t)n<cap;
}
/* Called once, before mounts / observers / worker threads start. */
int dm_android_d2r_debug_configure(const char *root,char *save,size_t capacity) {
    char marker[PATH_MAX];
    if (!root || !save || !path_join(dbg.directory,sizeof dbg.directory,root,"debug") ||
        !path_join(marker,sizeof marker,dbg.directory,"enable")) return EINVAL;
    const char *env=getenv("DM_DEBUG_MODE");
    dbg.enabled=(env && !strcmp(env,"1")) || access(marker,F_OK)==0;
    if (!dbg.enabled) return 0;
    if (!path_join(save,capacity,dbg.directory,"save") ||
        !path_join(dbg.inbox,sizeof dbg.inbox,dbg.directory,"commands.txt") ||
        !path_join(dbg.claimed,sizeof dbg.claimed,dbg.directory,"processing.txt") ||
        !path_join(dbg.status,sizeof dbg.status,dbg.directory,"status.json")) return ENAMETOOLONG;
    if (mkdir(dbg.directory,0700) && errno!=EEXIST) return errno;
    dbg.token=now_ns();
    return 0;
}
static int read_mem(uint64_t address,void *out,size_t bytes) {
    if (!address || address>UINT64_MAX-bytes) return 0;
    struct iovec local={out,bytes}, remote={(void *)(uintptr_t)address,bytes};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)bytes;
}
static int field(uint64_t object,size_t off,void *out,size_t bytes) {
    return object && object<=UINT64_MAX-off && read_mem(object+off,out,bytes);
}
static int write_mem(uint64_t address,const void *value,size_t bytes) {
    if (!address || address>UINT64_MAX-bytes) return 0;
    dm_guest_write_prepare((void *)(uintptr_t)address,bytes);
    struct iovec local={(void *)value,bytes}, remote={(void *)(uintptr_t)address,bytes};
    return process_vm_writev(getpid(),&local,1,&remote,1,0)==(ssize_t)bytes;
}
static void result(const char *state,const char *detail) {
    snprintf(dbg.outcome,sizeof dbg.outcome,"%s",state);
    snprintf(dbg.detail,sizeof dbg.detail,"%s",detail);
    LOG("ANDROID_DEBUG request=%s state=%s detail=%s",dbg.request,state,detail);
}
/* Strings written here are internal constants or validated ASCII request IDs. */
static int receipt(void) {
    char temp[PATH_MAX];
    char save_label[128]="debug/save";const char *mod=getenv("DM_ACTIVE_MOD");
    if(mod && *mod && strlen(mod)<=48 && strspn(mod,"abcdefghijklmnopqrstuvwxyz0123456789_-")==strlen(mod))
        snprintf(save_label,sizeof save_label,"mods/%s/debug-save",mod);
    if (!dbg.status[0] || !path_join(temp,sizeof temp,dbg.directory,"status.tmp")) return 0;
    FILE *f=fopen(temp,"w");if(!f)return 0;
    int ok=fprintf(f,"{\"enabled\":true,\"attached\":%s,\"ready\":%s,\"run\":\"%llx\","
        "\"tick\":%u,\"area\":%u,\"request\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\","
        "\"made\":%u,\"picked\":%u,\"target\":%u,\"save\":\"%s\"}\n",
        dbg.attached?"true":"false",dbg.ready?"true":"false",(unsigned long long)dbg.token,
        dbg.tick,dbg.area,dbg.request,dbg.outcome,dbg.detail,dbg.made,dbg.picked,dbg.target,save_label)>0;
    if (fflush(f) || fsync(fileno(f))) ok=0;
    if (fclose(f)) ok=0;
    return ok && !rename(temp,dbg.status);
}
int dm_android_d2r_debug_attach(module_t *main) {
    if (!dbg.enabled)return 0;
    if (!main || strcmp(main->name,"main") || !main->base || main->size<0x3211700 ||
        strcmp(main->build_id,D2R_BUILD_ID) || !gate_obs_present(OBS_TICK)) {
        result("unsupported","requires reviewed D2R 94012 and server tick gate");receipt();return 0;
    }
    dbg.image=main;dbg.attached=1;
    result("idle","waiting for a live test character");
    /* A crash may have happened before or after a guest mutation. Never replay. */
    if (!access(dbg.claimed,F_OK)) {
        result("interrupted","previous request outcome unknown; inspect before resubmitting");
        if (unlink(dbg.claimed)) {dbg.attached=0;result("io_error","cannot retire interrupted request");}
    }
    receipt();return dbg.attached;
}
static int number(const char *s,uint32_t limit,uint32_t *out) {
    if (!s || !*s)return 0;
    uint32_t n=0;
    for (;*s;s++) {if(*s<'0'||*s>'9'||n>limit/10)return 0;n=n*10+(unsigned)(*s-'0');if(n>limit)return 0;}
    *out=n;return 1;
}
static int request_id(const char *s) {
    if(!s || !*s || strlen(s)>40)return 0;
    for(;*s;s++)if(!isalnum((unsigned char)*s) && *s!='-' && *s!='_')return 0;
    return 1;
}
static int token_value(const char *s,uint64_t *out) {
    if(!s || !*s || strlen(s)>16)return 0;
    uint64_t n=0;
    for(;*s;s++){unsigned c=(unsigned char)*s;unsigned v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;if(v>15)return 0;n=n*16+v;}
    *out=n;return 1;
}
static int level_id(const char *name,uint32_t *out) {
    static const struct {const char *name;uint32_t id;} names[]={
        {"rogue",1},{"camp",1},{"blood_moor",2},{"lut",40},{"kurast",75},
        {"fortress",103},{"harrogath",109},{"tristram",38},{"canyon",46},
        {"throne",131},{"worldstone",132}};
    for(unsigned i=0;i<sizeof names/sizeof *names;i++)if(!strcmp(name,names[i].name)){*out=names[i].id;return 1;}
    /* Reviewed base-game areas only; reject unused/mod-specific IDs. */
    return number(name,136,out) && *out>0;
}
static uint32_t player_area(uint64_t player) {
    uint64_t path=0,room=0,extra=0,level=0;uint32_t id=0;
    if(field(player,0x38,&path,8) && field(path,0x20,&room,8) &&
       field(room,0x18,&extra,8) && field(extra,0x90,&level,8))field(level,0x1f8,&id,4);
    return id;
}
static int item_table(uint64_t player,uint64_t *rows,uint32_t *count) {
    uint8_t version=0;uint64_t table=0;
    return field(player,0x1bd,&version,1) && version>=1 && version<=3 &&
        field((uintptr_t)dbg.image->base,0x2d64a18+version*16,&table,8) &&
        field(table,0x15c0,rows,8) && *rows && field(table,0x15c8,count,4) && *count && *count<=16384;
}
static int valid_item(uint64_t player,const char *text,uint32_t *code) {
    size_t n=strlen(text);if(!n||n>4)return 0;
    for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)text[i]))return 0;
    *code=0x20202020u;memcpy(code,text,n);
    uint64_t rows;uint32_t count,value;
    if(!item_table(player,&rows,&count))return 0;
    for(uint32_t i=0;i<count;i++) {
        if(!field(rows,(size_t)i*0x1c0+0x80,&value,4))return 0;
        if(value==*code)return 1;
    }
    return 0;
}
/* Test harness substitutes only the original binary functions, not the parser,
 * filesystem transport, context guards or receipt state machine. */
#ifndef DM_DEBUG_TEST
static uint64_t drop(uint64_t g,uint64_t p,uint32_t c,int q) {
    return ((uint64_t (*)(uint64_t,uint64_t,uint32_t,int))(dbg.image->base+0x7c0b20))(g,p,c,q);
}
static void dispatch(uint64_t g,uint64_t p,uint8_t *packet) {
    ((void (*)(uint64_t,uint64_t,uint8_t *,uint32_t))(dbg.image->base+0x6e5b40))(g,p,packet,17);
}
static void warp(uint64_t g,uint64_t p,int level) {
    ((void (*)(uint64_t,uint64_t,int,int))(dbg.image->base+0x634000))(g,p,level,11);
}
static int get_stat(uint64_t p,int stat) {
    return ((int (*)(uint64_t,int,int))(dbg.image->base+0x5b9290))(p,stat,0);
}
static void set_stat(uint64_t p,int stat,int value) {
    ((void (*)(uint64_t,int,int,int))(dbg.image->base+0x5bc1f0))(p,stat,value,0);
}
#else
static uint64_t drop(uint64_t,uint64_t,uint32_t,int);
static void dispatch(uint64_t,uint64_t,uint8_t *);
static void warp(uint64_t,uint64_t,int);
static int get_stat(uint64_t,int);
static void set_stat(uint64_t,int,int);
#endif
static int pickup(uint64_t game,uint64_t player,uint64_t item) {
    uint64_t path=0;uint32_t id=0,x=0,y=0,previous=0,one=1,mode=3;
    uint8_t packet[17]={0x16};
    if(!field(item,8,&id,4)||!field(item,0x38,&path,8)||!field(path,0x10,&x,4)||
       !field(path,0x14,&y,4)||!field(game,0x19050,&previous,4)||previous)return 0;
    memcpy(packet+1,&id,4);memcpy(packet+5,&x,4);memcpy(packet+9,&y,4);memcpy(packet+13,&one,4);
    if(!write_mem(game+0x19050,&one,4))return 0;
    dispatch(game,player,packet);
    if(!write_mem(game+0x19050,&previous,4))die("debug pickup failed to restore processing flag");
    /* Pickup may consume/merge a stack. Do not follow the old pointer then. */
    uint64_t current=dm_diag_srv_item(id);
    return current==item && field(current,0xc,&mode,4) && mode!=3;
}
static void execute(uint64_t game,uint64_t player,char *text) {
    char *argv[9],*save=NULL;int argc=0;
    for(char *t=strtok_r(text," \t\r\n",&save);t;t=strtok_r(NULL," \t\r\n",&save)){
        if(argc==9){result("rejected","too many arguments");return;}argv[argc++]=t;
    }
    uint64_t token=0;
    if(argc<3 || !request_id(argv[0]) || !token_value(argv[1],&token)){
        result("rejected","syntax: request-id run-token command arguments");return;
    }
    if(!strcmp(dbg.request,argv[0]))return; /* Immediate retry is not a second mutation. */
    snprintf(dbg.request,sizeof dbg.request,"%s",argv[0]);dbg.made=dbg.picked=0;
    if(token!=dbg.token || !dbg.ready){result("rejected","stale run or game not ready");return;}
    char *cmd=argv[2];
    if(!strcmp(cmd,"status") && argc==3){result("ok","status sampled");return;}
    if(!strcmp(cmd,"give")) {
        uint32_t code=0,q=2,n=1;int inv=0,end=argc;
        if(end>4 && (!strcmp(argv[end-1],"inventory") || !strcmp(argv[end-1],"inv"))){inv=1;end--;}
        if(end<4 || end>6 || (end>4 && !number(argv[4],7,&q)) ||
           (q!=0 && q!=2 && q!=4 && q!=5 && q!=6 && q!=7) ||
           (end>5 && !number(argv[5],16,&n)) || !n || !valid_item(player,argv[3],&code)){
            result("rejected","give: valid code, quality 0/2/4/5/6/7, count 1..16, optional inventory");return;
        }
        result("executing","item generation started; crash outcome may be partial");
        if(!receipt()){result("io_error","receipt failed before item generation");return;}
        for(uint32_t i=0;i<n;i++){uint64_t u=drop(game,player,code,(int)q);if(u){dbg.made++;if(inv && pickup(game,player,u))dbg.picked++;}}
        result(dbg.made==n && (!inv || dbg.picked==n)?"ok":dbg.made?"partial":"failed",
            "made is spawn count; picked is observed item relocation; inspect actual quality in game");return;
    }
    if(!strcmp(cmd,"scene") || !strcmp(cmd,"warp")) {
        uint32_t level=0;uint8_t version=0;
        if(argc!=4 || !level_id(argv[3],&level) || !field(player,0x1bd,&version,1) ||
           version<1 || version>3 || (version==1 && level>108)){
            result("rejected","scene: base-game level 1..136 or documented alias; expansion required for Act V");return;
        }
        if(level==dbg.area){result("ok","already in requested area");return;}
        dbg.target=level;dbg.deadline=now_ns()+30000000000ull;dbg.ready=0;
        result("transition","waiting for observed target area and closed loading screen");
        if(!receipt()){dbg.target=0;result("io_error","receipt failed before warp");return;}
        warp(game,player,(int)level);return; /* No further guest pointer reads this tick. */
    }
    if(!strcmp(cmd,"heal") && argc==3){
        int hp=get_stat(player,7),mp=get_stat(player,9);
        if(hp<=0||mp<0){result("rejected","invalid maximum life or mana");return;}
        result("executing","restoring life and mana");
        if(!receipt()){result("io_error","receipt failed before healing");return;}
        set_stat(player,6,hp);set_stat(player,8,mp);
        result(get_stat(player,6)==hp && get_stat(player,8)==mp?"ok":"failed","server values checked; UI update requires device validation");return;
    }
    if(!strcmp(cmd,"gold")){
        uint32_t amount=0;int level=get_stat(player,12);
        if(argc!=4 || level<1 || level>99 || !number(argv[3],(uint32_t)level*10000,&amount)){
            result("rejected","gold must be decimal 0..character-level*10000");return;
        }
        result("executing","setting carried gold");
        if(!receipt()){result("io_error","receipt failed before setting gold");return;}
        set_stat(player,14,(int)amount);
        result(get_stat(player,14)==(int)amount?"ok":"failed","carried gold read back on server");return;
    }
    result("rejected","unknown command or invalid argument count");
}
static void poll(uint64_t game,uint64_t player) {
    /* Claim the inode before reading. New submissions cannot be truncated by
     * the consumer. Single producer must await the matching request receipt. */
    if(rename(dbg.inbox,dbg.claimed))return;
    int fd=open(dbg.claimed,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);struct stat st;
    char line[DEBUG_LINE];ssize_t n=-1;
    if(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<(off_t)sizeof line){
        n=read(fd,line,sizeof line);if(n!=st.st_size)n=-1;
    }
    if(fd>=0)close(fd);
    if(unlink(dbg.claimed)){result("io_error","cannot retire claimed request; no action executed");return;}
    if(n<=0 || n>=(ssize_t)sizeof line || memchr(line,0,(size_t)n)){
        result("rejected","request must be one ASCII line shorter than 256 bytes");return;
    }
    while(n>0 && (line[n-1]=='\n'||line[n-1]=='\r'))n--;
    for(ssize_t i=0;i<n;i++)if((unsigned char)line[i]<32 && line[i]!='\t'){
        result("rejected","only one command per submission");return;
    }else if((unsigned char)line[i]>126){result("rejected","ASCII commands only");return;}
    line[n]=0;execute(game,player,line);
}
void dm_android_d2r_debug_tick(uint64_t game,uint64_t player,uint8_t *base,uint64_t runs) {
    if(!dbg.attached || !dbg.enabled || base!=dbg.image->base)return;
    uint64_t now=now_ns();if(now<dbg.next_poll)return;dbg.next_poll=now+200000000ull;
    dm_android_d2r_scene_status scene;dm_android_d2r_scene_status_get(&scene);
    dm_android_window_state window;dm_android_platform_get_window(&window);
    uint32_t id=0,mode=0;uint8_t paused=1,rift_pause=1;
    if(!game || !runs || !scene.game){game=runs=player=0;}
    field(player,8,&id,4);
    if(game!=dbg.game || runs!=dbg.runs || scene.world_epoch!=dbg.world || (player && id!=dbg.player_id)){
        dbg.game=game;dbg.runs=runs;dbg.player_id=id;dbg.world=scene.world_epoch;dbg.token++;
        if(dbg.target){dbg.target=0;result("cancelled","run changed during transition");}
    }
    dbg.tick=0;field(game,0x170,&dbg.tick,4);dbg.area=player_area(player);
    dbg.ready=game && player && scene.game && !scene.navigation && window.focus==1 &&
        dbg.area && field(player,0xc,&mode,4) && mode!=0 && mode!=17 && mode<=20 &&
        field((uintptr_t)base,0x2d5ff51,&paused,1) && !paused &&
        field((uintptr_t)base,0x2d5ff67,&rift_pause,1) && !rift_pause && !dm_hud_held() && !dm_rift_active();
    if(dbg.target){
        if(dbg.ready && dbg.area==dbg.target){dbg.target=0;result("ok","target area observed on server; loading screen closed");}
        else if(now>=dbg.deadline){dbg.target=0;result("timeout","target not observed within 30 seconds; no automatic retry");}
        else dbg.ready=0;
        receipt();return;
    }
    poll(game,player);receipt();
}
