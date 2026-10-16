/* Bounded, canonical disk cache. The blob is untrusted until both our envelope
 * and the Vulkan header match. This is optional acceleration, never game data. */
#ifndef DM_NVN_VK_PIPELINE_CACHE_IO_H
#define DM_NVN_VK_PIPELINE_CACHE_IO_H
#include "nvn_vk_pipeline_cache_key.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define NVN_VK_CACHE_MAX_BYTES (64u * 1024u * 1024u)
#define NVN_VK_CACHE_KEY_BYTES 68u
#define NVN_VK_CACHE_HEADER_BYTES (8u + NVN_VK_CACHE_KEY_BYTES + 8u)

static inline uint32_t nvn_vk_cache_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline void nvn_vk_cache_put32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));
}
static inline uint32_t nvn_vk_cache_checksum(const uint8_t *p,size_t n) {
    uint32_t h=2166136261u;for(size_t i=0;i<n;i++){h^=p[i];h*=16777619u;}return h;
}
static inline void nvn_vk_cache_key_encode(uint8_t out[NVN_VK_CACHE_KEY_BYTES],const NvnVkPipelineCacheKey *k) {
    const uint32_t fields[]={k->version,k->api_version,k->vendor_id,k->device_id,k->driver_id,
        k->driver_version,k->quirk_flags,k->render_tier,k->subgroup_size,k->subgroup_stages,
        k->subgroup_operations,k->enabled_features,k->shader_abi};
    for(unsigned i=0;i<13;i++)nvn_vk_cache_put32(out+4*i,fields[i]);
    memcpy(out+52,k->pipeline_cache_uuid,VK_UUID_SIZE);
}
static inline uint64_t nvn_vk_cache_key_hash(const NvnVkPipelineCacheKey *k) {
    uint8_t bytes[NVN_VK_CACHE_KEY_BYTES];nvn_vk_cache_key_encode(bytes,k);
    uint64_t h=14695981039346656037ull;
    for(size_t i=0;i<sizeof bytes;i++){h^=bytes[i];h*=1099511628211ull;}return h;
}
static inline int nvn_vk_cache_blob_valid(const void *data,size_t n,const NvnVkPipelineCacheKey *k) {
    if(!data||n<32||n>NVN_VK_CACHE_MAX_BYTES)return 0;
    const uint8_t *p=data;uint32_t header=nvn_vk_cache_u32(p);
    return header==32&&nvn_vk_cache_u32(p+4)==VK_PIPELINE_CACHE_HEADER_VERSION_ONE&&
        nvn_vk_cache_u32(p+8)==k->vendor_id&&nvn_vk_cache_u32(p+12)==k->device_id&&
        !memcmp(p+16,k->pipeline_cache_uuid,VK_UUID_SIZE);
}
/* 1=hit, 0=absent, -1=invalid/stale, -2=IO/allocation failure. */
static inline int nvn_vk_cache_read(const char *path,const NvnVkPipelineCacheKey *key,void **data,size_t *bytes) {
    *data=NULL;*bytes=0;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?0:-2;
    struct stat st;uint8_t header[NVN_VK_CACHE_HEADER_BYTES],encoded[NVN_VK_CACHE_KEY_BYTES];
    int result=-1;uint8_t *blob=NULL;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<(off_t)(sizeof header+32)||
        st.st_size>(off_t)(sizeof header+NVN_VK_CACHE_MAX_BYTES))goto done;
    if(read(fd,header,sizeof header)!=(ssize_t)sizeof header)goto done;
    nvn_vk_cache_key_encode(encoded,key);
    if(memcmp(header,"DMVKPC02",8)||memcmp(header+8,encoded,sizeof encoded))goto done;
    size_t n=nvn_vk_cache_u32(header+8+sizeof encoded);
    if(n<32||n>NVN_VK_CACHE_MAX_BYTES||st.st_size!=(off_t)(sizeof header+n))goto done;
    blob=malloc(n);if(!blob){result=-2;goto done;}
    size_t offset=0;while(offset<n){ssize_t got=read(fd,blob+offset,n-offset);
        if(got<0&&errno==EINTR)continue;
        if(got<=0)goto done;
        offset+=(size_t)got;}
    if(nvn_vk_cache_checksum(blob,n)!=nvn_vk_cache_u32(header+12+sizeof encoded)||
        !nvn_vk_cache_blob_valid(blob,n,key))goto done;
    *data=blob;*bytes=n;blob=NULL;result=1;
done:
    free(blob);close(fd);return result;
}
static inline int nvn_vk_cache_write(const char *path,const NvnVkPipelineCacheKey *key,const void *data,size_t bytes) {
    if(!nvn_vk_cache_blob_valid(data,bytes,key))return 0;
    uint8_t header[NVN_VK_CACHE_HEADER_BYTES]={0};memcpy(header,"DMVKPC02",8);
    nvn_vk_cache_key_encode(header+8,key);
    nvn_vk_cache_put32(header+8+NVN_VK_CACHE_KEY_BYTES,(uint32_t)bytes);
    nvn_vk_cache_put32(header+12+NVN_VK_CACHE_KEY_BYTES,nvn_vk_cache_checksum(data,bytes));
    char temporary[1400];int length=snprintf(temporary,sizeof temporary,"%s.tmp-XXXXXX",path);
    if(length<0||(size_t)length>=sizeof temporary)return 0;
    int fd=mkstemp(temporary);if(fd<0)return 0;int ok=1;
    for(unsigned part=0;part<2&&ok;part++){
        const uint8_t *p=part?(const uint8_t *)data:header;size_t n=part?bytes:sizeof header;
        while(n){ssize_t count=write(fd,p,n);if(count<0&&errno==EINTR)continue;
            if(count<=0){ok=0;break;}p+=count;n-=(size_t)count;}
    }
    if(ok&&fsync(fd))ok=0;
    if(close(fd))ok=0;
    if(ok&&rename(temporary,path))ok=0;
    if(!ok)unlink(temporary);
    return ok;
}
#endif
