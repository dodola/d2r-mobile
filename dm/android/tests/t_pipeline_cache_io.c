#include "../../gfx/nvn/nvn_vk_pipeline_cache_io.h"
#include <assert.h>
#include <limits.h>
static unsigned checks;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#c);return 1;}}while(0)
static void envelope(uint8_t *file,const NvnVkPipelineCacheKey *key,const uint8_t *blob,size_t bytes) {
    memcpy(file,"DMVKPC02",8);nvn_vk_cache_key_encode(file+8,key);
    nvn_vk_cache_put32(file+8+NVN_VK_CACHE_KEY_BYTES,(uint32_t)bytes);
    nvn_vk_cache_put32(file+12+NVN_VK_CACHE_KEY_BYTES,nvn_vk_cache_checksum(blob,bytes));
    memcpy(file+NVN_VK_CACHE_HEADER_BYTES,blob,bytes);
}
static int raw(const char *path,const void *data,size_t bytes) {
    FILE *f=fopen(path,"wb");if(!f)return 0;
    int ok=fwrite(data,1,bytes,f)==bytes;return fclose(f)==0&&ok;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    char directory[1000],path[1100],linkpath[1100];
    snprintf(directory,sizeof directory,"%s/cache-io-XXXXXX",argv[1]);CHECK(mkdtemp(directory)!=NULL);
    snprintf(path,sizeof path,"%s/cache.bin",directory);snprintf(linkpath,sizeof linkpath,"%s/link",directory);
    VkPhysicalDeviceProperties properties={.apiVersion=VK_API_VERSION_1_1,.vendorID=0x5143,.deviceID=1234};
    for(unsigned i=0;i<VK_UUID_SIZE;i++)properties.pipelineCacheUUID[i]=(uint8_t)(i+1);
    NvnVkPipelineCacheKey key=nvn_vk_pipeline_cache_key_make(&properties,NULL,NULL);
    uint8_t blob[256]={0},file[NVN_VK_CACHE_HEADER_BYTES+sizeof blob];
    nvn_vk_cache_put32(blob,32);nvn_vk_cache_put32(blob+4,VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
    nvn_vk_cache_put32(blob+8,key.vendor_id);nvn_vk_cache_put32(blob+12,key.device_id);
    memcpy(blob+16,key.pipeline_cache_uuid,VK_UUID_SIZE);
    for(unsigned i=32;i<sizeof blob;i++)blob[i]=(uint8_t)i;
    void *data=NULL;size_t bytes=0;
    CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==0&&data==NULL&&bytes==0);
    CHECK(nvn_vk_cache_write(path,&key,blob,sizeof blob));
    CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==1&&bytes==sizeof blob&&!memcmp(data,blob,bytes));free(data);
    for(unsigned i=0;i<sizeof key;i++){
        NvnVkPipelineCacheKey other=key;((uint8_t *)&other)[i]^=1;
        CHECK(nvn_vk_cache_read(path,&other,&data,&bytes)==-1&&data==NULL&&bytes==0);
    }
    uint8_t canonical[NVN_VK_CACHE_KEY_BYTES];nvn_vk_cache_key_encode(canonical,&key);
    CHECK(canonical[0]==2&&canonical[8]==0x43&&canonical[9]==0x51&&canonical[52]==1);
    CHECK(symlink(path,linkpath)==0);CHECK(nvn_vk_cache_read(linkpath,&key,&data,&bytes)==-2);unlink(linkpath);
    int fifo=mkfifo(linkpath,0600),fifo_skipped=0;
    if(fifo==0){CHECK(nvn_vk_cache_read(linkpath,&key,&data,&bytes)==-1);unlink(linkpath);}
    else {CHECK(errno==EPERM||errno==EACCES);fifo_skipped=1;}
    envelope(file,&key,blob,sizeof blob);
    for(size_t n=0;n<sizeof file;n++){
        CHECK(raw(path,file,n));CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==-1&&data==NULL);
    }
    CHECK(raw(path,file,sizeof file));FILE *f=fopen(path,"ab");CHECK(f!=NULL);fputc(0,f);fclose(f);
    CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==-1);
    file[sizeof file-1]^=1;CHECK(raw(path,file,sizeof file));CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==-1);
    /* A checksummed envelope cannot disguise an incompatible Vulkan header. */
    for(unsigned offset=0;offset<32;offset+=4){
        uint8_t bad[sizeof blob];memcpy(bad,blob,sizeof bad);bad[offset]^=0x80;
        envelope(file,&key,bad,sizeof bad);CHECK(raw(path,file,sizeof file));
        CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==-1);
    }
    envelope(file,&key,blob,sizeof blob);nvn_vk_cache_put32(file+8+NVN_VK_CACHE_KEY_BYTES,UINT32_MAX);
    CHECK(raw(path,file,sizeof file));CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==-1);
    int fd=open(path,O_WRONLY|O_TRUNC);CHECK(fd>=0);
    CHECK(ftruncate(fd,(off_t)NVN_VK_CACHE_MAX_BYTES+NVN_VK_CACHE_HEADER_BYTES+1)==0);close(fd);
    CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==-1);
    CHECK(nvn_vk_cache_write(path,&key,blob,sizeof blob));
    CHECK(!nvn_vk_cache_write(path,&key,blob,16));
    CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==1&&!memcmp(data,blob,bytes));free(data);
    CHECK(!nvn_vk_cache_write(directory,&key,blob,sizeof blob));
    CHECK(nvn_vk_cache_write(path,&key,blob,32));
    CHECK(nvn_vk_cache_read(path,&key,&data,&bytes)==1&&bytes==32);free(data);
    CHECK(unlink(path)==0);CHECK(rmdir(directory)==0);
    printf("PIPELINE_CACHE_IO checks=%u fifo_skipped=%d passed=1\n",checks,fifo_skipped);return 0;
}
