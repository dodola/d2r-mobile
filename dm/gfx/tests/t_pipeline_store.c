#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include "../nvn/nvn_vk_pipeline_store.h"
static unsigned checks;
#define T(x) do{checks++;assert(x);}while(0)
static void missing(const char *p,unsigned kind,const DmPipelineIdentity *id,size_t limit){size_t n=99;void *b=dm_pipeline_read(p,kind,id,limit,&n);T(!b&&n==0);free(b);}
int main(void){
    char dir[]="/tmp/dm-pipeline-XXXXXX";T(mkdtemp(dir));char path[256];snprintf(path,sizeof path,"%s/cache",dir);
    DmPipelineIdentity id={.vendor=1,.device=2,.driver=3,.shader_crc=4,.key_size=96,.options=1};id.uuid[0]=5;
    const char payload[]="opaque-driver-data";size_t n=0;missing(path,1,&id,128);
    T(dm_pipeline_write(path,1,&id,payload,sizeof payload));
    void *b=dm_pipeline_read(path,1,&id,128,&n);T(b&&n==sizeof payload&&!memcmp(b,payload,n));free(b);
    missing(path,2,&id,128);missing(path,1,&id,sizeof payload-1);
    for(unsigned i=0;i<sizeof id;i++){DmPipelineIdentity stale=id;((unsigned char *)&stale)[i]^=1;missing(path,1,&stale,128);}
    unsigned char complete[sizeof(DmPipelineEnvelope)+sizeof payload];FILE *f=fopen(path,"rb");T(f&&fread(complete,1,sizeof complete,f)==sizeof complete);fclose(f);
    /* Every truncation and every one-byte corruption must reject the cache. */
    for(size_t i=0;i<sizeof complete;i++){
        f=fopen(path,"wb");T(f&&fwrite(complete,1,i,f)==i);fclose(f);missing(path,1,&id,128);
    }
    for(size_t i=0;i<sizeof complete;i++){
        unsigned char saved=complete[i];complete[i]^=1;f=fopen(path,"wb");T(f&&fwrite(complete,1,sizeof complete,f)==sizeof complete);fclose(f);
        /* Envelope padding/reserved bytes have no semantic effect. */
        if(i>=sizeof(DmPipelineEnvelope)||(i<offsetof(DmPipelineEnvelope,reserved)&&!(i>=offsetof(DmPipelineEnvelope,identity)+sizeof id&&i<offsetof(DmPipelineEnvelope,size))))missing(path,1,&id,128);
        complete[i]=saved;
    }
    T(dm_pipeline_write(path,1,&id,payload,sizeof payload));T(!dm_pipeline_write(path,1,&id,payload,0));
    char temp[300];snprintf(temp,sizeof temp,"%s.%d.new",path,getpid());T(!mkdir(temp,0700));
    T(!dm_pipeline_write(path,1,&id,"replacement",12));rmdir(temp);
    b=dm_pipeline_read(path,1,&id,128,&n);T(b&&n==sizeof payload&&!memcmp(b,payload,n));free(b);
    f=fopen(path,"ab");T(f&&fputc(0,f)!=EOF);fclose(f);missing(path,1,&id,128);
    T(!dm_pipeline_write("/no-such-directory/cache",1,&id,payload,sizeof payload));
    unlink(path);rmdir(dir);printf("PIPELINE_STORE checks=%u stale_identity=1 corruption=1 truncation=1 prior_file_preserved=1 passed=1\n",checks);return 0;
}
