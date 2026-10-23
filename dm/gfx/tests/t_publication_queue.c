/* 用真实线程验证发布不依赖录制锁、FIFO、槽复用和完成后的可见性。
 * Real-thread checks for publication without the recorder lock, FIFO, slot reuse, and visibility after completion.
 */
#include "../nvn/nvn_vk_publication_queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>

static NvnVkPublicationQueue queue=NVN_VK_PUBLICATION_QUEUE_INIT;
static pthread_mutex_t recorder=PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t input,output; } slots[4];
static atomic_int effects;
static void *publisher(void *unused) {
    (void)unused;
    for(unsigned n=0;n<1000;n++){
        NvnVkPublicationJob job=nvn_vk_publication_front(&queue);
        unsigned i=(unsigned)job.kind;assert(i<4&&job.context==&slots[i]);
        assert(job.ticket==n+1&&slots[i].input==job.ticket);
        slots[i].output=slots[i].input*3;
        atomic_fetch_add(&effects,1);
        assert(nvn_vk_publication_finish(&queue,job));
    }
    return NULL;
}
int main(void) {
    pthread_t worker;assert(!pthread_create(&worker,NULL,publisher,NULL));
    /* 整轮持有录制锁，发布仍必须完成；每四个任务复用同一槽。
     * Hold the recorder lock throughout; publication must progress and slots are reused every four jobs.
     */
    pthread_mutex_lock(&recorder);
    for(unsigned batch=0;batch<250;batch++){
        for(unsigned i=0;i<4;i++){
            uint64_t ticket=batch*4+i+1;slots[i].input=ticket;
            assert(nvn_vk_publication_push(&queue,(NvnVkPublicationJob){&slots[i],ticket,(int)i}));
        }
        nvn_vk_publication_wait(&queue,batch*4+4);
        for(unsigned i=0;i<4;i++)assert(slots[i].output==(batch*4+i+1)*3);
    }
    nvn_vk_publication_drain(&queue);assert(!nvn_vk_publication_pending(&queue));
    assert(atomic_load(&effects)==1000);pthread_mutex_unlock(&recorder);pthread_join(worker,NULL);
    /* 拒绝乱序完成和溢出，失败不能丢掉队首任务。
     * Reject out-of-order completion and overflow without dropping the head job.
     */
    for(unsigned i=0;i<8;i++)assert(nvn_vk_publication_push(&queue,(NvnVkPublicationJob){&slots[0],1001+i,0}));
    assert(!nvn_vk_publication_push(&queue,(NvnVkPublicationJob){&slots[0],1009,0}));
    assert(!nvn_vk_publication_finish(&queue,(NvnVkPublicationJob){&slots[0],1002,0}));
    for(unsigned i=0;i<8;i++)assert(nvn_vk_publication_finish(&queue,nvn_vk_publication_front(&queue)));
    assert(!nvn_vk_publication_pending(&queue));puts("PUBLICATION_QUEUE PASS jobs=1000 slots=4 FIFO overflow visibility recorder_overlap");return 0;
}
