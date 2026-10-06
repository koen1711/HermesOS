#ifndef OS_SYSCALLS_H
#define OS_SYSCALLS_H

#include <hardware/interrupts/interrupts.h>
#include <libc.h>

#ifdef LIBC
#if LIBC == mlibc

enum syscall_numbers {
    SYSCALL_WRITE = 1,
    SYSCALL_READ = 2,
    SYSCALL_OPEN = 3,
    SYSCALL_CLOSE = 4,
};

#endif
#endif

void handle_syscall(interrupt_context* context);

#endif //OS_SYSCALLS_H