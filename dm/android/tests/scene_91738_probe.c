/* Read-only scene evidence for the pinned Android build91738. Run under the
 * debuggable app UID. The slot table and lookup chain are verified against
 * original main 0x458220..0x45824c and 0x457e40..0x457e6c; do not reuse the
 * build94012 offsets from dm_world.c. No guest function is called or changed. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fd;
static int read_at(uint64_t address,void *out,size_t size){
    return address>=4096&&address<UINT64_C(0x10000000000)&&pread(fd,out,size,(off_t)address)==(ssize_t)size;
}
static uint64_t pointer(uint64_t address){uint64_t p=0;return read_at(address,&p,8)?p:0;}
static void renderer_snapshot(uint64_t base){
    /* Independently pin scale arithmetic and the UI/final target dimension
     * sources before interpreting this build-specific renderer layout. */
    static const uint32_t arithmetic[]={0x1e230021,0x528cd488,0xbc686800,0x1e210801,0x529dcf0a,0x1e250028};
    static const uint32_t ui[]={0x529f9808,0x8b080260,0xf9569268,0xf9400901,0xf9400b42,0x90000018};
    static const uint32_t final[]={0x529fae08,0x8b080260,0xf9569268,0xf9400901,0xf9400b42};
    uint32_t code[6];
    if(!read_at(base+0x160d210,code,sizeof arithmetic)||memcmp(code,arithmetic,sizeof arithmetic)||
       !read_at(base+0x160f188,code,sizeof ui)||memcmp(code,ui,sizeof ui)||
       !read_at(base+0x160f250,code,sizeof final)||memcmp(code,final,sizeof final)){
        printf("{\"readable\":false,\"version_verified\":false}");return;
    }
    uint64_t renderer=pointer(base+0x31b2528);float scale;uint32_t sizes[6];
    if(!renderer||!read_at(renderer+0x66a4,&scale,sizeof scale)||!isfinite(scale)||
       !read_at(renderer+0xee78,sizes,sizeof sizes)||renderer!=pointer(base+0x31b2528)){
        printf("{\"readable\":false,\"version_verified\":true}");return;
    }
    printf("{\"readable\":true,\"version_verified\":true,\"pointer\":\"0x%" PRIx64 "\",\"scale\":%.9g,\"internal\":[%u,%u],\"duplicate_internal\":[%u,%u],\"output\":[%u,%u]}",
           renderer,(double)scale,sizes[0],sizes[1],sizes[2],sizes[3],sizes[4],sizes[5]);
}
int main(int argc,char **argv){
    if(argc!=3)return 2;char *end;long pid=strtol(argv[1],&end,10);if(*end||pid<=0)return 2;
    uint64_t base=strtoull(argv[2],&end,0);if(*end||!base)return 2;
    char path[64];snprintf(path,sizeof path,"/proc/%ld/mem",pid);fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0){printf("{\"readable\":false,\"errno\":%d}\n",errno);return 1;}
    const unsigned char lookup[]={0x00,0xd8,0x61,0xf8,0x80,0x00,0x00,0xb5};
    unsigned char code[sizeof lookup];
    if(!read_at(base+0x457e40,code,sizeof code)||memcmp(code,lookup,sizeof code)){
        printf("{\"readable\":true,\"version_verified\":false}\n");close(fd);return 1;
    }
    uint32_t ids[8],mode=UINT32_MAX;uint64_t buckets[128];
    if(!read_at(base+0x2d320e8,ids,sizeof ids)||!read_at(base+0x2d32108,buckets,sizeof buckets))return 1;
    read_at(base+0x2d4d844,&mode,sizeof mode);
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    uint64_t ns=(uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
    printf("{\"readable\":true,\"version_verified\":true,\"pid\":%ld,\"mono_ns\":%" PRIu64 ",\"frontend_mode\":%u,\"players\":[",pid,ns,mode);
    unsigned emitted=0;
    for(unsigned slot=0;slot<8;slot++){
        if(ids[slot]==UINT32_MAX)continue;
        uint64_t unit=buckets[ids[slot]&127];
        for(unsigned guard=0;unit&&guard<256;guard++){
            uint32_t header[4];if(!read_at(unit,header,sizeof header))break;
            if(header[0]==0&&header[2]==ids[slot]){
                uint64_t p=pointer(unit+0x38),room=p?pointer(p+0x20):0,ex=room?pointer(room+0x18):0,level=ex?pointer(ex+0x90):0;
                uint32_t xy[2]={0},area=0;read_at(p,xy,sizeof xy);if(level)read_at(level+0x1f8,&area,sizeof area);
                printf("%s{\"slot\":%u,\"id\":%u,\"unit\":\"0x%" PRIx64 "\",\"mode\":%u,\"path\":\"0x%" PRIx64 "\",\"x_fixed\":%u,\"y_fixed\":%u,\"level\":%u,\"level_ptr\":\"0x%" PRIx64 "\"}",
                    emitted++?",":"",slot,ids[slot],unit,header[3],p,xy[0],xy[1],area,level);break;
            }
            uint64_t next=pointer(unit+0x158);if(next==unit)break;unit=next;
        }
    }
    printf("],\"renderer\":");renderer_snapshot(base);printf("}\n");close(fd);return 0;
}
