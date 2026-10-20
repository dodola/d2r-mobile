/* Included by dm_custom.c so iOS and Android use the same key order,
 * pending settings, radio rules and Apply/Discard/default state. */
static int custom_read(const dm_custom_ui_ops *o,uint64_t w,size_t off,void *out,size_t bytes) {
    return w && o->read(o->opaque,w,off,out,bytes);
}
static int custom_visible(const dm_custom_ui_ops *o,uint64_t w) {
    uint8_t visible=0;return custom_read(o,w,0x50,&visible,1)&&visible;
}
static int custom_toggle_value(const dm_custom_ui_ops *o,uint64_t w) {
    uint8_t on=0;return custom_read(o,w,0xa88,&on,1)?on!=0:-1;
}
static uint64_t custom_parent(const dm_custom_ui_ops *o,uint64_t w) {
    uint64_t parent=0;custom_read(o,w,0x30,&parent,sizeof parent);return parent;
}
static uint64_t custom_find(const dm_custom_ui_ops *o,uint64_t w,const char *name) {
    return w?o->find(o->opaque,w,name,0):0;
}
static int custom_rect(const dm_custom_ui_ops *o,uint64_t w,int32_t out[4]) {
    return custom_read(o,w,0x70,out,4*sizeof *out);
}
static int custom_key_visible(int i) {
    if(i<0||i>=NK)return 0;
#if defined(DM_ANDROID_HOST) && DM_ANDROID_HOST
    /* Keep the shared key table and its indices intact; Android has no cloud
     * implementation, so the iOS-only row must not participate in the UI. */
    return (i<K_SHOW_FPS||dm_android_custom_settings_apply)&&(!dm_android_custom_settings_apply||i!=K_Q3D)&&strcmp(k_keys[i],"icloud")!=0;
#else
    return i<K_SHOW_FPS;
#endif
}
static int custom_first_visible(void) {
    for(int i=0;i<NK;i++)if(custom_key_visible(i))return i;
    return 0;
}
static int custom_next_visible(int from,int direction) {
    int i=from+direction;
    while(i>=0&&i<NK&&!custom_key_visible(i))i+=direction;
    return i>=0&&i<NK?i:from;
}
void dm_custom_ui_pump(unsigned long long root) {
    dm_custom_ui_ops ops;
    if(!custom_context(root,&ops))return;
    const dm_custom_ui_ops *o=&ops;
    static struct {uint64_t w,z;uint32_t gen;} seen[NK];
    uint64_t sp=o->find(o->opaque,root,"VideoOptionsPanel",1);
    if(!sp || !custom_visible(o,sp)) {
        int was=0;for(int i=0;i<NK;i++)was|=seen[i].w!=0;
        if(was)for(int i=0;i<NK;i++)atomic_store(&g_pend[i],-1);
        memset(seen,0,sizeof seen);return;
    }
    load_once();if(atomic_exchange(&g_repaint,0))memset(seen,0,sizeof seen);
    if(atomic_load(&g_lang_explicit)<0)atomic_store(&g_val[0],lang_running());
    int pad=o->pad?o->pad(o->opaque):0;

    uint64_t cm=custom_find(o,root,"ConfirmationModal");
    if(custom_visible(o,cm))pad=0;
    static int selected;
    static uint64_t selected_panel;static uint32_t selected_generation;
    if(selected_panel!=sp || selected_generation!=o->generation) {
        selected_panel=sp;selected_generation=o->generation;selected=custom_first_visible();
    }
    if(!custom_key_visible(selected))selected=custom_first_visible();
    uint64_t selection=custom_find(o,sp,"TouchCustomSel"),tw=0,cw=0;
    char name[64];
    if(pad) {
        uint64_t ev=o->keys(o->opaque,DM_BTN_UP|DM_BTN_DOWN|DM_BTN_A);
        if(ev&DM_BTN_DOWN)selected=custom_next_visible(selected,1);
        else if(ev&DM_BTN_UP)selected=custom_next_visible(selected,-1);
        snprintf(name,sizeof name,"TouchCustom_%s",k_keys[selected]);cw=custom_find(o,sp,selected==0?k_lang_box[LANG_N-1]:selected==K_RENDER_MODE?"TouchCustom_render_mode_3":name);
        snprintf(name,sizeof name,"TouchCustom_%s_Text",k_keys[selected]);tw=custom_find(o,sp,name);
        if((ev&DM_BTN_A) && cw && (selected==K_MOD||selected==K_MAP_REVEAL)) {
            if(o->action)o->action(o->opaque,cw);
        } else if((ev&DM_BTN_A) && cw && seen[selected].w==cw && seen[selected].gen==o->generation) {
            if(selected==K_RENDER_MODE) {
                int pd=atomic_load(&g_pend[selected]),shown=pd>=0?pd:atomic_load(&g_val[selected]);
                atomic_store(&g_pend[selected],(shown+1)%4);
                for(int k=0;k<4;k++){snprintf(name,sizeof name,"TouchCustom_render_mode_%d",k);o->toggle(o->opaque,custom_find(o,sp,name),k==(shown+1)%4);}
            } else if(selected==0) {
                int pd=atomic_load(&g_pend[0]),shown=pd>=0?pd:atomic_load(&g_val[0]);
                atomic_store(&g_pend[0],(shown+1)%LANG_N);
                for(int k=0;k<LANG_N;k++)o->toggle(o->opaque,custom_find(o,sp,k_lang_box[k]),k==(shown+1)%LANG_N);
            } else {
            int on=custom_toggle_value(o,cw);
            uint64_t z=rev_box(selected)?custom_find(o,sp,rev_box(selected)):0;
            if(on>=0 && (!rev_box(selected)||custom_toggle_value(o,z)>=0)) {
                int pd=atomic_load(&g_pend[selected]),shown=pd>=0?pd:atomic_load(&g_val[selected]);
                int to=rev_box(selected)?!shown:!on;
                if(o->toggle(o->opaque,cw,to) && (!z||o->toggle(o->opaque,z,!to))) {
                    if(z)atomic_store(&g_pend[selected],to);
                }
            }
            }
        }
    }
    int32_t sr[4],tr[4],cr[4];int have_row=pad&&custom_visible(o,tw)&&custom_visible(o,cw)&&custom_rect(o,tw,tr)&&custom_rect(o,cw,cr);
    if(custom_rect(o,selection,sr)) {
        int32_t want[4]={99999,0,sr[2],sr[3]};
        if(have_row) {
            int top=tr[1]<cr[1]?tr[1]:cr[1],bottom=tr[1]+tr[3]>cr[1]+cr[3]?tr[1]+tr[3]:cr[1]+cr[3];
            want[0]=tr[0]-30;want[1]=top-10;want[2]=cr[0]+cr[2]+30-want[0];want[3]=bottom-top+20;
        }
        if(memcmp(sr,want,sizeof want))o->rect(o->opaque,selection,want);
    }
    if(have_row && o->wheel && o->frame) {
        uint64_t rows=custom_find(o,sp,"TouchCustomRows"),view=custom_parent(o,rows),sv=custom_parent(o,view);
        static uint64_t last_wheel;uint64_t f=o->frame(o->opaque);int32_t vr[4],scroll[4];
        if(sv && f-last_wheel>=4 && custom_rect(o,view,vr)&&custom_rect(o,sv,scroll)) {
            int top=(tr[1]<cr[1]?tr[1]:cr[1])-10,bottom=(tr[1]+tr[3]>cr[1]+cr[3]?tr[1]+tr[3]:cr[1]+cr[3])+10;
            int off=-vr[1],height=scroll[3]>0?scroll[3]:1260;if(!selected)top=0;
            if(top<off){last_wheel=f;o->wheel(o->opaque,sv,1);}
            else if(bottom>off+height){last_wheel=f;o->wheel(o->opaque,sv,-1);}
        }
    }
    for(int i=0;i<NK;i++) {
        if(!custom_key_visible(i)){seen[i].w=seen[i].z=0;continue;}
        if(i==K_MOD||i==K_MAP_REVEAL) {
#if defined(DM_ANDROID_HOST) && DM_ANDROID_HOST
            snprintf(name,sizeof name,"TouchCustom_%s",k_keys[i]);uint64_t w=custom_find(o,sp,name);
            if(!custom_visible(o,w))continue;
            int pressed=o->clicked&&o->clicked(o->opaque,w);
            if(pressed&&!custom_visible(o,cm)&&o->action)o->action(o->opaque,w);
            int pd=atomic_load(&g_pend[K_MOD]),shown=pd>=0?pd:atomic_load(&g_val[K_MOD]);char text[256];
            if(!dm_android_custom_status)continue;dm_android_custom_status(i==K_MOD,shown,text,sizeof text);
            snprintf(name,sizeof name,"TouchCustom_%s_Text",k_keys[i]);
            if(o->text)o->text(o->opaque,custom_find(o,sp,name),text);
#endif
            continue;
        }
        if(i==K_RENDER_MODE) {
            uint64_t boxes[4];int values[4],valid=1;
            for(int k=0;k<4;k++){snprintf(name,sizeof name,"TouchCustom_render_mode_%d",k);boxes[k]=custom_find(o,sp,name);values[k]=custom_toggle_value(o,boxes[k]);valid&=custom_visible(o,boxes[k])&&values[k]>=0;}
            if(!valid){seen[i].w=0;continue;}
            int pd=atomic_load(&g_pend[i]),shown=pd>=0?pd:atomic_load(&g_val[i]),to=shown;
            if(seen[i].w==boxes[3]&&seen[i].gen==o->generation)for(int k=0;k<4;k++)if(k!=shown&&values[k]){to=k;break;}
            for(int k=0;k<4;k++)o->toggle(o->opaque,boxes[k],k==to);
            if(to!=shown)atomic_store(&g_pend[i],to);
            seen[i].w=boxes[3];seen[i].gen=o->generation;continue;
        }
        if(i==0) {
            /* 四项语言单选仍使用平台校验过的控件传输。
             * The four language choices still use the platform-validated widget transport. */
            static uint64_t language_boxes[LANG_N];
            uint64_t boxes[LANG_N];int values[LANG_N],valid=1,changed=seen[0].gen!=o->generation;
            for(int k=0;k<LANG_N;k++) {
                boxes[k]=custom_find(o,sp,k_lang_box[k]);values[k]=custom_toggle_value(o,boxes[k]);
                valid&=custom_visible(o,boxes[k])&&values[k]>=0;
                changed|=language_boxes[k]!=boxes[k];
            }
            if(!valid){seen[0].w=0;memset(language_boxes,0,sizeof language_boxes);continue;}
            int pd=atomic_load(&g_pend[0]),shown=pd>=0?pd:atomic_load(&g_val[0]),to=shown;
            if(seen[0].w&&!changed)for(int k=0;k<LANG_N;k++)if(k!=shown&&values[k]){to=k;break;}
            for(int k=0;k<LANG_N;k++)o->toggle(o->opaque,boxes[k],k==to);
            if(to!=shown)atomic_store(&g_pend[0],to);
            memcpy(language_boxes,boxes,sizeof boxes);seen[0].w=boxes[LANG_N-1];seen[0].gen=o->generation;
            continue;
        }
        snprintf(name,sizeof name,"TouchCustom_%s",k_keys[i]);uint64_t w=custom_find(o,sp,name);
        if(!custom_visible(o,w)||custom_toggle_value(o,w)<0){seen[i].w=seen[i].z=0;continue;}
        uint64_t z=rev_box(i)?custom_find(o,sp,rev_box(i)):0;
        if(rev_box(i) && (!custom_visible(o,z)||custom_toggle_value(o,z)<0)){seen[i].w=seen[i].z=0;continue;}
        int pd=atomic_load(&g_pend[i]),shown=pd>=0?pd:atomic_load(&g_val[i]);
        if(seen[i].w!=w || seen[i].z!=z || seen[i].gen!=o->generation) {
            if(o->toggle(o->opaque,w,shown)&&(!z||o->toggle(o->opaque,z,!shown))) {
                seen[i].w=w;seen[i].z=z;seen[i].gen=o->generation;
            }
            continue;
        }
        int on=custom_toggle_value(o,w);if(on<0)continue;
        if(z) {
            int zon=custom_toggle_value(o,z);if(zon<0)continue;
            int to=on!=shown?(on?1:shown):zon!=!shown?(zon?0:shown):shown;
            if(on!=to&&!o->toggle(o->opaque,w,to))continue;
            if(zon!=!to&&!o->toggle(o->opaque,z,!to))continue;
            if(to!=shown){atomic_store(&g_pend[i],to);LOG("自定义设置：%s → %d（点应用生效）",k_keys[i],to);}
        } else if(on!=shown) {
            atomic_store(&g_pend[i],on);LOG("自定义设置：%s 勾选框 → %d（点应用生效）",k_keys[i],on);
        }
    }
    uint64_t label=custom_find(o,sp,"TouchCustom_lang_Text");int32_t lr[4];
    if(custom_rect(o,label,lr)) {
        int pd=atomic_load(&g_pend[0]),shown=pd>=0?pd:atomic_load(&g_val[0]),diff=shown!=lang_running();
        for(int k=0;k<LANG_N;k++) {
            uint64_t hint=custom_find(o,sp,k_lang_hint[k]);int32_t r[4];
            if(custom_rect(o,hint,r)) {int x=diff&&shown==k?lr[0]:99999;if(r[0]!=x){r[0]=x;o->rect(o->opaque,hint,r);}}
        }
    }
    int dirty=0;for(int i=0;i<NK;i++){if(!custom_key_visible(i))continue;int pd=atomic_load(&g_pend[i]);dirty|=pd>=0&&pd!=atomic_load(&g_val[i]);}
    o->dirty(o->opaque,sp,dirty);
}
