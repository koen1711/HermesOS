#include <utils/sync/spinlock.h>

#include <hardware/interrupts/irq.h>
#include <hardware/smp/smp.h>

void spinlock_init(spinlock_t* lock, const char* name)
{
    lock->locked = 0;
    lock->name = name;
}

void spinlock_acquire(spinlock_t* lock)
{
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
            smp_spin_poll();
            asm volatile("pause");
        }
    }
}

void spinlock_release(spinlock_t* lock)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

bool spinlock_try_acquire(spinlock_t* lock)
{
    return !__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE);
}

bool spinlock_held(const spinlock_t* lock)
{
    return __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0;
}

void spinlock_acquire_irqsave(spinlock_t* lock, uint64_t* flags)
{
    *flags = irq_save();
    spinlock_acquire(lock);
}

void spinlock_release_irqrestore(spinlock_t* lock, const uint64_t flags)
{
    spinlock_release(lock);
    irq_restore(flags);
}
