#ifndef OS_SYSCALLS_H
#define OS_SYSCALLS_H

#include <hardware/interrupts/interrupts.h>
#include <libc.h>

#ifdef LIBC
#if LIBC == mlibc

enum syscall_numbers {
    SYSCALL_READ = 0,
    SYSCALL_WRITE = 1,
    SYSCALL_OPEN = 2,
    SYSCALL_CLOSE = 3,
    SYSCALL_LSEEK = 6,
    SYSCALL_MMAP = 9,
    SYSCALL_MUNMAP = 11,
    SYSCALL_CLOCK_GETTIME = 288,

};

#endif
#endif

void handle_syscall(interrupt_context* context);

#endif //OS_SYSCALLS_H