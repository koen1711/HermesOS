#include "interrupts.h"

#include <os/stdbool.h>
#include <os/stdint.h>
#include <hardware/interrupts/pic.h>
#include <hardware/interrupts/irq.h>
#include <drivers/syscalls/syscalls.h>
#include <drivers/terminal/terminal.h>
#include <drivers/timer/pit.h>
#include <drivers/scheduler/process.h> // timer: process_yield, exceptions: process_exit
#include <drivers/scheduler/signal.h>
#include <hardware/memory/vm.h>
#include <hardware/smp/lapic.h>
#include <hardware/smp/smp.h>

#define EXCEPTION_COUNT   32
#define PAGE_FAULT_VECTOR 14

void handle_pic_interrupt(const interrupt_context* context);

extern uint64_t exception_stub_table[EXCEPTION_COUNT]; // interrupt.asm

static const char* exception_names[EXCEPTION_COUNT] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device not available", "double fault", "coprocessor segment overrun",
    "invalid TSS", "segment not present", "stack fault", "general protection",
    "page fault", "reserved", "x87 floating point", "alignment check",
    "machine check", "SIMD floating point", "virtualization", "control protection",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "reserved", "VMM communication", "security", "reserved"
};

idt_entry idt[IDT_SIZE];

void idt_set_entry(const uint8_t index, const uintptr_t handler)
{
    idt_entry entry;
    entry.offset_low = handler & 0xFFFF;
    entry.selector = 0x08;
    entry.ist = 0;
    entry.type_attr = 0x8E;
    entry.offset_mid = (handler >> 16) & 0xFFFF;
    entry.offset_high = (handler >> 32) & 0xFFFFFFFF;
    entry.zero = 0;

    idt[index] = entry;
}

static bool from_user_mode(const interrupt_context* context)
{
    return (context->cs & 3) == 3;
}

static void handle_exception(const interrupt_context* context)
{
    uint64_t cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    const process_t* current = scheduler_get()->current;
    if (context->interrupt_number == PAGE_FAULT_VECTOR && current != nullptr && current->vm != nullptr
        && vm_fault(current->vm, cr2, context->error_code & 2))
        return;

    if (from_user_mode(context) && signal_fault(context, cr2))
        return;

    terminal_printf("\nException %d (%s), error code 0x%lx\n",
                    (int)context->interrupt_number, exception_names[context->interrupt_number],
                    context->error_code);
    terminal_printf("rip 0x%lx  cs 0x%lx  rsp 0x%lx  rflags 0x%lx\n",
                    context->rip, context->cs, context->rsp, context->rflags);
    if (context->interrupt_number == PAGE_FAULT_VECTOR)
        terminal_printf("page fault address (cr2) 0x%lx\n", cr2);

    if (from_user_mode(context)) {
        terminal_printf("killing process %d\n", scheduler_get()->current->pid);
        process_exit_group(signal_for_exception(context->interrupt_number));
    }

    terminal_printf("kernel halted\n");
    while (1)
        asm volatile("cli; hlt");
}

static bool should_preempt(const interrupt_context* context)
{
    const process_t* current = scheduler_get()->current;
    if (current == nullptr)
        return false;
    return from_user_mode(context) || current->pid == IDLE_PROC_PID;
}

void trap_handler(interrupt_context* context)
{
    // answered without the kernel lock: its holder may be the CPU waiting for this
    if (context->interrupt_number == TLB_SHOOTDOWN_VECTOR) {
        smp_tlb_interrupt();
        return;
    }
    if (context->interrupt_number == LAPIC_SPURIOUS_VECTOR)
        return;

    const bool locked = kernel_lock_enter();
    if (context->interrupt_number < EXCEPTION_COUNT)
        handle_exception(context);
    else if (context->interrupt_number == SYSCALL_VECTOR)
        handle_syscall(context);
    else if (context->interrupt_number == LAPIC_TIMER_VECTOR) {
        lapic_eoi();
        if (should_preempt(context))
            process_yield();
    }
    else if (context->interrupt_number >= IRQ_OFFSET && context->interrupt_number < IRQ_OFFSET + IRQ_MAX_LINES) {
        handle_pic_interrupt(context);

        if (context->interrupt_number == IRQ_OFFSET + PIT_IRQ && should_preempt(context))
            process_yield();
    }
    signal_deliver(context);
    if (locked)
        kernel_lock_exit();
}



bool are_interrupts_enabled()
{
    uint64_t flags;
    asm volatile ( "pushfq\n\t"
                   "popq %0"
                   : "=g"(flags) );
    return flags & (1 << 9);
}


void idt_initialize()
{

    struct {
        uint16_t size;
        uint64_t address;
    } __attribute__((packed)) idtr;

    idtr.size = IDT_SIZE * IDT_ENTRY_SIZE - 1;
    idtr.address = (uint64_t)idt;

    for (int i = 0; i < EXCEPTION_COUNT; i++)
        idt_set_entry(i, exception_stub_table[i]);

    pic_remap(IRQ_OFFSET, IRQ_OFFSET + 8);

    asm volatile("lidt %0" : : "m"(idtr));
}
