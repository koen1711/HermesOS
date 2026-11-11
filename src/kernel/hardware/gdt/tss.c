#include "tss.h"

#define TSS_TYPE_AVAILABLE_64 0x89

extern uint64_t gdt_start[];

tss_t task_state_segment;

#ifdef HERMES_UNIKERNEL
#include <hardware/smp/smp.h>

#define IST_TRAP_SIZE  4096
#define IST_FATAL_SIZE 16384

static uint8_t ist_trap_stacks[MAX_CPUS][IST_TRAP_SIZE] __attribute__((aligned(16)));
static uint8_t ist_fatal_stacks[MAX_CPUS][IST_FATAL_SIZE] __attribute__((aligned(16)));

void tss_install_ist(tss_t* tss, const int cpu_index)
{
    tss->ist2 = (uint64_t)&ist_trap_stacks[cpu_index][IST_TRAP_SIZE];
    tss->ist3 = (uint64_t)&ist_fatal_stacks[cpu_index][IST_FATAL_SIZE];
}
#endif

void tss_initialize() {
    const uint64_t base = (uint64_t)&task_state_segment;
    constexpr uint64_t limit = sizeof(tss_t) - 1;


    task_state_segment.iopb = sizeof(tss_t);
#ifdef HERMES_UNIKERNEL
    tss_install_ist(&task_state_segment, 0);
#endif

    gdt_start[TSS_GDT_INDEX] = limit & 0xFFFF
                             | (base & 0xFFFFFF) << 16
                             | (uint64_t)TSS_TYPE_AVAILABLE_64 << 40
                             | (limit >> 16 & 0xF) << 48
                             | (base >> 24 & 0xFF) << 56;
    gdt_start[TSS_GDT_INDEX + 1] = base >> 32;

    asm volatile("ltr %w0" : : "r"((uint16_t)TSS_SELECTOR));
}
