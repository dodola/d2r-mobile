/* 每提交的常量缓冲版本：只追加、按内容去重，重置仅允许在提交退役之后。
 * Submission-owned uniform versions: append-only with content deduplication; reset only after retirement.
 */
#ifndef NVN_VK_UNIFORM_ARENA_H
#define NVN_VK_UNIFORM_ARENA_H
#include <stdint.h>
#include <string.h>
#define NVN_UNIFORM_BLOCKS 32
#define NVN_UNIFORM_ENTRIES 8192
#define NVN_UNIFORM_BUCKETS 8192
typedef struct {void *owner;uint8_t *bytes;uint32_t offset;} NvnUniformRef;
typedef struct {void *owner;uint8_t *bytes;uint32_t size,used;} NvnUniformBlock;
typedef struct {uint32_t hash,size,block,offset,next;} NvnUniformEntry;
typedef struct {
    NvnUniformBlock blocks[NVN_UNIFORM_BLOCKS];
    NvnUniformEntry entries[NVN_UNIFORM_ENTRIES];
    uint32_t buckets[NVN_UNIFORM_BUCKETS],nblock,nentry;
} NvnUniformArena;
static inline void nvn_uniform_reset(NvnUniformArena *a) {
    a->nblock=a->nentry=0;memset(a->buckets,0,sizeof a->buckets);
}
static inline int nvn_uniform_add_block(NvnUniformArena *a,void *owner,void *bytes,uint32_t size) {
    if(a->nblock==NVN_UNIFORM_BLOCKS||!bytes||!size)return 0;
    a->blocks[a->nblock++]=(NvnUniformBlock){owner,bytes,size,0};return 1;
}
/* 返回 0 表示需要新块或独立缓冲；旧版本始终有效。2 表示内容复用。
 * Return 0 to request another block or a private buffer; old versions remain valid. Return 2 for content reuse.
 */
static inline int nvn_uniform_intern(NvnUniformArena *a,const void *bytes,uint32_t size,uint32_t alignment,uint32_t hash,int dedup,NvnUniformRef *out) {
    if(!size||!alignment)return 0;
    uint32_t bucket=hash&(NVN_UNIFORM_BUCKETS-1);
    if(dedup)for(uint32_t it=a->buckets[bucket];it;){
        NvnUniformEntry *e=&a->entries[it-1];NvnUniformBlock *b=&a->blocks[e->block];
        if(e->hash==hash&&e->size==size&&!memcmp(b->bytes+e->offset,bytes,size)){
            *out=(NvnUniformRef){b->owner,b->bytes,e->offset};return 2;
        }
        it=e->next;
    }
    if(!a->nblock)return 0;
    NvnUniformBlock *b=&a->blocks[a->nblock-1];uint64_t offset=((uint64_t)b->used+alignment-1)/alignment*alignment;
    if(offset>b->size||size>b->size-offset)return 0;
    memcpy(b->bytes+offset,bytes,size);b->used=(uint32_t)offset+size;
    *out=(NvnUniformRef){b->owner,b->bytes,(uint32_t)offset};
    if(dedup&&a->nentry<NVN_UNIFORM_ENTRIES){
        a->entries[a->nentry]=(NvnUniformEntry){hash,size,a->nblock-1,(uint32_t)offset,a->buckets[bucket]};a->buckets[bucket]=++a->nentry;
    }
    return 1;
}
#endif
