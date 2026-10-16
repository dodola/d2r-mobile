#include <stdio.h>
#include <stdlib.h>
#include "../../gfx/nvn/nvn_vk_ballot.h"
int main(int argc,char **argv) {
    if(argc!=3)return 1;
    FILE *in=fopen(argv[1],"rb");if(!in)return 2;
    fseek(in,0,SEEK_END);long length=ftell(in);rewind(in);if(length<20||length>16*1024*1024)return 3;
    size_t size=(size_t)length;uint8_t *data=malloc(size);if(!data||fread(data,1,size,in)!=size)return 4;fclose(in);
    unsigned count=0;uint8_t *out=dm_compute_ballot_apply(data,&size,&count);if(!out){free(data);return 5;}
    FILE *file=fopen(argv[2],"wb");if(!file||fwrite(out,1,size,file)!=size)return 6;fclose(file);free(out);
    printf("ballots=%u bytes=%zu\n",count,size);return 0;
}
