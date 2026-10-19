#include "../memory_permission.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const uint64_t base=0x10000,size=0x10000;
    assert(!dm_android_permission_validate(base,0x4000,1,base,size,4096));
    assert(!dm_android_permission_validate(base,0x4000,3,base,size,16384));
    assert(!dm_android_permission_validate(base,0x4000,0,base,size,16384));
    assert(dm_android_permission_validate(base+1,0x4000,1,base,size,4096));
    assert(dm_android_permission_validate(base,0,1,base,size,4096));
    assert(dm_android_permission_validate(base,0x4001,1,base,size,4096));
    assert(dm_android_permission_validate(base-4096,4096,1,base,size,4096));
    assert(dm_android_permission_validate(base,size+4096,1,base,size,4096));
    assert(dm_android_permission_validate(base+size,4096,1,base,size,4096));
    assert(dm_android_permission_validate(base,4096,5,base,size,4096));
    assert(dm_android_permission_validate(base,4096,2,base,size,4096));
    assert(dm_android_permission_validate(UINT64_MAX-4095,4096,1,base,size,4096));
    assert(dm_android_permission_validate(base+4096,4096,1,base,size,16384));
    assert(dm_android_permission_validate(base,4096,1,base,size,16384));
    puts("Guest permission bounds/permissions and 4 KiB/16 KiB host pages passed");
}
