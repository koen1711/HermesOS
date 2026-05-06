#ifndef OS_SYSCALLS_H
#define OS_SYSCALLS_H

#include <hardware/interrupts/interrupts.h>

void handle_syscall(interrupt_context* context);

#endif //OS_SYSCALLS_H