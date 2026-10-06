#include "syscalls.h"

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