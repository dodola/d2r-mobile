/* SHA-1 for upstream layout text identity checks (no platform crypto API). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t dm_sha1_rotate(uint32_t x,unsigned n) {return (x<<n)|(x>>(32-n));}
static inline void dm_sha1_block(uint32_t state[5],const unsigned char block[64]) {
    uint32_t words[80];
    for(unsigned i=0;i<16;i++)words[i]=(uint32_t)block[i*4]<<24|(uint32_t)block[i*4+1]<<16|
        (uint32_t)block[i*4+2]<<8|block[i*4+3];
    for(unsigned i=16;i<80;i++)words[i]=dm_sha1_rotate(words[i-3]^words[i-8]^words[i-14]^words[i-16],1);
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4];
    for(unsigned i=0;i<80;i++) {
        uint32_t f,k;
        if(i<20){f=(b&c)|(~b&d);k=0x5a827999;}
        else if(i<40){f=b^c^d;k=0x6ed9eba1;}
        else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdc;}
        else{f=b^c^d;k=0xca62c1d6;}
        uint32_t next=dm_sha1_rotate(a,5)+f+e+k+words[i];
        e=d;d=c;c=dm_sha1_rotate(b,30);b=a;a=next;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;
}
static inline void dm_sha1(const void *data,size_t bytes,unsigned char digest[20]) {
    uint32_t state[]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    const unsigned char *input=data;uint64_t bits=(uint64_t)bytes*8;
    while(bytes>=64){dm_sha1_block(state,input);input+=64;bytes-=64;}
    unsigned char tail[128]={0};
    if(bytes)memcpy(tail,input,bytes);
    tail[bytes]=0x80;size_t padded=bytes<56?64:128;
    for(unsigned i=0;i<8;i++)tail[padded-1-i]=(unsigned char)(bits>>(i*8));
    dm_sha1_block(state,tail);if(padded==128)dm_sha1_block(state,tail+64);
    for(unsigned i=0;i<20;i++)digest[i]=(unsigned char)(state[i/4]>>(24-(i%4)*8));
}
