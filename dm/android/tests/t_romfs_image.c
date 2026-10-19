#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../romfs_image.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put32(unsigned char *p, uint32_t value) { memcpy(p,&value,4); }
static void put64(unsigned char *p, uint64_t value) { memcpy(p,&value,8); }
static void fixture(const char *path, int trailing, int bad) {
    unsigned char bytes[1024]={0};
    uint64_t meta=trailing?512:80, data=trailing?128:512;
    uint64_t h[]={80,meta,4,meta+4,24,meta+28,4,meta+32,36,data};
    if (bad==2) h[8]=UINT64_MAX;
    memcpy(bytes,h,sizeof h);
    unsigned char *dir=bytes+meta+4,*file=bytes+meta+32;
    put32(dir+4,UINT32_MAX); put32(dir+8,UINT32_MAX);
    put32(dir+16,UINT32_MAX);
    put32(file+4,UINT32_MAX); put64(file+16,2);
    put32(file+24,UINT32_MAX); put32(file+28,1); file[32]='a';
    if (bad==1) put64(file+8,meta-data);
    memcpy(bytes+data,"ok",2);
    int fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0600); assert(fd>=0);
    assert(write(fd,bytes,sizeof bytes)==sizeof bytes); assert(!close(fd));
}
int main(int argc,char **argv) {
    if (argc==3) {
        assert(!romfs_image_setup(argv[1],argv[2]));
        assert(!romfs_image_shutdown()); puts("real RomFS metadata accepted"); return 0;
    }
    char dir[]="/tmp/dm-romfs-test-XXXXXX"; assert(mkdtemp(dir));
    for (int trailing=0;trailing<2;trailing++) {
        char source[256],view[256],file[256];
        snprintf(source,sizeof source,"%s/image",dir);
        snprintf(view,sizeof view,"%s/view",dir);
        snprintf(file,sizeof file,"%s/view/a",dir);
        fixture(source,trailing,0);
        assert(!romfs_image_setup_checked(source,view,1024));
        int type,fd; int64_t size; ssize_t received; char data[4]={0};
        assert(romfs_image_lookup(file,&type,&size)==1&&size==2);
        assert(romfs_image_open(file,1,&fd)==1);
        assert(romfs_image_pread(fd,data,sizeof data,0,&received)==1&&received==2);
        assert(!memcmp(data,"ok",2)); assert(romfs_image_close_fd(fd)==1);
        assert(!romfs_image_shutdown());
        for (int bad=1;bad<=2;bad++) {
            fixture(source,trailing,bad);
            assert(romfs_image_setup(source,view)==-1&&errno==EINVAL);
        }
        assert(!unlink(source)); assert(!rmdir(view));
    }
    assert(!rmdir(dir)); puts("RomFS leading/trailing metadata and invalid ranges passed");
}
