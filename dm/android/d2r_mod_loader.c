/* Immutable prepared CASC packages, composed at build time against the exact
 * installed touch overlay. Raw PC D2RMM scripts are never run on Android. */
#include "d2r_mod_loader.h"
#include "../host/dm_sha1.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int mod_path(char *out,size_t capacity,const char *root,const char *leaf) {
    int n=snprintf(out,capacity,"%s/%s",root,leaf);return n>0 && (size_t)n<capacity;
}
static int mod_directory(const char *path) {
    struct stat st;return !lstat(path,&st) && S_ISDIR(st.st_mode);
}
static int mod_regular(const char *path) {
    struct stat st;return !lstat(path,&st) && S_ISREG(st.st_mode);
}
/* Identity checksum, not a signature/trust decision. Stream large archives. */
static int mod_hash(const char *path,char hex[41],uint64_t *bytes) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return 0;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0){close(fd);return 0;}
    uint32_t state[]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    unsigned char block[64];size_t used=0;uint64_t length=0;
    unsigned char buffer[32768];ssize_t n;
    while((n=read(fd,buffer,sizeof buffer))>0) {
        length+=(uint64_t)n;size_t at=0;
        while(at<(size_t)n) {
            size_t k=64-used;if(k>(size_t)n-at)k=(size_t)n-at;
            memcpy(block+used,buffer+at,k);used+=k;at+=k;
            if(used==64){dm_sha1_block(state,block);used=0;}
        }
    }
    close(fd);if(n<0 || length!=(uint64_t)st.st_size)return 0;
    unsigned char tail[128]={0};memcpy(tail,block,used);tail[used]=0x80;
    size_t padded=used<56?64:128;uint64_t bits=length*8;
    for(unsigned i=0;i<8;i++)tail[padded-1-i]=(unsigned char)(bits>>(i*8));
    dm_sha1_block(state,tail);if(padded==128)dm_sha1_block(state,tail+64);
    for(unsigned i=0;i<20;i++)snprintf(hex+i*2,3,"%02x",(unsigned char)(state[i/4]>>(24-(i%4)*8)));
    *bytes=length;return 1;
}
static int mod_line(FILE *f,char *out,size_t capacity) {
    if(!fgets(out,(int)capacity,f))return 0;
    size_t n=strlen(out);if(!n || out[n-1]!='\n')return 0;
    out[--n]=0;if(n && out[n-1]=='\r')out[--n]=0;
    for(size_t i=0;i<n;i++)if((unsigned char)out[i]<32 || (unsigned char)out[i]>126)return 0;
    return 1;
}
static int mod_id(const char *id) {
    size_t n=strlen(id);if(!n||n>48)return 0;
    for(size_t i=0;i<n;i++)if(!((id[i]>='a'&&id[i]<='z')||(id[i]>='0'&&id[i]<='9')||id[i]=='-'||id[i]=='_'))return 0;
    return 1;
}
static int mod_digest(const char *hex) {
    if(strlen(hex)!=40)return 0;
    for(unsigned i=0;i<40;i++)if(!((hex[i]>='a'&&hex[i]<='f')||(hex[i]>='0'&&hex[i]<='9')))return 0;
    return 1;
}
static int mod_archive(const char *name) {
    if(strlen(name)!=21 || strncmp(name,"data/",5) || name[7]!='-' || strcmp(name+16,".data"))return 0;
    for(unsigned i=5;i<16;i++)if(i!=7 && (name[i]<'0'||name[i]>'9'))return 0;
    return 1;
}
int dm_android_mod_configure(const char *root,char *overlay,size_t capacity,char *save,size_t save_capacity) {
    if(!root || !overlay || !save)return EINVAL;
    unsetenv("DM_ACTIVE_MOD");
    char mods[4096],path[4096],package[4096],mod_overlay[4096],id[64],line[256];
    if(!mod_path(mods,sizeof mods,root,"mods") || !mod_path(path,sizeof path,mods,"active.txt"))return ENAMETOOLONG;
    struct stat st;
    if(lstat(path,&st))return errno==ENOENT?0:errno;
    if(!mod_directory(mods)||!S_ISREG(st.st_mode))return EINVAL;
    FILE *f=fopen(path,"r");if(!f)return errno;
    int valid=mod_line(f,id,sizeof id) && fgetc(f)==EOF;fclose(f);
    if(!valid)return EINVAL;
    if(!id[0])return 0;
    if(!mod_id(id)||!overlay[0]||strchr(overlay,':'))return EINVAL;
    if(!mod_path(package,sizeof package,mods,id)||!mod_directory(package)||
       !mod_path(mod_overlay,sizeof mod_overlay,package,"overlay")||!mod_directory(mod_overlay)||
       !mod_path(path,sizeof path,mod_overlay,"data")||!mod_directory(path)||
       !mod_path(path,sizeof path,package,"package.txt")||!mod_regular(path))return EINVAL;
    f=fopen(path,"r");if(!f)return errno;
    valid=mod_line(f,line,sizeof line) && !strcmp(line,"d2r-mod-v1") &&
        mod_line(f,line,sizeof line) && !strcmp(line,"build db0507f36b84200ac05f75d76ed5026f") &&
        mod_line(f,line,sizeof line) && !strncmp(line,"base ",5) && mod_digest(line+5);
    char digest[41];uint64_t bytes=0;
    if(valid)valid=mod_path(path,sizeof path,overlay,"data/.build.config") &&
        mod_hash(path,digest,&bytes) && !strcmp(digest,line+5);
    unsigned files=0,configs=0;int ended=0;
    while(valid && mod_line(f,line,sizeof line)) {
        if(!strcmp(line,"end")){ended=1;break;}
        char expected[41],rel[64],extra;uint64_t expected_bytes;
        if(sscanf(line,"file %40s %" SCNu64 " %63s %c",expected,&expected_bytes,rel,&extra)!=3 ||
           !mod_digest(expected) || (strcmp(rel,"data/.build.config") && !mod_archive(rel)) || ++files>32) {valid=0;break;}
        configs+=!strcmp(rel,"data/.build.config");
        if(!mod_path(path,sizeof path,mod_overlay,rel)||!mod_hash(path,digest,&bytes)||
           bytes!=expected_bytes||strcmp(digest,expected)){valid=0;break;}
    }
    valid=valid && ended && configs==1 && files>=2 && fgetc(f)==EOF;fclose(f);
    if(!valid)return EINVAL;
    char chosen[8192],chosen_save[4096];
    if(snprintf(chosen,sizeof chosen,"%s:%s",mod_overlay,overlay)>=(int)sizeof chosen)return ENAMETOOLONG;
    /* If debug already selected a save, keep both forms of isolation. */
    int debug=strstr(save,"/debug/save")!=NULL;
    if(!mod_path(chosen_save,sizeof chosen_save,package,debug?"debug-save":"save") ||
       strlen(chosen)>=capacity || strlen(chosen_save)>=save_capacity)return ENAMETOOLONG;
    if(setenv("DM_ACTIVE_MOD",id,1))return errno;
    memcpy(overlay,chosen,strlen(chosen)+1);memcpy(save,chosen_save,strlen(chosen_save)+1);
    return 0;
}
