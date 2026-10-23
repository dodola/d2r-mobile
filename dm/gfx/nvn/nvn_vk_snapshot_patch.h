/* 为已退役槽的只读快照更新脏页；调用者在拷贝前保护源页并保存新代号。
 * Update dirty pages of a retired slot's read-only snapshot; the caller protects source pages and captures the new generation before copying.
 */
#ifndef NVN_VK_SNAPSHOT_PATCH_H
#define NVN_VK_SNAPSHOT_PATCH_H
#include "nvn_dirty.h"
#include "nvn_vk_pool_upload.h"
#include <string.h>
/* 合并写入交集，确保 GPU 覆盖拷贝的目标互不重叠。
 * Merge write intersections so GPU overlay copy destinations never overlap.
 */
static inline int nvn_vk_snapshot_next_write(PoolUploadRange range,const PoolUploadRange *writes,size_t count,
        uint64_t *cursor,PoolUploadRange *span) {
    uint64_t end=range.addr+range.size,lo=end,hi=0;
    for(size_t i=0;i<count;i++){
        uint64_t a=writes[i].addr,b=a+writes[i].size;if(a<*cursor)a=*cursor;if(b>end)b=end;
        if(a<b&&a<lo){lo=a;hi=b;}
    }
    if(lo==end)return 0;
    for(;;){uint64_t before=hi;
        for(size_t i=0;i<count;i++){
            uint64_t a=writes[i].addr,b=a+writes[i].size;
            if(a<=hi&&b>lo&&b>hi)hi=b<end?b:end;
        }
        if(hi==before)break;
    }
    *span=(PoolUploadRange){lo,hi-lo};*cursor=hi;return 1;
}
static inline uint64_t nvn_vk_snapshot_patch(void *destination,const void *source,size_t size,uint64_t previous) {
    if(!previous){memcpy(destination,source,size);return size;}
    size_t cursor=0,offset=0,length=0;uint64_t copied=0;
    while(nvn_dirty_next_range(source,size,previous,&cursor,&offset,&length)){
        if(!length||offset>size||length>size-offset)return UINT64_MAX;
        memcpy((uint8_t *)destination+offset,(const uint8_t *)source+offset,length);copied+=length;
    }
    return copied;
}
#endif
