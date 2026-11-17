#include <utils/sync/brlock.h>

#include <hardware/interrupts/irq.h>

void brlock_init(brlock_t* lock, const char* name)
{
    for (int i = 0; i < MAX_CPUS; i++)
        lock->slot[i].locked = 0;
    lock->name = name;
}

static void slot_acquire(volatile uint32_t* locked)
{
    while (__atomic_exchange_n(locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(locked, __ATOMIC_RELAXED)) {
            smp_spin_poll();
            asm volatile("pause");
        }
    }
}

void brlock_read_acquire(brlock_t* lock, uint64_t* flags)
{
    *flags = irq_save();
    slot_acquire(&lock->slot[cpu_this()->index].locked);
}

void brlock_read_release(brlock_t* lock, const uint64_t flags)
{
    __atomic_store_n(&lock->slot[cpu_this()->index].locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

void brlock_write_acquire(brlock_t* lock, uint64_t* flags)
{
    *flags = irq_save();
    for (int i = 0; i < MAX_CPUS; i++)
        slot_acquire(&lock->slot[i].locked);
}

void brlock_write_release(brlock_t* lock, const uint64_t flags)
{
    for (int i = MAX_CPUS - 1; i >= 0; i--)
        __atomic_store_n(&lock->slot[i].locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}
