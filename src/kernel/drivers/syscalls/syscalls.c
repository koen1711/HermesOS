#include "syscalls.h"
#include <scheduler/process.h> // fork syscall: process_fork, scheduler_get

extern void irq_interrupt_syscall();   // interrupt.asm

void syscalls_initialize()
{
    idt_set_entry(SYSCALL_VECTOR, (uintptr_t)irq_interrupt_syscall);
    idt[SYSCALL_VECTOR].type_attr = 0xEE;  // DPL 3, so user mode can raise it
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
        case SYSCALL_FORK: {
            process_t* current = scheduler_get()->current;
            if (current == nullptr) {
                context->rax = (uint64_t)-1;
                break;
            }
            current->frame = context;
            context->rax = (uint64_t)(int64_t)process_fork();
            break;
        }
        default:
            // Unknown syscall
            break;
    }
}