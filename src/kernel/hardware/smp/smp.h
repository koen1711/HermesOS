#ifndef SMP_SMP_H
#define SMP_SMP_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <hardware/gdt/tss.h>

#define MAX_CPUS 16

struct cpu {
    int index;
    uint32_t apic_id;
    volatile bool online;
    volatile bool tlb_flush;
    tss_t* tss;
};

struct cpu* cpu_this();
struct cpu* cpu_get(int index);
int cpu_count();

bool kernel_lock_enter();
void kernel_lock_exit();
void kernel_lock_release_for_new_process();

void smp_initialize();
void smp_tlb_shootdown();
void smp_tlb_interrupt();

#endif //SMP_SMP_H
