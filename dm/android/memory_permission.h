#pragma once
#include <stdint.h>
#include <stddef.h>

/* Horizon page size is 4 KiB. A larger host page cannot safely change a
 * neighbouring guest page's protection, so such requests fail explicitly. */
static inline uint32_t dm_android_permission_validate(uint64_t address,uint64_t size,
        uint64_t permission,uint64_t region,uint64_t region_size,size_t host_page) {
    if(address&4095)return (102u<<9)|1u;
    if(!size || (size&4095))return (101u<<9)|1u;
    if(address>UINT64_MAX-size)return (106u<<9)|1u;
    if(permission!=0 && permission!=1 && permission!=3)return (108u<<9)|1u;
    if(address<region || size>region_size || address-region>region_size-size)return (106u<<9)|1u;
    if(!host_page || (host_page&(host_page-1)) || address%host_page || size%host_page)return (106u<<9)|1u;
    return 0;
}
