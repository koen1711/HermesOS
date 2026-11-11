#ifndef INTERRUPTS_MODE_H
#define INTERRUPTS_MODE_H

#include <os/stdbool.h>
#include <os/stdint.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/memory/vm.h>

#define SYSCALL_FRAME_KCALL 1

#ifdef HERMES_UNIKERNEL
#include <uapi/hermes/unikernel.h>

#define WORKLOAD_CS 0x08
#define WORKLOAD_SS 0x10

extern void kcall_entry();

static inline bool frame_from_workload(const interrupt_context* context)
{
    return (context->rip >= USER_ADDR_MIN && context->rip < USER_ADDR_LIMIT)
           || context->rip == (uint64_t)kcall_entry;
}
#else
#define WORKLOAD_CS (0x20 | 3)
#define WORKLOAD_SS (0x18 | 3)

static inline bool frame_from_workload(const interrupt_context* context)
{
    return (context->cs & 3) == 3;
}
#endif

#endif //INTERRUPTS_MODE_H
