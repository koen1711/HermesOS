#ifndef UTILS_SYNC_SPINLOCK_H
#define UTILS_SYNC_SPINLOCK_H

#include <os/stdint.h>

typedef struct spinlock_s {
    volatile uint32_t locked;
    const char* name;
} spinlock_t;

void spinlock_init(spinlock_t* lock, const char* name);
void spinlock_acquire(spinlock_t* lock);
void spinlock_release(spinlock_t* lock);
bool spinlock_try_acquire(spinlock_t* lock);
bool spinlock_held(const spinlock_t* lock);

void spinlock_acquire_irqsave(spinlock_t* lock, uint64_t* flags);
void spinlock_release_irqrestore(spinlock_t* lock, uint64_t flags);

#endif //UTILS_SYNC_SPINLOCK_H
