/* 独立发布队列：只保护任务和已发布票号，不持有渲染器资源锁。
 * Independent publication queue: owns jobs and published tickets, never the renderer resource lock.
 */
#ifndef NVN_VK_PUBLICATION_QUEUE_H
#define NVN_VK_PUBLICATION_QUEUE_H
#include <pthread.h>
#include <stdint.h>

typedef struct { void *context; uint64_t ticket; int kind; } NvnVkPublicationJob;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready,done;
    NvnVkPublicationJob jobs[8];
    unsigned head,count;
    uint64_t published;
} NvnVkPublicationQueue;
#define NVN_VK_PUBLICATION_QUEUE_INIT { .mutex=PTHREAD_MUTEX_INITIALIZER, .ready=PTHREAD_COND_INITIALIZER, .done=PTHREAD_COND_INITIALIZER }

static inline int nvn_vk_publication_push(NvnVkPublicationQueue *q,NvnVkPublicationJob job) {
    pthread_mutex_lock(&q->mutex);
    if(q->count==8){pthread_mutex_unlock(&q->mutex);return 0;}
    q->jobs[(q->head+q->count)%8]=job;q->count++;
    pthread_cond_signal(&q->ready);pthread_mutex_unlock(&q->mutex);return 1;
}
static inline NvnVkPublicationJob nvn_vk_publication_front(NvnVkPublicationQueue *q) {
    pthread_mutex_lock(&q->mutex);
    while(!q->count)pthread_cond_wait(&q->ready,&q->mutex);
    NvnVkPublicationJob job=q->jobs[q->head];pthread_mutex_unlock(&q->mutex);return job;
}
/* 单个发布者在所有客体效果落地后调用；失败时不推进队列。
 * The single publisher calls this after all guest effects; failure never advances the queue.
 */
static inline int nvn_vk_publication_finish(NvnVkPublicationQueue *q,NvnVkPublicationJob job) {
    pthread_mutex_lock(&q->mutex);
    if(!q->count||q->jobs[q->head].context!=job.context||q->jobs[q->head].ticket!=job.ticket||job.ticket<=q->published){
        pthread_mutex_unlock(&q->mutex);return 0;
    }
    q->published=job.ticket;q->head=(q->head+1)%8;q->count--;
    pthread_cond_broadcast(&q->done);pthread_mutex_unlock(&q->mutex);return 1;
}
static inline void nvn_vk_publication_wait(NvnVkPublicationQueue *q,uint64_t ticket) {
    pthread_mutex_lock(&q->mutex);
    while(q->published<ticket)pthread_cond_wait(&q->done,&q->mutex);
    pthread_mutex_unlock(&q->mutex);
}
static inline void nvn_vk_publication_drain(NvnVkPublicationQueue *q) {
    pthread_mutex_lock(&q->mutex);
    while(q->count)pthread_cond_wait(&q->done,&q->mutex);
    pthread_mutex_unlock(&q->mutex);
}
static inline unsigned nvn_vk_publication_pending(NvnVkPublicationQueue *q) {
    pthread_mutex_lock(&q->mutex);unsigned count=q->count;pthread_mutex_unlock(&q->mutex);return count;
}
#endif
