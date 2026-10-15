/* Private, versioned cache envelopes. Invalid/stale/truncated data is a miss.
 * The Vulkan payload is opaque; only vkGetPipelineCacheData produces it. */
#ifndef NVN_VK_PIPELINE_STORE_H
#define NVN_VK_PIPELINE_STORE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>
typedef struct {
    uint32_t vendor,device,driver,shader_crc,key_size,options;
    uint8_t uuid[16];
} DmPipelineIdentity;
typedef struct {
    char magic[8];uint32_t version,kind;
    DmPipelineIdentity identity;uint64_t size;uint32_t crc,reserved;
} DmPipelineEnvelope;
static void *dm_pipeline_read(const char *path,uint32_t kind,const DmPipelineIdentity *id,size_t limit,size_t *size) {
    *size=0;FILE *f=fopen(path,"rb");if(!f)return NULL;
    DmPipelineEnvelope h;struct stat st;void *data=NULL;
    if(fstat(fileno(f),&st)||fread(&h,1,sizeof h,f)!=sizeof h||memcmp(h.magic,"DMVKPSO",8)||h.version!=1||h.kind!=kind||
       memcmp(&h.identity,id,sizeof *id)||!h.size||h.size>limit||h.size+sizeof h!=(uint64_t)st.st_size)goto done;
    data=malloc((size_t)h.size);if(!data)goto done;
    if(fread(data,1,(size_t)h.size,f)!=(size_t)h.size||crc32(0,data,(uInt)h.size)!=h.crc){free(data);data=NULL;goto done;}
    *size=(size_t)h.size;
done:fclose(f);return data;
}
static int dm_pipeline_write(const char *path,uint32_t kind,const DmPipelineIdentity *id,const void *data,size_t size) {
    if(!size||size>64u*1024*1024)return 0;
    char temp[1400];if(snprintf(temp,sizeof temp,"%s.%d.new",path,getpid())>=(int)sizeof temp)return 0;
    DmPipelineEnvelope h={0};memcpy(h.magic,"DMVKPSO",8);h.version=1;h.kind=kind;h.identity=*id;h.size=size;h.crc=crc32(0,data,(uInt)size);
    int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);if(fd<0)return 0;
    FILE *f=fdopen(fd,"wb");if(!f){close(fd);unlink(temp);return 0;}
    int ok=fwrite(&h,1,sizeof h,f)==sizeof h&&fwrite(data,1,size,f)==size&&!fflush(f)&&!fsync(fd);
    if(fclose(f))ok=0;
    if(ok&&rename(temp,path))ok=0;
    if(!ok)unlink(temp);
    return ok;
}
#endif
