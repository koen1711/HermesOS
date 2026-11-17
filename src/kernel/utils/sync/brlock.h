#ifndef UTILS_SYNC_BRLOCK_H
#define UTILS_SYNC_BRLOCK_H

#include <os/stdint.h>
#include <hardware/smp/smp.h>

typedef struct brlock_s {
    struct {
        volatile uint32_t locked;
    } __attribute__((aligned(64))) slot[MAX_CPUS];
    const char* name;
} brlock_t;

void brlock_init(brlock_t* lock, const char* name);
void brlock_read_acquire(brlock_t* lock, uint64_t* flags);
void brlock_read_release(brlock_t* lock, uint64_t flags);
void brlock_write_acquire(brlock_t* lock, uint64_t* flags);
void brlock_write_release(brlock_t* lock, uint64_t flags);

#endif //UTILS_SYNC_BRLOCK_H
