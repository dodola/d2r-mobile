/* Included by d2r_debug_mode.c. Real-time counterpart of tools/android/
 * build_gameplay_mod.py: the same Misc record fields (cost +232, vendor
 * min/max, PermStoreItem +436, spawnable +323, multibuy +437, item type +302)
 * are written into the loaded item table instead of shipping a package. The
 * in-memory rows have the BIN record layout (stride 0x1c0, code at +0x80).
 * Applied once per item table, only after a spot check of known rows.
 */
enum { SHOP_COST=232, SHOP_SPAWN=323, SHOP_PERM=436, SHOP_MULTI=437, SHOP_TYPE=302 };
static const uint16_t shop_vendor[5][2]={{343,360},{348,365},{351,368},{356,373},{357,374}};
static int shop_wanted(const char *code) {
    static const char *const fixed[]={"gpv","gpy","gpb","gpg","gpr","gpw","skz","rvl","toa","jew","pk1","pk2","pk3"};
    if(code[0]=='r'&&code[1]>='0'&&code[1]<='3'&&code[2]>='0'&&code[2]<='9'&&!code[3]){int n=atoi(code+1);return n>=1&&n<=33;}
    for(unsigned i=0;i<sizeof fixed/sizeof *fixed;i++)if(!strcmp(code,fixed[i]))return 1;
    return 0;
}
static int shop_row(uint64_t rows,uint32_t count,const char *code) {
    for(uint32_t i=0;i<count;i++){
        uint32_t v=0;char c[5]={0};
        if(!field(rows,(size_t)i*0x1c0+0x80,&v,4))return -1;
        memcpy(c,&v,4);for(int j=3;j>=0&&(c[j]==' '||!c[j]);j--)c[j]=0;
        if(!strcmp(c,code))return (int)i;
    }
    return -1;
}
static void shop_patch(uint64_t player) {
    static uint64_t done[4];
    uint8_t version=0;uint64_t table=0,rows=0;uint32_t count=0;
    if(!field(player,0x1bd,&version,1)||version<1||version>3||
       !field((uintptr_t)dbg.image->base,0x2d64a18+version*16,&table,8)||!table||
       !field(table,0x15c0,&rows,8)||!rows||!field(table,0x15c8,&count,4)||!count||count>16384||done[version]==rows)return;
    done[version]=rows;
    int hp=shop_row(rows,count,"hp1"),key=shop_row(rows,count,"key");
    uint32_t cost=0;uint8_t perm=0,spawn=0,amin=0,amax=0;
    if(hp<0||key<0||!field(rows,(size_t)hp*0x1c0+SHOP_COST,&cost,4)||cost!=30||
       !field(rows,(size_t)hp*0x1c0+SHOP_PERM,&perm,1)||perm!=1||!field(rows,(size_t)hp*0x1c0+SHOP_SPAWN,&spawn,1)||spawn!=1||
       !field(rows,(size_t)hp*0x1c0+shop_vendor[0][0],&amin,1)||amin!=1||!field(rows,(size_t)hp*0x1c0+shop_vendor[0][1],&amax,1)||amax!=1){
        LOG("ANDROID_DEBUG_SHOP skipped version=%u unexpected item table layout",version);return;
    }
    uint16_t key_type=0;field(rows,(size_t)key*0x1c0+SHOP_TYPE,&key_type,2);
    uint32_t cheap=0,stocked=0;
    for(uint32_t i=0;i<count;i++){
        uint64_t row=rows+(size_t)i*0x1c0;uint32_t v=0,price=0;char code[5]={0};
        if(!field(row,0x80,&v,4)||!field(row,SHOP_COST,&price,4))continue;
        memcpy(code,&v,4);for(int j=3;j>=0&&(code[j]==' '||!code[j]);j--)code[j]=0;
        if(price>1){uint32_t one=1;if(write_mem(row+SHOP_COST,&one,4))cheap++;}
        if(!shop_wanted(code))continue;
        uint8_t one=1;
        for(int vendor=0;vendor<5;vendor++){write_mem(row+shop_vendor[vendor][0],&one,1);write_mem(row+shop_vendor[vendor][1],&one,1);}
        write_mem(row+SHOP_PERM,&one,1);write_mem(row+SHOP_SPAWN,&one,1);
        if(!strcmp(code,"rvl"))write_mem(row+SHOP_MULTI,&one,1);
        if(code[0]=='p'&&code[1]=='k'&&key_type)write_mem(row+SHOP_TYPE,&key_type,2);
        stocked++;
    }
    LOG("ANDROID_DEBUG_SHOP version=%u rows=%u cheap=%u stocked=%u",version,count,cheap,stocked);
}
