/* Reuse dm_world.c's reviewed 94012 client-room initialization/reveal contract.
 * Do not run the client automap callback from the server tick or JNI thread. */
#include "d2r_map_reveal.h"
#include "d2r_host_modules.h"
#include "services_platform.h"
#include "../host/dm_hud.h"
#include "../host/dm_rift.h"
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

#define MAP_ROOMS 4096
static module_t *map_image;
static pthread_mutex_t map_lock=PTHREAD_MUTEX_INITIALIZER;
static dm_android_map_status map_status;
/* Private execution state is owned by the original client input thread. */
static struct {
    uint64_t sampled,world,player,level,callback,deadline;
    uint64_t rooms[MAP_ROOMS];
    uint32_t id,version,area;
} map_job;
static int map_read(uint64_t object,size_t offset,void *out,size_t size) {
    if(!object || object>UINT64_MAX-offset || object+offset>UINT64_MAX-size)return 0;
    struct iovec local={out,size},remote={(void *)(uintptr_t)(object+offset),size};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)size;
}
#ifndef DM_MAP_TEST
static uint64_t map_player(void) {
    uint32_t slot=0;if(!map_read((uintptr_t)map_image->base,0x2d20e4c,&slot,4) || slot>=8)return 0;
    return ((uint64_t (*)(uint32_t))(map_image->base+0x45f9d0))(slot);
}
static void map_init_room(int version,uint64_t room) {
    ((void (*)(int,uint64_t))(map_image->base+0x52dcc0))(version,room);
}
static void map_reveal_room(uint64_t callback,uint64_t room) {
    ((void (*)(uint64_t))(uintptr_t)callback)(room);
}
#else
static uint64_t map_player(void);
static void map_init_room(int,uint64_t);
static void map_reveal_room(uint64_t,uint64_t);
#endif
int dm_android_map_attach(module_t *main) {
    if(!main || !main->base || strcmp(main->name,"main") || main->size<0x3211700 ||
       main->text_end<=0x52dcc0 || main->text_end>main->size ||
       strcmp(main->build_id,"db0507f36b84200ac05f75d76ed5026f"))return 0;
    map_image=main;
    pthread_mutex_lock(&map_lock);map_status.state=DM_MAP_IDLE;pthread_mutex_unlock(&map_lock);
    return 1;
}
void dm_android_map_status_get(dm_android_map_status *out) {
    if(!out)return;
    pthread_mutex_lock(&map_lock);*out=map_status;pthread_mutex_unlock(&map_lock);
}
static int map_request(uint64_t *sequence,int defer) {
    pthread_mutex_lock(&map_lock);
    uint64_t now=now_ns();int error=0;
    if(map_status.state==DM_MAP_WAITING || map_status.state==DM_MAP_QUEUED || map_status.state==DM_MAP_RUNNING)error=EBUSY;
    else if(!(defer?(map_status.game&&map_job.area):map_status.ready) || now<map_job.sampled || now-map_job.sampled>500000000ull)error=EAGAIN;
    else {
        map_status.sequence++;map_status.state=defer?DM_MAP_WAITING:DM_MAP_QUEUED;
        map_status.total=map_status.done=0;map_status.error=0;map_status.ready=0;
        map_job.deadline=now+30000000000ull;
        if(sequence)*sequence=map_status.sequence;
    }
    pthread_mutex_unlock(&map_lock);return error;
}
int dm_android_map_request(uint64_t *sequence){return map_request(sequence,0);}
int dm_android_map_request_when_ready(uint64_t *sequence){return map_request(sequence,1);}
static uint64_t map_level(uint64_t player) {
    uint64_t path=0,room=0,extra=0,level=0;
    if(map_read(player,0x38,&path,8)&&map_read(path,0x20,&room,8)&&
       map_read(room,0x18,&extra,8))map_read(extra,0x90,&level,8);
    return level;
}
static int map_rooms(uint64_t level,uint32_t *total) {
    uint64_t room=0;*total=0;
    if(!map_read(level,0x10,&room,8))return 0;
    while(room) {
        if(*total>=MAP_ROOMS)return 0;
        for(uint32_t i=0;i<*total;i++)if(map_job.rooms[i]==room)return 0;
        uint64_t owner=0;
        if(!map_read(room,0x90,&owner,8)||owner!=level)return 0;
        map_job.rooms[(*total)++]=room;
        if(!map_read(room,0x48,&room,8))return 0;
    }
    return *total!=0;
}
void dm_android_map_pump(void) {
    if(!map_image)return;
    /* Only this original-thread function reads guest pointers. */
    dm_android_d2r_scene_status scene;dm_android_d2r_scene_status_get(&scene);
    dm_android_window_state window;dm_android_platform_get_window(&window);
    dm_hud_info hud;dm_hud_get(&hud);
    uint64_t player=0,level=0,map=0,callback=0;uint32_t id=0,mode=0,area=0;
    uint8_t version=0,paused=1,rift_pause=1;
    int ready=scene.game && !scene.navigation && window.focus==1 && !dm_rift_active() && !dm_hud_held();
    if(ready)player=map_player();
    if(player)level=map_level(player);
    ready=ready && level && map_read(player,8,&id,4) && map_read(player,0xc,&mode,4) && mode>0 && mode<=20 && mode!=17 &&
        map_read(player,0x1bd,&version,1) && version>=1 && version<=3 &&
        map_read((uintptr_t)map_image->base,0x2d5ff51,&paused,1) && !paused &&
        map_read((uintptr_t)map_image->base,0x2d5ff67,&rift_pause,1) && !rift_pause &&
        map_read(level,0x1f8,&area,4) && area &&
        map_read(level,0x1c8,&map,8) && map_read(map,0x838,&callback,8) &&
        callback>=(uintptr_t)map_image->base && callback-(uintptr_t)map_image->base<map_image->text_end && !(callback&3);
    uint64_t now=now_ns();
    pthread_mutex_lock(&map_lock);
    map_status.game=scene.game;
    /* 等待期间不调用游戏函数；换区、退出或超时会撤销请求。
     * Waiting never invokes guest callbacks; changing area, exiting or timing out cancels it. */
    if(map_status.state==DM_MAP_WAITING) {
        if(!scene.game || scene.world_epoch!=map_job.world ||
           (hud.valid && hud.area>0 && (uint32_t)hud.area!=map_job.area) || (ready && area!=map_job.area)) {
            map_status.state=DM_MAP_CANCELLED;map_status.error=ECANCELED;
        } else if(now>=map_job.deadline) {
            map_status.state=DM_MAP_FAILED;map_status.error=ETIMEDOUT;
        } else if(!ready) {
            map_status.ready=0;pthread_mutex_unlock(&map_lock);return;
        } else {
            map_job.player=player;map_job.id=id;map_job.level=level;
            map_job.callback=callback;map_job.version=version;
            map_status.area=area;
            map_status.state=DM_MAP_QUEUED;
        }
    }
    int busy=map_status.state==DM_MAP_QUEUED || map_status.state==DM_MAP_RUNNING;
    int changed=player!=map_job.player || id!=map_job.id || level!=map_job.level || scene.world_epoch!=map_job.world;
    if(busy && (!ready || player!=map_job.player || id!=map_job.id || level!=map_job.level ||
                scene.world_epoch!=map_job.world || callback!=map_job.callback || version!=map_job.version)) {
        map_status.state=DM_MAP_CANCELLED;map_status.error=ECANCELED;busy=0;
    }
    if(busy && now>=map_job.deadline){map_status.state=DM_MAP_FAILED;map_status.error=ETIMEDOUT;busy=0;}
    if(!busy) {
        /* 打开设置不清完成状态；换区、退局或玩家身份变化才清。
         * Preserve completion in options; clear it on area/exit/player changes. */
        if(map_status.state==DM_MAP_DONE && (!scene.game || scene.world_epoch!=map_job.world ||
           (hud.valid&&hud.area>0&&(uint32_t)hud.area!=map_job.area) || (ready&&changed))) {
            map_status.state=DM_MAP_IDLE;map_status.done=map_status.total=0;
        }
        if(map_status.state!=DM_MAP_DONE || ready) {
            map_job.player=player;map_job.id=id;map_job.level=level;map_job.world=scene.world_epoch;
            map_job.callback=callback;map_job.version=version;
            map_job.area=area?area:hud.valid&&hud.area>0?(uint32_t)hud.area:0;
        }
        map_job.sampled=now;map_status.ready=ready;map_status.area=map_job.area;
        pthread_mutex_unlock(&map_lock);return;
    }
    if(map_status.state==DM_MAP_QUEUED) {
        if(!map_rooms(level,&map_status.total)) {
            map_status.state=DM_MAP_FAILED;map_status.error=EINVAL;
            pthread_mutex_unlock(&map_lock);return;
        }
        map_status.state=DM_MAP_RUNNING;
    }
    uint32_t cursor=map_status.done,total=map_status.total;
    pthread_mutex_unlock(&map_lock);
    /* At most eight rooms / 1.5 ms per input poll. One guest call itself
     * cannot be preempted. Never hold a host lock across guest callbacks. */
    uint64_t until=now_ns()+1500000ull;int error=0;
    for(unsigned count=0;cursor<total && count<8;count++) {
        uint64_t room=map_job.rooms[cursor],owner=0,active=0;
        if(!map_read(room,0x90,&owner,8)||owner!=level){error=EINVAL;break;}
        map_init_room(version,room);
        if(!map_read(room,0x58,&active,8)||!active){error=EIO;break;}
        map_reveal_room(callback,active);cursor++;
        if(now_ns()>=until)break;
    }
    pthread_mutex_lock(&map_lock);map_status.done=cursor;
    if(error){map_status.state=DM_MAP_FAILED;map_status.error=error;}
    else if(cursor==total)map_status.state=DM_MAP_DONE;
    pthread_mutex_unlock(&map_lock);
}
