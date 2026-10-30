#include "futex.h"

#include <drivers/scheduler/process.h>
#include <drivers/syscalls/uaccess.h>
#include <drivers/timer/clock.h>
#include <os/errno.h>

enum futex_op {
    FUTEX_WAIT = 0,
    FUTEX_WAKE = 1,
    FUTEX_REQUEUE = 3,
    FUTEX_CMP_REQUEUE = 4,
    FUTEX_WAIT_BITSET = 9,
    FUTEX_WAKE_BITSET = 10,
    FUTEX_PRIVATE_FLAG = 128,
    FUTEX_CLOCK_REALTIME = 256
};

static long futex_key(struct vm_space* vm, const uint64_t uaddr, int* key)
{
    if (uaddr & 3)
        return -EINVAL;
    if (!vm_populate(vm, uaddr, sizeof(uint32_t), false))
        return -EFAULT;
    spinlock_acquire(&vm->lock);
    const struct vm_area* a = vm_find(vm, uaddr);
    const bool shared = a && (a->flags & VMA_SHARED);
    spinlock_release(&vm->lock);
    const uint64_t id = shared ? vm_translate(vm, uaddr) >> 2 : (uaddr >> 2) ^ ((uint64_t)vm >> 3);
    *key = WAIT_CHANNEL_FUTEX | (int)(id & 0x0FFFFFFF);
    return 0;
}

long futex_wait(struct vm_space* vm, const uint64_t uaddr, const uint32_t expected, const uint64_t timeout)
{
    int key;
    const long err = futex_key(vm, uaddr, &key);
    if (err)
        return err;
    if (*(volatile uint32_t*)phys_to_virt(vm_translate(vm, uaddr)) != expected)
        return -EAGAIN;
    return process_sleep_timeout(key, timeout);
}

long futex_wake(struct vm_space* vm, const uint64_t uaddr, const int count)
{
    int key;
    const long err = futex_key(vm, uaddr, &key);
    if (err)
        return err;
    return process_wakeup_n(key, count);
}

long sys_futex(uint32_t* uaddr, const int op, const uint32_t val, const void* timeout, uint32_t* uaddr2,
               const uint32_t val3)
{
    (void)uaddr2;
    (void)val3;
    process_t* current = scheduler_get()->current;
    const int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        uint64_t ticks = 0;
        if (timeout) {
            struct timespec_k ts;
            if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
                return -EFAULT;
            if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 || ts.tv_sec < 0)
                return -EINVAL;
            if (cmd == FUTEX_WAIT_BITSET) {
                const int clock = op & FUTEX_CLOCK_REALTIME ? CLOCK_REALTIME_K : CLOCK_MONOTONIC_K;
                const int64_t left = clock_ns_until(clock, &ts);
                if (left <= 0)
                    return -ETIMEDOUT;
                ticks = clock_ticks_from_ns((uint64_t)left);
            } else {
                ticks = clock_ticks_from_timespec(&ts);
            }
        }
        return futex_wait(current->vm, (uint64_t)uaddr, val, ticks);
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return futex_wake(current->vm, (uint64_t)uaddr, (int)(val > 0x7FFFFFFF ? 0x7FFFFFFF : val));
    default:
        return -ENOSYS;
    }
}
