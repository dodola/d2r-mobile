#pragma once
#include <stddef.h>
#include <stdint.h>

/* Game-thread widget transport for the shared Custom-page policy. Android
 * supplies this only for the reviewed D2R title, inside its guest input pump.
 * Reads copy fields; every write/call must revalidate the current tree/class.
 * find(direct=1) searches only root children, otherwise descendants to depth14.
 * No widget pointer or callback may be retained beyond the current pump. */
typedef struct {
    void *opaque;
    uint32_t generation;
    uint64_t (*find)(void *,uint64_t,const char *,int direct);
    int (*read)(void *,uint64_t,size_t,void *,size_t);
    int (*toggle)(void *,uint64_t,int);
    int (*rect)(void *,uint64_t,const int32_t[4]);
    int (*dirty)(void *,uint64_t,int);
    void (*wheel)(void *,uint64_t,int);
    int (*pad)(void *);
    uint64_t (*keys)(void *,uint64_t);
    uint64_t (*frame)(void *);
    /* 页内地图与 MOD 操作以及动态文字；各平台在调用前校验当前控件。
     * Inline map/MOD actions and dynamic text; each platform validates current widgets before calls. */
    void (*action)(void *,uint64_t);
    int (*clicked)(void *,uint64_t);
    int (*text)(void *,uint64_t,const char *);
} dm_custom_ui_ops;
