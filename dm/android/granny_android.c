#include "../host/dm.h"

/* These calls replace the two reviewed BitKnit2 streaming call sites.
 * The decoder still consumes the original chunks and context. No resource
 * or save is synthesized by the Android adapter. */
int dm_android_granny_call(int number, uint64_t *registers) {
    if ((number != 0x83 && number != 0x84) || !registers) return 0;
    module_t *main=NULL;
    for (int i=0;i<g_nmod;++i)
        if (!strcmp(g_mod[i].name,"main")) { main=&g_mod[i]; break; }
    if (!main || !main->base || strcmp(main->build_id,"db0507f36b84200ac05f75d76ed5026f") ||
        main->text_end < 0x11f4c74 || main->size < 0x11f4c74) return 0;
    if (number == 0x83) {
        void svc_granny_chunk_a(uint64_t *); svc_granny_chunk_a(registers);
    } else {
        void svc_granny_chunk_b(uint64_t *); svc_granny_chunk_b(registers);
    }
    return 1;
}
