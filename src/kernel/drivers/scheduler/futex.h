#ifndef SCHEDULER_FUTEX_H
#define SCHEDULER_FUTEX_H

#include <os/stdint.h>
#include <hardware/memory/vm.h>

long futex_wait(struct vm_space* vm, uint64_t uaddr, uint32_t expected, uint64_t timeout);
long futex_wake(struct vm_space* vm, uint64_t uaddr, int count);

long sys_futex(uint32_t* uaddr, int op, uint32_t val, const void* timeout, uint32_t* uaddr2, uint32_t val3);

#endif //SCHEDULER_FUTEX_H
