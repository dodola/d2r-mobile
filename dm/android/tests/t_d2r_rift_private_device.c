/* Real upstream private gate semantics with a synthetic run and ARM item
 * predicate. State is injected only into this standalone fixture process. */
#include "../../host/dm_rift.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

module_t g_mod[MAX_MOD];int g_nmod;
void (*g_dm_pre_commit_hook[5])(const char *);
uint64_t dm_fs_commit_count(void) {return 0;}
static unsigned notices;
void dm_game_notice(uint8_t *base,const char *text) {assert(base==B && text);notices++;}
const char *dm_str(uint8_t *base,const char *key) {assert(base==B);return key;}
static int accepted_type=10;
static int item_type(uint64_t item,int type) {assert(item);return type==accepted_type;}
static void put64(void *object,size_t offset,uint64_t value) {memcpy((uint8_t *)object+offset,&value,8);}
static void put32(void *object,size_t offset,uint32_t value) {memcpy((uint8_t *)object+offset,&value,4);}
static void fill(uint64_t registers[31]) {for(unsigned i=0;i<31;i++)registers[i]=0x8877660000000000ull+i;}
static void unchanged(const uint64_t before[31],const uint64_t after[31],uint32_t allowed) {
    for(unsigned i=0;i<31;i++)if(!(allowed&(1u<<i)))assert(before[i]==after[i]);
}
int main(void) {
    _Alignas(8) uint8_t game[512]={0},player[512]={0},path[128]={0},room[64]={0},extra[160]={0},level[512]={0};
    uint64_t G=(uintptr_t)game,pl=(uintptr_t)player;
    put64(player,0x38,(uintptr_t)path);put64(path,0x20,(uintptr_t)room);
    put64(room,0x18,(uintptr_t)extra);put64(extra,0x90,(uintptr_t)level);put32(level,0x1f8,111);
    L.G=G;L.player=pl;K.nmaps=2;K.maps[0]=111;K.maps[1]=112;
    uint64_t regs[31],before[31];
    fill(regs);regs[0]=G;regs[1]=pl;regs[2]=0;memcpy(before,regs,sizeof regs);
    atomic_store(&K.st,ST_IDLE);svc_rift_exit(regs);assert(!regs[8]);unchanged(before,regs,1u<<8);
    atomic_store(&K.st,ST_RUN);K.cur=0;svc_rift_exit(regs);
    assert(regs[2]==112 && regs[3]==11 && regs[8]==1 && K.cur==1);unchanged(before,regs,(1u<<2)|(1u<<3)|(1u<<8));
    regs[2]=0;svc_rift_exit(regs);assert(regs[2]==112 && K.cur==1);
    regs[0]=G+8;memcpy(before,regs,sizeof regs);svc_rift_exit(regs);assert(!regs[8]);unchanged(before,regs,1u<<8);
    regs[0]=G;regs[1]=pl+8;memcpy(before,regs,sizeof regs);svc_rift_exit(regs);assert(!regs[8]);unchanged(before,regs,1u<<8);
    regs[1]=pl;put32(level,0x1f8,109);memcpy(before,regs,sizeof regs);svc_rift_exit(regs);assert(!regs[8]);unchanged(before,regs,1u<<8);

    fill(regs);regs[21]=G;regs[22]=pl;regs[9]=1;put32(level,0x1f8,111);memcpy(before,regs,sizeof regs);
    atomic_store(&K.st,ST_IDLE);svc_save_act(regs);assert(!regs[8] && regs[9]==1);unchanged(before,regs,1u<<8);
    atomic_store(&K.st,ST_RUN);svc_save_act(regs);assert(!regs[8] && regs[9]==4);unchanged(before,regs,(1u<<8)|(1u<<9));
    atomic_store(&K.st,ST_DONE);regs[9]=2;svc_save_act(regs);assert(regs[9]==4);
    put32(level,0x1f8,109);regs[9]=2;memcpy(before,regs,sizeof regs);svc_save_act(regs);assert(regs[9]==2);unchanged(before,regs,1u<<8);

    size_t bytes=0x2d66000,page=(size_t)sysconf(_SC_PAGESIZE);
    B=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(B!=MAP_FAILED);
    uint32_t instructions[]={0x58000050,0xd61f0200};void *function=item_type;
    memcpy(B+0x574bc0,instructions,sizeof instructions);memcpy(B+0x574bc8,&function,8);
    __builtin___clear_cache((char *)B+0x574bc0,(char *)B+0x574bd0);
    assert(!mprotect(B+(0x574bc0&~(page-1)),page,PROT_READ|PROT_EXEC));
    _Alignas(8) uint8_t owner[512]={0},item[512]={0},table[0x600]={0},merc_row[0x150]={0};
    uint64_t owner_pointer=(uintptr_t)owner;
    owner[0x1bd]=1;put32(owner,0,1);put32(owner,4,271);put32(item,8,21);
    put64(B,0x2d64a18+16,(uintptr_t)table);put64(table,0x558,(uintptr_t)merc_row);put64(table,0x560,1);put32(merc_row,8,271);
    fill(regs);regs[0]=0;regs[20]=(uintptr_t)item;regs[22]=(uintptr_t)&owner_pointer;memcpy(before,regs,sizeof regs);
    R.best=0;atomic_store(&g_merc_req,0);svc_merc_slot(regs);
    assert(regs[0]==1 && !regs[8]);unchanged(before,regs,(1u<<0)|(1u<<8)); /* Existing save equipment must survive. */
    uint8_t request=0x51;dm_rift_c2s(G,&request,1);regs[0]=0;svc_merc_slot(regs);
    assert(!regs[0] && !regs[8] && notices==1); /* A fresh equip remains locked. */
    R.best=10;svc_merc_slot(regs);assert(regs[0]==1 && !regs[8]);
    regs[0]=0;put32(owner,4,272);svc_merc_slot(regs);assert(!regs[0]); /* Non-mercenary owner. */
    put32(owner,4,271);accepted_type=3;svc_merc_slot(regs);assert(!regs[0]); /* Ordinary armor keeps the original check. */
    accepted_type=10;

    fill(regs);regs[1]=0x25;regs[19]=(uintptr_t)item;R.best=0;memcpy(before,regs,sizeof regs);
    svc_merc_slot_cl(regs);assert(regs[1]==0x25 && !regs[8]);unchanged(before,regs,1u<<8);
    R.best=10;const int types[]={10,12,15,16,19};
    for(unsigned i=0;i<5;i++){accepted_type=types[i];regs[1]=0x25;svc_merc_slot_cl(regs);assert(regs[1]==(uint64_t)types[i] && !regs[8]);unchanged(before,regs,(1u<<1)|(1u<<8));}
    regs[1]=3;memcpy(before,regs,sizeof regs);svc_merc_slot_cl(regs);assert(regs[1]==3);unchanged(before,regs,1u<<8);
    puts("ANDROID_D2R_RIFT_PRIVATE_OK exit_cases=6 save_act_cases=4 merc_server_cases=5 merc_client_cases=7 load_equipment_preserved=1 registers_preserved=1");return 0;
}
