/* Native iOS/Mac widget transport. Android opts into a checked title adapter. */
#if defined(DM_ANDROID_HOST)
extern int dm_android_custom_ui_context(uint64_t,dm_custom_ui_ops *) __attribute__((weak));
static int custom_context(uint64_t root,dm_custom_ui_ops *out) {
    return dm_android_custom_ui_context&&dm_android_custom_ui_context(root,out);
}
#else
static uint8_t *custom_main_base(void) {
    for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main"))return g_mod[i].base;
    return NULL;
}
static uint64_t custom_native_find(void *opaque,uint64_t w,const char *name,int direct) {
    (void)opaque;if(direct)return dm_ui_top(w,name);
    if(!w)return 0;const char *nm=*(const char **)(w+8);if(nm&&!strcmp(nm,name))return w;
    uint64_t children=*(uint64_t *)(w+0x58),count=*(uint64_t *)(w+0x60);
    static int depth;if(!children||count>256||depth>=14)return 0;
    depth++;uint64_t found=0;for(uint64_t i=0;i<count&&!found;i++)found=custom_native_find(NULL,*(uint64_t *)(children+i*8),name,0);
    depth--;return found;
}
static int custom_native_read(void *opaque,uint64_t w,size_t off,void *out,size_t bytes) {
    (void)opaque;if(!w)return 0;
    if(off==0xa88&&!dm_widget_is(custom_main_base(),w,DM_VT_TOGGLE))return 0;
    memcpy(out,(const void *)(w+off),bytes);return 1;
}
DM_WIDGET_RED_DEF
static int custom_native_toggle(void *opaque,uint64_t w,int on) {
    (void)opaque;if(!DM_WIDGET_OK(custom_main_base(),w,DM_VT_TOGGLE,"自定义页"))return 0;
    static const int32_t frames[2][4]={{0,1,3,2},{4,5,6,-1}};on=on!=0;
    *(uint8_t *)(w+0xa88)=on;memcpy((void *)(w+0x6e8),frames[on],sizeof frames[on]);*(int32_t *)(w+0x610)=frames[on][0];return 1;
}
static int custom_native_rect(void *opaque,uint64_t w,const int32_t rect[4]) {
    (void)opaque;memcpy((void *)(w+0x70),rect,16);((void(*)(uint64_t))*(uint64_t *)(*(uint64_t *)w+0x10))(w);return 1;
}
static int custom_native_dirty(void *opaque,uint64_t w,int dirty) {
    (void)opaque;uint64_t vt=*(uint64_t *)w,fn=(uint64_t)custom_main_base()+0xae5f28;
    for(int i=0;i<96;i++)if(*(uint64_t *)(vt+i*8)==fn){*(uint8_t *)(w+0x1a0)=dirty!=0;return 1;}
    return 0;
}
static void custom_native_wheel(void *opaque,uint64_t w,int direction) {(void)opaque;void dm_ui_wheel_on(uint64_t,int);dm_ui_wheel_on(w,direction);}
static int custom_native_pad(void *opaque) {(void)opaque;int dm_padmode_current(void);return dm_padmode_current();}
static uint64_t custom_native_keys(void *opaque,uint64_t mask) {(void)opaque;uint64_t dm_padmode_page_keys(uint64_t);return dm_padmode_page_keys(mask);}
static uint64_t custom_native_frame(void *opaque) {(void)opaque;uint64_t dm_guest_frame(void);return dm_guest_frame();}
static int custom_context(uint64_t root,dm_custom_ui_ops *out) {
    if(!root||!custom_main_base())return 0;
    *out=(dm_custom_ui_ops){.generation=dm_ui_gen(),.find=custom_native_find,.read=custom_native_read,.toggle=custom_native_toggle,
        .rect=custom_native_rect,.dirty=custom_native_dirty,.wheel=custom_native_wheel,.pad=custom_native_pad,.keys=custom_native_keys,.frame=custom_native_frame};return 1;
}
#endif
