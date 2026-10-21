#include "interrupts.h"

#include <os/stdbool.h>
#include <os/stdint.h>
#include <hardware/interrupts/pic.h>
#include <drivers/syscalls/syscalls.h>
#include <drivers/terminal/terminal.h>
#include <scheduler/process.h> // Timer: Yield, exceptions: Exit

#define EXCEPTION_COUNT   32
#define TRAP_STUB_COUNT   (IRQ_OFFSET + 16) // exceptions 0-31 and IRQs 0-15
#define PAGE_FAULT_VECTOR 14

void handle_pic_interrupt(const interrupt_context* context);

extern uint64_t trap_stub_table[TRAP_STUB_COUNT]; // interrupt.asm
extern void irq_interrupt_syscall();

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

static bool from_user_mode(const TrapFrame* tf)
{
    return (tf->cs & 3) == 3;
}


static void handle_exception(const TrapFrame* tf)
{
    uint64_t cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    terminal_printf("\nException %d (%s), error code 0x%lx\n",
                    (int)tf->trapno, exception_names[tf->trapno], tf->errorcode);
    terminal_printf("rip 0x%lx  cs 0x%lx  rsp 0x%lx  rflags 0x%lx\n",
                    tf->rip, tf->cs, tf->rsp, tf->rflags);
    if (tf->trapno == PAGE_FAULT_VECTOR)
        terminal_printf("page fault address (cr2) 0x%lx\n", cr2);

    if (from_user_mode(tf)) {
        terminal_printf("killing process %d\n", GetScheduler()->current_proc->pid);
        Exit();
    }

    terminal_printf("kernel halted\n");
    while (1)
        asm volatile("cli; hlt");
}


static bool should_preempt(const TrapFrame* tf)
{
    const Process* current = GetScheduler()->current_proc;
    if (current == nullptr) // ProcInit has not run yet
        return false;
    return from_user_mode(tf) || current->pid == IDLE_PROC_PID;
}

void trap_handler(TrapFrame* tf)
{
    if (tf->trapno < EXCEPTION_COUNT)
        handle_exception(tf);
    else if (tf->trapno == SYSCALL_VECTOR)
        handle_syscall(tf);
    else if (tf->trapno >= IRQ_OFFSET && tf->trapno < IRQ_OFFSET + 16) {
        handle_pic_interrupt(tf); // sends the EOI, so the PIC keeps sending timer ticks after a switch

        if (tf->trapno == IRQ_OFFSET && should_preempt(tf))
            Yield();
    }
}



bool are_interrupts_enabled()
{
    uint64_t flags;
    asm volatile ( "pushfq\n\t"
                   "popq %0"
                   : "=g"(flags) );
    return flags & (1 << 9);
}

void irq_restore(uint64_t flags)
{
    asm ("pushq %0\n\tpopfq" : : "rm"(flags) : "memory","cc");
}


void idt_initialize()
{

    struct {
        uint16_t size;
        uint64_t address;
    } __attribute__((packed)) idtr;

    idtr.size = IDT_SIZE * IDT_ENTRY_SIZE - 1;

    idtr.address = (uint64_t)idt;

    for (int i = 0; i < TRAP_STUB_COUNT; i++)
        idt_set_entry(i, trap_stub_table[i]);

    uintptr_t irq_syscall_handler_address = (uintptr_t)irq_interrupt_syscall;
    idt_set_entry(SYSCALL_VECTOR, irq_syscall_handler_address);
    idt[SYSCALL_VECTOR].type_attr = 0xEE;




    pic_remap(IRQ_OFFSET, IRQ_OFFSET + 8);

    asm volatile("lidt %0" : : "m"(idtr));
    irq_restore(0x200);
}