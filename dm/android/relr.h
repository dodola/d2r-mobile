#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ELF64 DT_RELR: an even word is an address; an odd word describes the
 * following 63 pointer-sized locations. Addends live in the image itself.
 * https://gabi.xinuos.com/elf/06-reloc.html#relative-relocation-table
 *
 * The caller supplies an immutable copy of the table. Validate the complete
 * stream before writing, and confine writes to the module's RW image range.
 * memcpy also supports the even, unaligned addresses allowed by the ABI.
 */
static inline int dm_android_relr_apply(uint8_t *image, size_t image_size,
        size_t write_lo, size_t write_hi, const uint64_t *words,
        size_t nwords, size_t *applied) {
    if (!image || (!words && nwords) || write_lo>write_hi || write_hi>image_size) return -1;
    size_t count=0;
    for(unsigned pass=0;pass<2;pass++) {
        uint64_t cursor=0; int have_address=0;
        for(size_t i=0;i<nwords;i++) {
            uint64_t word=words[i], bits;
            if (!(word&1)) {
                if(word>UINT64_MAX-8) return -1;
                cursor=word; bits=1; have_address=1;
            } else {
                if(!have_address || cursor>UINT64_MAX-63*8) return -1;
                bits=word>>1;
            }
            unsigned width=(word&1)?63:1;
            for(unsigned bit=0;bit<width;bit++) if(bits&(UINT64_C(1)<<bit)) {
                uint64_t offset=cursor+bit*8;
                if(offset<write_lo || offset>write_hi || write_hi-offset<8) return -1;
                if(pass) {
                    uint64_t value; memcpy(&value,image+(size_t)offset,8);
                    value+=(uint64_t)(uintptr_t)image;
                    memcpy(image+(size_t)offset,&value,8);
                } else {
                    if(count==SIZE_MAX) return -1;
                    count++;
                }
            }
            cursor+=width*8;
        }
    }
    if(applied)*applied=count;
    return 0;
}
