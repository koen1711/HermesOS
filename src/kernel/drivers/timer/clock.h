#ifndef DRIVERS_TIMER_CLOCK_H
#define DRIVERS_TIMER_CLOCK_H

#include <os/stdint.h>

enum clock_id_k {
    CLOCK_REALTIME_K = 0,
    CLOCK_MONOTONIC_K = 1,
    CLOCK_PROCESS_CPUTIME_ID_K = 2,
    CLOCK_THREAD_CPUTIME_ID_K = 3,
    CLOCK_MONOTONIC_RAW_K = 4,
    CLOCK_REALTIME_COARSE_K = 5,
    CLOCK_MONOTONIC_COARSE_K = 6,
    CLOCK_BOOTTIME_K = 7
};

struct timespec_k {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct timeval_k {
    int64_t tv_sec;
    int64_t tv_usec;
};

uint64_t clock_monotonic_ns();
int clock_get_ns(int clock, uint64_t* ns);
int64_t clock_ns_until(int clock, const struct timespec_k* deadline);
uint64_t clock_ticks_from_ns(uint64_t ns);
uint64_t clock_ticks_from_timespec(const struct timespec_k* ts);

#endif //DRIVERS_TIMER_CLOCK_H
