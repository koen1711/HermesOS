#include "smp.h"

#include "acpi.h"
#include "lapic.h"
#include <hardware/gdt/tss.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/interrupts/irq.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <drivers/scheduler/process.h>
#include <drivers/syscalls/syscalls.h>
#include <drivers/terminal/terminal.h>
#include <drivers/timer/tsc.h>

#define AP_TRAMPOLINE_BASE 0x8000
#define AP_STARTUP_TIMEOUT_US 200000
#define AP_TIMER_HZ 1000

#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

extern uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern uint64_t ap_trampoline_cr3[], ap_trampoline_stack[], ap_trampoline_entry[];
extern uint64_t gdt_start[];
extern void irq_lapic_timer(), irq_tlb_shootdown(), irq_resched(), irq_lapic_spurious();

static struct cpu cpus[MAX_CPUS] = { [0] = { .self = &cpus[0], .index = 0, .online = true, .tss = &task_state_segment } };
static int ncpus = 1;

static volatile uint32_t kernel_lock;
static volatile int kernel_lock_owner = -1;

static struct {
    uint16_t size;
    uint64_t address;
} __attribute__((packed)) kernel_gdtr, kernel_idtr;

static void wrmsr(const uint32_t msr, const uint64_t value)
{
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static void cpu_install(struct cpu* cpu)
{
    cpu->self = cpu;
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

struct cpu* cpu_this()
{
    struct cpu* cpu;
    asm volatile("mov %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

struct cpu* cpu_get(const int index)
{
    return index >= 0 && index < ncpus ? &cpus[index] : nullptr;
}

int cpu_count()
{
    return ncpus;
}

static void tlb_flush_local()
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}

static void tlb_poll(struct cpu* cpu)
{
    if (cpu->tlb_flush) {
        tlb_flush_local();
        __atomic_store_n(&cpu->tlb_flush, false, __ATOMIC_RELEASE);
    }
}

bool kernel_lock_enter()
{
    struct cpu* cpu = cpu_this();
    if (kernel_lock_owner == cpu->index)
        return false;

    while (__atomic_exchange_n(&kernel_lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&kernel_lock, __ATOMIC_RELAXED)) {
            tlb_poll(cpu);
            asm volatile("pause");
        }
    }
    kernel_lock_owner = cpu->index;
    return true;
}

void kernel_lock_exit()
{
    kernel_lock_owner = -1;
    __atomic_store_n(&kernel_lock, 0, __ATOMIC_RELEASE);
}

bool kernel_lock_drop()
{
    if (kernel_lock_owner != cpu_this()->index)
        return false;
    kernel_lock_exit();
    return true;
}

void smp_kick(const int index)
{
    if (ncpus > 1 && index >= 0 && index < ncpus && cpus[index].online)
        lapic_send_ipi(cpus[index].apic_id, RESCHED_VECTOR);
}

void smp_spin_poll()
{
    if (ncpus > 1)
        tlb_poll(cpu_this());
}

void smp_tlb_shootdown()
{
    if (ncpus == 1)
        return;
    const struct cpu* self = cpu_this();
    for (int i = 0; i < ncpus; i++) {
        if (&cpus[i] != self && cpus[i].online)
            __atomic_store_n(&cpus[i].tlb_flush, true, __ATOMIC_RELEASE);
    }
    lapic_send_others(TLB_SHOOTDOWN_VECTOR);
    for (int i = 0; i < ncpus; i++) {
        while (__atomic_load_n(&cpus[i].tlb_flush, __ATOMIC_ACQUIRE)) {
            smp_spin_poll();
            asm volatile("pause");
        }
    }
}

void smp_tlb_interrupt()
{
    tlb_poll(cpu_this());
    lapic_eoi();
}

static void install_tss(const int index, tss_t* tss)
{
    const uint64_t base = (uint64_t)tss;
    constexpr uint64_t limit = sizeof(tss_t) - 1;
    const int slot = TSS_GDT_INDEX + 2 * index;
    tss->iopb = sizeof(tss_t);
    gdt_start[slot] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) | (((limit >> 16) & 0xF) << 48)
                      | (((base >> 24) & 0xFF) << 56);
    gdt_start[slot + 1] = base >> 32;
    asm volatile("ltr %w0" : : "r"((uint16_t)(slot * 8)));
}

static volatile int booting_index;

static void ap_main()
{
    struct cpu* cpu = &cpus[booting_index];
    cpu_install(cpu);

    asm volatile("lgdt %0" : : "m"(kernel_gdtr));
    asm volatile("pushq $0x08; leaq 1f(%%rip), %%rax; pushq %%rax; lretq; 1:" : : : "rax", "memory");
    asm volatile("lidt %0" : : "m"(kernel_idtr));
    install_tss(cpu->index, cpu->tss);
    syscalls_cpu_initialize();

    asm volatile("fninit");
    const uint32_t mxcsr = 0x1F80;
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));

    lapic_enable(true);
    lapic_timer_start();
    __atomic_store_n(&cpu->online, true, __ATOMIC_RELEASE);

    for (;;)
        asm volatile("sti; hlt");
}

static bool start_ap(struct cpu* cpu)
{
    void* stack = alloc_kernel_pages(KERNEL_STACK_PAGES);
    tss_t* tss = malloc(sizeof(tss_t));
    if (!stack || !tss)
        return false;
    memset(tss, 0, sizeof(*tss));
    cpu->tss = tss;
    if (!process_create_idle(cpu->index, (uint64_t)stack))
        return false;

    memcpy((void*)AP_TRAMPOLINE_BASE, ap_trampoline_start, ap_trampoline_end - ap_trampoline_start);
    const uint64_t offset_cr3 = (uint8_t*)ap_trampoline_cr3 - ap_trampoline_start;
    const uint64_t offset_stack = (uint8_t*)ap_trampoline_stack - ap_trampoline_start;
    const uint64_t offset_entry = (uint8_t*)ap_trampoline_entry - ap_trampoline_start;
    *(uint64_t*)(AP_TRAMPOLINE_BASE + offset_cr3) = mmu_kernel_page_map();
    *(uint64_t*)(AP_TRAMPOLINE_BASE + offset_stack) = (uint64_t)stack + KERNEL_STACK_SIZE;
    *(uint64_t*)(AP_TRAMPOLINE_BASE + offset_entry) = (uint64_t)ap_main;
    booting_index = cpu->index;

    lapic_send_init(cpu->apic_id);
    const uint64_t t0 = tsc_micros();
    while (tsc_micros() - t0 < 10000)
        asm volatile("pause");
    for (int attempt = 0; attempt < 2 && !cpu->online; attempt++) {
        lapic_send_startup(cpu->apic_id, AP_TRAMPOLINE_BASE >> 12);
        const uint64_t start = tsc_micros();
        while (!__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE) && tsc_micros() - start < AP_STARTUP_TIMEOUT_US)
            asm volatile("pause");
    }
    return cpu->online;
}

void smp_early_initialize()
{
    cpu_install(&cpus[0]);
}

void smp_initialize()
{
    uint8_t apic_ids[MAX_CPUS];
    uint64_t lapic_phys = 0;
    const int found = acpi_find_cpus(apic_ids, MAX_CPUS, &lapic_phys);
    if (found <= 1 || !lapic_initialize(lapic_phys))
        return;

    kernel_gdtr.size = 0x800 - 1;
    kernel_gdtr.address = (uint64_t)gdt_start;
    asm volatile("sidt %0" : "=m"(kernel_idtr));
    idt_set_entry(LAPIC_TIMER_VECTOR, (uintptr_t)irq_lapic_timer);
    idt_set_entry(TLB_SHOOTDOWN_VECTOR, (uintptr_t)irq_tlb_shootdown);
    idt_set_entry(RESCHED_VECTOR, (uintptr_t)irq_resched);
    idt_set_entry(LAPIC_SPURIOUS_VECTOR, (uintptr_t)irq_lapic_spurious);

    const uint64_t flags = irq_save();
    const bool locked = kernel_lock_enter();
    lapic_enable(false);
    lapic_timer_calibrate(AP_TIMER_HZ);
    const uint32_t bsp = lapic_id();
    cpus[0].apic_id = bsp;

    for (int i = 0; i < found; i++) {
        if (apic_ids[i] == bsp || ncpus >= MAX_CPUS)
            continue;
        struct cpu* cpu = &cpus[ncpus];
        cpu->index = ncpus;
        cpu->apic_id = apic_ids[i];

        ncpus++;
        if (!start_ap(cpu)) {
            terminal_printf("smp: cpu with apic id %d did not start\n", apic_ids[i]);
            ncpus--;
        }
    }
    terminal_printf("smp: %d cpus online\n", ncpus);
    if (locked)
        kernel_lock_exit();
    irq_restore(flags);
}
