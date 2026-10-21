#include "syscalls.h"
#include <scheduler/process.h> // Fork syscall: Fork, GetScheduler

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
            Process* current = GetScheduler()->current_proc;
            if (current == NULL) {
                context->rax = (uint64_t)-1;
                break;
            }
            current->tf = context;
            context->rax = (uint64_t)(int64_t)Fork();
            break;
        }
        default:
            // Unknown syscall
            break;
    }
}