/* 验证同址多版本、跨地址复用、哈希碰撞、对齐、容量回退和退役后复用。
 * Check versions at one address, reuse across addresses, hash collisions, alignment, capacity fallback, and reuse after retirement.
 */
#include "../nvn/nvn_vk_uniform_arena.h"
#include "../nvn/nvn_vk_publication_queue.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static NvnUniformArena arena;
static NvnVkPublicationQueue queue=NVN_VK_PUBLICATION_QUEUE_INIT;
static unsigned char storage[64],overflow[64];
static void *complete(void *unused) {
    (void)unused;NvnVkPublicationJob job=nvn_vk_publication_front(&queue);
    assert(storage[0]==1&&storage[16]==2);
    assert(nvn_vk_publication_finish(&queue,job));return NULL;
}
int main(void) {
    unsigned char bytes[16];memset(bytes,1,sizeof bytes);NvnUniformRef first,second,copy;
    assert(!nvn_uniform_intern(&arena,bytes,16,16,7,1,&first));
    assert(nvn_uniform_add_block(&arena,storage,storage,sizeof storage));
    assert(nvn_uniform_intern(&arena,bytes,16,16,7,1,&first)==1&&first.offset==0);
    memset(bytes,2,sizeof bytes);
    assert(nvn_uniform_intern(&arena,bytes,16,16,7,1,&second)==1&&second.offset==16);
    assert(first.bytes[first.offset]==1);
    unsigned char elsewhere[16];memset(elsewhere,1,sizeof elsewhere);
    assert(nvn_uniform_intern(&arena,elsewhere,16,16,7,1,&copy)==2&&copy.offset==first.offset);
    assert(nvn_uniform_intern(&arena,bytes,16,32,8,0,&copy)==1&&copy.offset==32);
    assert(!nvn_uniform_intern(&arena,bytes,16,32,9,0,&copy));
    assert(nvn_uniform_add_block(&arena,overflow,overflow,sizeof overflow));
    assert(nvn_uniform_intern(&arena,bytes,16,32,9,0,&copy)==1&&copy.owner==overflow);
    assert(storage[0]==1&&storage[16]==2);
    pthread_t worker;assert(!pthread_create(&worker,NULL,complete,NULL));
    assert(nvn_vk_publication_push(&queue,(NvnVkPublicationJob){&arena,1,0}));
    nvn_vk_publication_wait(&queue,1);pthread_join(worker,NULL);
    nvn_uniform_reset(&arena);assert(nvn_uniform_add_block(&arena,storage,storage,sizeof storage));
    memset(bytes,3,sizeof bytes);assert(nvn_uniform_intern(&arena,bytes,16,16,7,1,&copy)==1&&copy.offset==0);
    for(unsigned i=1;i<NVN_UNIFORM_BLOCKS;i++)assert(nvn_uniform_add_block(&arena,overflow,overflow,sizeof overflow));
    assert(!nvn_uniform_add_block(&arena,overflow,overflow,sizeof overflow));
    puts("UNIFORM_ARENA PASS versions content_reuse collisions alignment overflow retirement");return 0;
}
