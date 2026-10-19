#include "syscalls.h"

extern void irq_interrupt_syscall();   // interrupt.asm

void syscalls_initialize()
{
    idt_set_entry(SYSCALL_VECTOR, (uintptr_t)irq_interrupt_syscall);
}

void handle_syscall(interrupt_context* context)
{
    switch (context->rax) {
        case SYSCALL_WRITE:
            // Handle write syscall
            break;
        case SYSCALL_READ:
            // Handle read syscall
            break;
        case SYSCALL_OPEN:
            // Handle open syscall
            break;
        case SYSCALL_CLOSE:
            // Handle close syscall
            break;
        default:
            // Unknown syscall
            break;
    }
}