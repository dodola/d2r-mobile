#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nvn_platform_clock.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

uint64_t dm_android_nvn_clock_ns(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)) { fprintf(stderr,"[nvn-android] CLOCK_MONOTONIC failed errno=%d\n",errno);__builtin_trap(); }
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
uint64_t dm_android_nvn_thread_cpu_ns(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)) { fprintf(stderr,"[nvn-android] thread CPU clock failed errno=%d\n",errno);__builtin_trap(); }
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
void dm_android_nvn_sleep_until(uint64_t deadline) {
    struct timespec t={(time_t)(deadline/1000000000ULL),(long)(deadline%1000000000ULL)};
    int error;do {error=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL);}while(error==EINTR);
    if(error) { fprintf(stderr,"[nvn-android] absolute sleep failed errno=%d\n",error);__builtin_trap(); }
}
void dm_android_nvn_schedule_thread(const char *role) {
    /* An unprivileged Android application has no Darwin real-time/QoS API.
     * Request a modest native nice priority, then report the actual scheduler.
     * Permission failure is a measured state, never reported as RT success.
     */
    static _Thread_local int done;
    if(done)return;done=1;
    pid_t tid=(pid_t)syscall(SYS_gettid);
    int error=0;if(setpriority(PRIO_PROCESS,(id_t)tid,-2))error=errno;
    errno=0;int nice=getpriority(PRIO_PROCESS,(id_t)tid);int nice_error=errno;
    int policy=0;struct sched_param parameter={0};int query=pthread_getschedparam(pthread_self(),&policy,&parameter);
    fprintf(stderr,"[nvn-android] %s tid=%d requested_nice=-2 actual_nice=%d nice_errno=%d set_errno=%d policy=%d priority=%d sched_query=%d; Darwin hardware instruction/cycle statistics unavailable\n",
            role,(int)tid,nice,nice_error,error,policy,parameter.sched_priority,query);
}
