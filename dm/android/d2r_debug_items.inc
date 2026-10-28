/* Included by d2r_debug_mode.c. Server thread only, D2R 94012 only.
 * Chronicle preview units contain display-only flags and no rolled unique
 * properties. They MUST NOT be serialized into player saves. Generate real
 * units with the original drop descriptor instead (0x7c0b20 -> 0x656780).
 * Forced unique/set path: 0x653cfc / 0x654ba0, descriptor +0x3c and +0x50.
 * Ground placement mirrors 0x7c0b20: 0x5c84f0 + 0x6529e0 pick a free spot
 * next to the player; descriptor mode 3 puts the new unit on that spot.
 */
static uint64_t generate_item(uint64_t game,uint64_t player,uint32_t cls,int quality,int record) {
    uint8_t descriptor[0xd0]={0};uint8_t *b=dbg.image->base;
    uint32_t level=99,mode=3,force=record>=0,seed=(uint32_t)now_ns();uint16_t one=1,difficulty=0;
    uint64_t path=0,room=0;uint8_t scratch[8]={0};int32_t spot[2]={0,0};
    field(game,0x128,&difficulty,2);
    if(!field(player,0x38,&path,8)||!field(path,0x20,&room,8)||!room)return 0;
    ((void (*)(uint64_t,void *))(b+0x5c84f0))(player,scratch);
    uint64_t where=((uint64_t (*)(uint64_t,uint64_t,void *,void *,int))(b+0x6529e0))(game,room,scratch,spot,1);
    if(!where)return 0;
    memcpy(descriptor,&player,8);memcpy(descriptor+0x10,&game,8);
    memcpy(descriptor+0x18,&level,4);memcpy(descriptor+0x20,&cls,4);
    memcpy(descriptor+0x24,&mode,4);memcpy(descriptor+0x28,spot,8);memcpy(descriptor+0x30,&where,8);
    memcpy(descriptor+0x38,&one,2);
    memcpy(descriptor+0x3a,&difficulty,2);memcpy(descriptor+0x3c,&force,4);
    memcpy(descriptor+0x40,&quality,4);memcpy(descriptor+0x50,&record,4);
    memcpy(descriptor+0x58,&seed,4);
    return ((uint64_t (*)(uint64_t,void *,int))(b+0x656780))(game,descriptor,0);
}
static void discard_item(uint64_t game,uint64_t item) {
    if(item)((void (*)(uint64_t,uint64_t))(dbg.image->base+0x658be0))(game,item);
}
#define BAIL(code) do{LOG("ANDROID_DEBUG_ITEM fail=%d",(code));discard_item(game,item);return 0;}while(0)
/* kind 0 = set, 1 = unique. The item lands on the ground beside the player. */
static int catalog_item(uint64_t game,uint64_t player,int kind,uint32_t index,uint32_t cls) {
    uint8_t *b=dbg.image->base;uint8_t version=0;uint64_t table=0,rows=0,items=0;
    uint32_t count=0,itemcount=0,code=0;(void)cls;
    if(kind<0||kind>1||!field(player,0x1bd,&version,1)||version<1||version>3||
        !field((uintptr_t)b,0x2d64a18+version*16,&table,8)||!table||
        !item_table(player,&items,&itemcount))return 0;
    size_t off=kind?0x13e8:0x13c8,stride=kind?0x15c:0x1cc;
    if(!field(table,off,&rows,8)||!rows||!field(table,off+8,&count,4)||index>=count||count>2048||
        !field(rows,(size_t)index*stride+0x28,&code,4))return 0;
    for(cls=0;cls<itemcount;cls++){uint32_t c=0;if(!field(items,(size_t)cls*0x1c0+0x80,&c,4))return 0;if(c==code)break;}
    if(cls==itemcount)return 0;
    uint64_t item=generate_item(game,player,cls,kind?7:5,(int)index);
    if(!item){LOG("ANDROID_DEBUG_ITEM fail=10 generator");return 0;}
    uint64_t data=0;uint32_t quality=0,actual_index=0;
    if(!field(item,0x10,&data,8)||!data||!field(data,0,&quality,4)||
        !field(data,0x34,&actual_index,4)||quality!=(kind?7u:5u)||actual_index!=index)BAIL(11);
    /* Debug items are identified (ITEMFLAG_IDENTIFIED 0x10) so the rolled
     * properties are visible once picked up, without an identify scroll. */
    uint32_t item_flags=0;
    if(!field(data,0x18,&item_flags,4))BAIL(5);
    item_flags|=0x10u;
    if(!write_mem(data+0x18,&item_flags,4))die("debug catalogue cannot identify item");
    LOG("ANDROID_DEBUG_ITEM kind=%d record=%u cls=%u quality=%u id=%u ground=1",kind,index,cls,quality,*(uint32_t *)(item+8));
    return 1;
}
