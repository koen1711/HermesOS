#include "sys.h"

#include "uaccess.h"
#include <drivers/scheduler/process.h>
#include <drivers/scheduler/signal.h>
#include <drivers/timer/callout.h>
#include <drivers/timer/clock.h>
#include <os/errno.h>

#define NS_PER_SEC 1000000000ULL
#define TIMER_ABSTIME 1
#define ITIMER_REAL 0
#define CLK_TCK 100

struct itimerval_k {
    struct timeval_k it_interval;
    struct timeval_k it_value;
};

struct tms_k {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

static struct timespec_k ns_to_timespec(const uint64_t ns)
{
    return (struct timespec_k){ .tv_sec = (int64_t)(ns / NS_PER_SEC), .tv_nsec = (int64_t)(ns % NS_PER_SEC) };
}

long sys_clock_gettime(const int clock, struct timespec_k* uts)
{
    uint64_t ns;
    const int err = clock_get_ns(clock, &ns);
    if (err)
        return err;
    const struct timespec_k ts = ns_to_timespec(ns);
    return copy_to_user(uts, &ts, sizeof(ts));
}

long sys_clock_getres(const int clock, struct timespec_k* uts)
{
    uint64_t ns;
    const int err = clock_get_ns(clock, &ns);
    if (err)
        return err;
    const struct timespec_k res = { .tv_sec = 0, .tv_nsec = 1000 };
    return uts ? copy_to_user(uts, &res, sizeof(res)) : 0;
}

long sys_gettimeofday(struct timeval_k* utv, void* tz)
{
    (void)tz;
    if (utv == nullptr)
        return 0;
    uint64_t ns;
    clock_get_ns(CLOCK_REALTIME_K, &ns);
    const struct timeval_k tv = { .tv_sec = (int64_t)(ns / NS_PER_SEC), .tv_usec = (int64_t)(ns % NS_PER_SEC / 1000) };
    return copy_to_user(utv, &tv, sizeof(tv));
}

long sys_time(int64_t* ut)
{
    uint64_t ns;
    clock_get_ns(CLOCK_REALTIME_K, &ns);
    const int64_t secs = (int64_t)(ns / NS_PER_SEC);
    if (ut && copy_to_user(ut, &secs, sizeof(secs)) < 0)
        return -EFAULT;
    return secs;
}

static long sleep_until(const int clock, const uint64_t deadline, struct timespec_k* urem)
{
    process_t* current = scheduler_get()->current;
    for (;;) {
        uint64_t now;
        clock_get_ns(clock, &now);
        if (now >= deadline)
            return 0;
        const int err = process_sleep_timeout(WAIT_CHANNEL_TIMER | current->pid, clock_ticks_from_ns(deadline - now));
        if (err == -EINTR) {
            clock_get_ns(clock, &now);
            const struct timespec_k rem = ns_to_timespec(deadline > now ? deadline - now : 0);
            if (urem && copy_to_user(urem, &rem, sizeof(rem)) < 0)
                return -EFAULT;
            return -EINTR;
        }
    }
}

static long timespec_in(const struct timespec_k* uts, uint64_t* ns)
{
    struct timespec_k ts;
    if (copy_from_user(&ts, uts, sizeof(ts)) < 0)
        return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= (int64_t)NS_PER_SEC)
        return -EINVAL;
    const uint64_t sec = ts.tv_sec > 1000000000 ? 1000000000 : (uint64_t)ts.tv_sec;
    *ns = sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
    return 0;
}

long sys_nanosleep(const struct timespec_k* req, struct timespec_k* rem)
{
    return sys_clock_nanosleep(CLOCK_MONOTONIC_K, 0, req, rem);
}

long sys_clock_nanosleep(const int clock, const int flags, const struct timespec_k* req, struct timespec_k* rem)
{
    uint64_t ns;
    long err = timespec_in(req, &ns);
    if (err)
        return err;
    uint64_t now;
    if ((err = clock_get_ns(clock, &now)) != 0)
        return err;
    if (ns == 0 && !(flags & TIMER_ABSTIME)) {
        process_yield();
        return 0;
    }
    return sleep_until(clock, flags & TIMER_ABSTIME ? ns : now + ns, flags & TIMER_ABSTIME ? nullptr : rem);
}

static void itimer_fire(void* arg)
{
    process_t* proc = arg;
    signal_send(proc->tgid, SIGALRM);
    if (proc->itimer_interval)
        callout_reset(&proc->itimer, proc->itimer_interval, itimer_fire, proc);
}

static uint64_t ticks_from_timeval(const struct timeval_k* tv)
{
    if (tv->tv_sec == 0 && tv->tv_usec == 0)
        return 0;
    const uint64_t sec = tv->tv_sec > 1000000000 ? 1000000000 : (uint64_t)tv->tv_sec;
    return clock_ticks_from_ns(sec * NS_PER_SEC + (uint64_t)tv->tv_usec * 1000);
}

static struct timeval_k timeval_from_ticks(const uint64_t ticks)
{
    const uint64_t us = ticks * 1000000 / ticks_hz();
    return (struct timeval_k){ .tv_sec = (int64_t)(us / 1000000), .tv_usec = (int64_t)(us % 1000000) };
}

static struct itimerval_k itimer_get(const process_t* proc)
{
    struct itimerval_k v = { .it_interval = timeval_from_ticks(proc->itimer_interval) };
    if (callout_pending(&proc->itimer)) {
        const uint64_t now = ticks_get();
        v.it_value = timeval_from_ticks(proc->itimer.expires > now ? proc->itimer.expires - now : 1);
    }
    return v;
}

long sys_setitimer(const int which, const void* unew, void* uold)
{
    if (which != ITIMER_REAL)
        return -EINVAL;
    process_t* current = scheduler_get()->current;
    struct itimerval_k next = {0};
    if (unew && copy_from_user(&next, unew, sizeof(next)) < 0)
        return -EFAULT;
    if (next.it_value.tv_usec < 0 || next.it_value.tv_usec >= 1000000 || next.it_interval.tv_usec < 0
        || next.it_interval.tv_usec >= 1000000 || next.it_value.tv_sec < 0 || next.it_interval.tv_sec < 0)
        return -EINVAL;

    const struct itimerval_k old = itimer_get(current);
    if (uold && copy_to_user(uold, &old, sizeof(old)) < 0)
        return -EFAULT;

    callout_drain(&current->itimer);
    current->itimer_interval = ticks_from_timeval(&next.it_interval);
    const uint64_t value = ticks_from_timeval(&next.it_value);
    if (value)
        callout_reset(&current->itimer, value, itimer_fire, current);
    return 0;
}

long sys_getitimer(const int which, void* uvalue)
{
    if (which != ITIMER_REAL)
        return -EINVAL;
    const struct itimerval_k v = itimer_get(scheduler_get()->current);
    return copy_to_user(uvalue, &v, sizeof(v));
}

long sys_alarm(const unsigned int seconds)
{
    process_t* current = scheduler_get()->current;
    const struct itimerval_k old = itimer_get(current);
    callout_drain(&current->itimer);
    current->itimer_interval = 0;
    if (seconds)
        callout_reset(&current->itimer, (uint64_t)seconds * ticks_hz(), itimer_fire, current);
    return old.it_value.tv_sec + (old.it_value.tv_usec >= 500000 || (!old.it_value.tv_sec && old.it_value.tv_usec));
}

long sys_times(void* ubuf)
{
    const struct tms_k t = {0};
    if (ubuf && copy_to_user(ubuf, &t, sizeof(t)) < 0)
        return -EFAULT;
    return (long)(clock_monotonic_ns() / (NS_PER_SEC / CLK_TCK));
}
