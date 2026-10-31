#include <drivers/timer/clock.h>

#include <drivers/timer/callout.h>
#include <drivers/timer/rtc.h>
#include <drivers/timer/tsc.h>
#include <hardware/smp/smp.h>
#include <os/errno.h>

#define NS_PER_SEC 1000000000ULL

static uint64_t boot_epoch_ns;
static bool boot_epoch_known;

static int64_t days_from_civil(int64_t y, const unsigned m, const unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static uint64_t realtime_ns()
{
    if (!__atomic_load_n(&boot_epoch_known, __ATOMIC_ACQUIRE)) {
        const bool locked = kernel_lock_enter();
        if (boot_epoch_known) {
            if (locked)
                kernel_lock_exit();
            return boot_epoch_ns + clock_monotonic_ns();
        }
        const rtc_datetime now = rtc_get_datetime();
        const int64_t days = days_from_civil(2000 + now.date.year, now.date.month, now.date.day);
        const uint64_t secs = (uint64_t)days * 86400 + now.time.hours * 3600 + now.time.minutes * 60
                              + now.time.seconds;
        boot_epoch_ns = secs * NS_PER_SEC - clock_monotonic_ns();
        __atomic_store_n(&boot_epoch_known, true, __ATOMIC_RELEASE);
        if (locked)
            kernel_lock_exit();
    }
    return boot_epoch_ns + clock_monotonic_ns();
}

void clock_set_realtime_ns(const uint64_t ns)
{
    boot_epoch_ns = ns - clock_monotonic_ns();
    boot_epoch_known = true;
}

uint64_t clock_monotonic_ns()
{
    return tsc_micros() * 1000;
}

int clock_get_ns(const int clock, uint64_t* ns)
{
    switch (clock) {
    case CLOCK_REALTIME_K:
    case CLOCK_REALTIME_COARSE_K:
        *ns = realtime_ns();
        return 0;
    case CLOCK_MONOTONIC_K:
    case CLOCK_MONOTONIC_RAW_K:
    case CLOCK_MONOTONIC_COARSE_K:
    case CLOCK_BOOTTIME_K:
    case CLOCK_PROCESS_CPUTIME_ID_K:
    case CLOCK_THREAD_CPUTIME_ID_K:
        *ns = clock_monotonic_ns();
        return 0;
    default:
        return -EINVAL;
    }
}

int64_t clock_ns_until(const int clock, const struct timespec_k* deadline)
{
    uint64_t now;
    if (clock_get_ns(clock, &now) < 0)
        return 0;
    const uint64_t target = (uint64_t)deadline->tv_sec * NS_PER_SEC + (uint64_t)deadline->tv_nsec;
    return (int64_t)(target - now);
}

uint64_t clock_ticks_from_ns(const uint64_t ns)
{
    const uint64_t hz = ticks_hz();
    const uint64_t ticks = ns / NS_PER_SEC * hz + (ns % NS_PER_SEC * hz + NS_PER_SEC - 1) / NS_PER_SEC;
    return ticks ? ticks : 1;
}

uint64_t clock_ticks_from_timespec(const struct timespec_k* ts)
{
    const uint64_t sec = ts->tv_sec > 1000000000 ? 1000000000 : (uint64_t)ts->tv_sec;
    return clock_ticks_from_ns(sec * NS_PER_SEC + (uint64_t)ts->tv_nsec);
}
