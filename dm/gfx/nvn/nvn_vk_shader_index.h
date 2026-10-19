#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const uint8_t *entry,*control,*code;
    uint32_t stage,code_size;
} nvn_shader_index_record;

/* The cursor is published only after the complete record passes validation.
 * v1 is the existing D2R control-only format; v2 compares complete code too. */
static inline int nvn_shader_index_read(const uint8_t *data,size_t bytes,uint32_t version,
                                      size_t *cursor,nvn_shader_index_record *out) {
    if(version!=1 && version!=2)return 0;
    size_t header=version==2?24:20;
    if(*cursor>bytes || bytes-*cursor<header+0x880)return 0;
    nvn_shader_index_record record={.entry=data+*cursor,.control=data+*cursor+header};
    memcpy(&record.stage,record.entry+16,4);
    if(record.stage!=0 && record.stage!=1 && record.stage!=5)return 0;
    if(version==2) {
        uint32_t declared,control_stage,magic,major,minor;
        memcpy(&record.code_size,record.entry+20,4);memcpy(&declared,record.control+0x704,4);
        memcpy(&control_stage,record.control+0x714,4);memcpy(&magic,record.control,4);
        memcpy(&major,record.control+4,4);memcpy(&minor,record.control+8,4);
        if(magic!=0x98761234 || major!=1 || minor!=16 || control_stage!=record.stage ||
           !record.code_size || record.code_size!=declared || record.code_size>16*1024*1024 ||
           record.code_size>bytes-*cursor-header-0x880)return 0;
        record.code=record.control+0x880;
    }
    *cursor+=header+0x880+record.code_size;*out=record;return 1;
}

static inline int nvn_shader_index_equal(const nvn_shader_index_record *a,const nvn_shader_index_record *b) {
    return a->stage==b->stage && a->code_size==b->code_size && !memcmp(a->control,b->control,0x880) &&
           (!a->code_size || !memcmp(a->code,b->code,a->code_size));
}
