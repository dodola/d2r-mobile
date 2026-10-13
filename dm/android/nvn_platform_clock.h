#pragma once
#include <stdint.h>
uint64_t dm_android_nvn_clock_ns(void);
uint64_t dm_android_nvn_thread_cpu_ns(void);
void dm_android_nvn_schedule_thread(const char *role);
void dm_android_nvn_sleep_until(uint64_t deadline_ns);
void dm_android_nvn_presented(uint64_t frame);
