#include "../dm_sha1.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void check(const void *input,size_t bytes,const char *expected) {
    unsigned char digest[20];char text[41];dm_sha1(input,bytes,digest);
    for(unsigned i=0;i<20;i++)snprintf(text+i*2,3,"%02x",digest[i]);
    assert(!strcmp(text,expected));
}
int main(void) {
    check("",0,"da39a3ee5e6b4b0d3255bfef95601890afd80709");
    check("abc",3,"a9993e364706816aba3e25717850c26c9cd0d89d");
    const char *multi="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check(multi,strlen(multi),"84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    char *million=malloc(1000000);assert(million);memset(million,'a',1000000);
    check(million,1000000,"34aa973cd4c4daa4f61eeb2bdbad27316534016f");free(million);
    puts("SHA1_OK fips_vectors=4 empty_padding_multiblock_million=1");return 0;
}
