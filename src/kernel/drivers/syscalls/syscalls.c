#include "syscalls.h"
#include "fd.h"
#include "sys.h"
#include "uaccess.h"
#include <drivers/scheduler/process.h>
#include <drivers/scheduler/futex.h>
#include <drivers/scheduler/signal.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/net/socket/socket_syscalls.h>
#include <drivers/net/core/netisr.h>
#include <os/errno.h>

#define AT_FDCWD (-100)
#define CLONE_VM 0x00000100
#define CLONE_VFORK 0x00004000

#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define EFER_SCE  0x1
#define FMASK_BITS 0x47700  // TF, DF, IF, IOPL, NT, AC

extern void irq_interrupt_syscall();
extern void syscall_entry();

static uint64_t rdmsr(const uint32_t msr)
{
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static void wrmsr(const uint32_t msr, const uint64_t value)
{
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

void syscalls_cpu_initialize()
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR, (0x13ULL << 48) | (0x08ULL << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, FMASK_BITS);
}

void syscalls_initialize()
{
    idt_set_entry(SYSCALL_VECTOR, (uintptr_t)irq_interrupt_syscall);
    idt[SYSCALL_VECTOR].type_attr = 0xEE;
    syscalls_cpu_initialize();
}

static long socket_syscall(const interrupt_context* c)
{
    const int fd = (int)c->rdi;
    switch (c->rax) {
        case SYSCALL_SOCKET:
            return sys_socket((int)c->rdi, (int)c->rsi, (int)c->rdx);
        case SYSCALL_CONNECT:
            return sys_connect(fd, (const struct sockaddr*)c->rsi, (socklen_t)c->rdx);
        case SYSCALL_ACCEPT:
            return sys_accept(fd, (struct sockaddr*)c->rsi, (socklen_t*)c->rdx);
        case SYSCALL_ACCEPT4:
            return sys_accept4(fd, (struct sockaddr*)c->rsi, (socklen_t*)c->rdx, (int)c->r10);
        case SYSCALL_SENDTO:
            return sys_sendto(fd, (const void*)c->rsi, c->rdx, (int)c->r10, (const struct sockaddr*)c->r8,
                              (socklen_t)c->r9);
        case SYSCALL_RECVFROM:
            return sys_recvfrom(fd, (void*)c->rsi, c->rdx, (int)c->r10, (struct sockaddr*)c->r8, (socklen_t*)c->r9);
        case SYSCALL_SENDMSG:
            return sys_sendmsg(fd, (const struct msghdr_s*)c->rsi, (int)c->rdx);
        case SYSCALL_RECVMSG:
            return sys_recvmsg(fd, (struct msghdr_s*)c->rsi, (int)c->rdx);
        case SYSCALL_SHUTDOWN:
            return sys_shutdown(fd, (int)c->rsi);
        case SYSCALL_BIND:
            return sys_bind(fd, (const struct sockaddr*)c->rsi, (socklen_t)c->rdx);
        case SYSCALL_LISTEN:
            return sys_listen(fd, (int)c->rsi);
        case SYSCALL_GETSOCKNAME:
            return sys_getsockname(fd, (struct sockaddr*)c->rsi, (socklen_t*)c->rdx);
        case SYSCALL_GETPEERNAME:
            return sys_getpeername(fd, (struct sockaddr*)c->rsi, (socklen_t*)c->rdx);
        case SYSCALL_SOCKETPAIR:
            return sys_socketpair((int)c->rdi, (int)c->rsi, (int)c->rdx, (int*)c->r10);
        case SYSCALL_SETSOCKOPT:
            return sys_setsockopt(fd, (int)c->rsi, (int)c->rdx, (const void*)c->r10, (socklen_t)c->r8);
        case SYSCALL_GETSOCKOPT:
            return sys_getsockopt(fd, (int)c->rsi, (int)c->rdx, (void*)c->r10, (socklen_t*)c->r8);
        default:
            return -ENOSYS;
    }
}

static long file_syscall(const interrupt_context* c)
{
    switch (c->rax) {
        case SYSCALL_READ:
            return sys_read((int)c->rdi, (void*)c->rsi, c->rdx);
        case SYSCALL_WRITE:
            return sys_write((int)c->rdi, (const void*)c->rsi, c->rdx);
        case SYSCALL_OPEN:
            return sys_openat(AT_FDCWD, (const char*)c->rdi, (int)c->rsi, (uint32_t)c->rdx);
        case SYSCALL_OPENAT:
            return sys_openat((int)c->rdi, (const char*)c->rsi, (int)c->rdx, (uint32_t)c->r10);
        case SYSCALL_CLOSE:
            return sys_close((int)c->rdi);
        case SYSCALL_STAT:
            return sys_newfstatat(AT_FDCWD, (const char*)c->rdi, (void*)c->rsi, 0);
        case SYSCALL_LSTAT:
            return sys_newfstatat(AT_FDCWD, (const char*)c->rdi, (void*)c->rsi, 0x100);
        case SYSCALL_FSTAT:
            return sys_fstat((int)c->rdi, (void*)c->rsi);
        case SYSCALL_NEWFSTATAT:
            return sys_newfstatat((int)c->rdi, (const char*)c->rsi, (void*)c->rdx, (int)c->r10);
        case SYSCALL_LSEEK:
            return sys_lseek((int)c->rdi, (int64_t)c->rsi, (int)c->rdx);
        case SYSCALL_PREAD64:
            return sys_pread64((int)c->rdi, (void*)c->rsi, c->rdx, (int64_t)c->r10);
        case SYSCALL_PWRITE64:
            return sys_pwrite64((int)c->rdi, (const void*)c->rsi, c->rdx, (int64_t)c->r10);
        case SYSCALL_READV:
            return sys_readv((int)c->rdi, (const void*)c->rsi, (int)c->rdx);
        case SYSCALL_WRITEV:
            return sys_writev((int)c->rdi, (const void*)c->rsi, (int)c->rdx);
        case SYSCALL_GETDENTS64:
            return sys_getdents64((int)c->rdi, (void*)c->rsi, c->rdx);
        case SYSCALL_GETCWD:
            return sys_getcwd((char*)c->rdi, c->rsi);
        case SYSCALL_CHDIR:
            return sys_chdir((const char*)c->rdi);
        case SYSCALL_FCHDIR:
            return sys_fchdir((int)c->rdi);
        case SYSCALL_DUP:
            return sys_dup((int)c->rdi);
        case SYSCALL_DUP2:
            return sys_dup2((int)c->rdi, (int)c->rsi);
        case SYSCALL_DUP3:
            return sys_dup3((int)c->rdi, (int)c->rsi, (int)c->rdx);
        case SYSCALL_FCNTL:
            return sys_fcntl((int)c->rdi, (int)c->rsi, c->rdx);
        case SYSCALL_IOCTL:
            return sys_ioctl((int)c->rdi, c->rsi, (void*)c->rdx);
        case SYSCALL_ACCESS:
            return sys_faccessat(AT_FDCWD, (const char*)c->rdi, (int)c->rsi, 0);
        case SYSCALL_FACCESSAT:
            return sys_faccessat((int)c->rdi, (const char*)c->rsi, (int)c->rdx, 0);
        case SYSCALL_FACCESSAT2:
            return sys_faccessat((int)c->rdi, (const char*)c->rsi, (int)c->rdx, (int)c->r10);
        case SYSCALL_READLINK:
            return sys_readlinkat(AT_FDCWD, (const char*)c->rdi, (char*)c->rsi, c->rdx);
        case SYSCALL_READLINKAT:
            return sys_readlinkat((int)c->rdi, (const char*)c->rsi, (char*)c->rdx, c->r10);
        case SYSCALL_MKDIR:
            return sys_mkdirat(AT_FDCWD, (const char*)c->rdi, (uint32_t)c->rsi);
        case SYSCALL_MKDIRAT:
            return sys_mkdirat((int)c->rdi, (const char*)c->rsi, (uint32_t)c->rdx);
        case SYSCALL_UNLINK:
            return sys_unlinkat(AT_FDCWD, (const char*)c->rdi, 0);
        case SYSCALL_RMDIR:
            return sys_unlinkat(AT_FDCWD, (const char*)c->rdi, 0x200);
        case SYSCALL_UNLINKAT:
            return sys_unlinkat((int)c->rdi, (const char*)c->rsi, (int)c->rdx);
        case SYSCALL_RENAME:
            return sys_renameat(AT_FDCWD, (const char*)c->rdi, AT_FDCWD, (const char*)c->rsi, 0);
        case SYSCALL_RENAMEAT:
            return sys_renameat((int)c->rdi, (const char*)c->rsi, (int)c->rdx, (const char*)c->r10, 0);
        case SYSCALL_RENAMEAT2:
            return sys_renameat((int)c->rdi, (const char*)c->rsi, (int)c->rdx, (const char*)c->r10,
                                (unsigned int)c->r8);
        case SYSCALL_LINK:
        case SYSCALL_SYMLINK:
        case SYSCALL_LINKAT:
        case SYSCALL_SYMLINKAT:
            return -EPERM;
        case SYSCALL_TRUNCATE:
            return sys_truncate((const char*)c->rdi, (int64_t)c->rsi);
        case SYSCALL_FTRUNCATE:
            return sys_ftruncate((int)c->rdi, (int64_t)c->rsi);
        case SYSCALL_CHMOD:
        case SYSCALL_CHOWN:
        case SYSCALL_LCHOWN:
            return sys_path_exists(AT_FDCWD, (const char*)c->rdi);
        case SYSCALL_FCHMODAT:
        case SYSCALL_FCHOWNAT:
        case SYSCALL_UTIMENSAT:
            return sys_path_exists((int)c->rdi, (const char*)c->rsi);
        case SYSCALL_FCHMOD:
        case SYSCALL_FCHOWN:
            return fd_get((int)c->rdi) ? 0 : -EBADF;
        case SYSCALL_UMASK:
            return sys_umask((uint32_t)c->rdi);
        case SYSCALL_FSYNC:
        case SYSCALL_FDATASYNC:
            return sys_fsync((int)c->rdi);
        case SYSCALL_FLOCK:
        case SYSCALL_FADVISE64:
            return fd_get((int)c->rdi) ? 0 : -EBADF;
        case SYSCALL_SETXATTR:
        case SYSCALL_LSETXATTR:
        case SYSCALL_FSETXATTR:
        case SYSCALL_GETXATTR:
        case SYSCALL_LGETXATTR:
        case SYSCALL_FGETXATTR:
        case SYSCALL_LISTXATTR:
        case SYSCALL_LLISTXATTR:
        case SYSCALL_FLISTXATTR:
        case SYSCALL_REMOVEXATTR:
        case SYSCALL_LREMOVEXATTR:
        case SYSCALL_FREMOVEXATTR:
            return -EOPNOTSUPP;
        case SYSCALL_SYNC:
            vfs_sync();
            return 0;
        case SYSCALL_PIPE:
            return sys_pipe2((int*)c->rdi, 0);
        case SYSCALL_PIPE2:
            return sys_pipe2((int*)c->rdi, (int)c->rsi);
        case SYSCALL_SENDFILE:
            return sys_sendfile((int)c->rdi, (int)c->rsi, (int64_t*)c->rdx, c->r10);
        case SYSCALL_POLL:
            return sys_poll((void*)c->rdi, c->rsi, (int)c->rdx);
        case SYSCALL_PPOLL:
            return sys_ppoll((void*)c->rdi, c->rsi, (const struct timespec_k*)c->rdx, (const uint64_t*)c->r10, c->r8);
        case SYSCALL_SELECT:
            return sys_select((int)c->rdi, (uint64_t*)c->rsi, (uint64_t*)c->rdx, (uint64_t*)c->r10,
                              (struct timeval_k*)c->r8);
        case SYSCALL_PSELECT6:
            return sys_pselect6((int)c->rdi, (uint64_t*)c->rsi, (uint64_t*)c->rdx, (uint64_t*)c->r10,
                                (const struct timespec_k*)c->r8, (const void*)c->r9);
        case SYSCALL_EPOLL_CREATE:
            return (int)c->rdi <= 0 ? -EINVAL : sys_epoll_create1(0);
        case SYSCALL_EPOLL_CREATE1:
            return sys_epoll_create1((int)c->rdi);
        case SYSCALL_EPOLL_CTL:
            return sys_epoll_ctl((int)c->rdi, (int)c->rsi, (int)c->rdx, (const void*)c->r10);
        case SYSCALL_EPOLL_WAIT:
        case SYSCALL_EPOLL_PWAIT:
            return sys_epoll_pwait((int)c->rdi, (void*)c->rsi, (int)c->rdx,
                                   (int)c->r10 < 0 ? -1 : (int64_t)(int)c->r10 * 1000000,
                                   c->rax == SYSCALL_EPOLL_PWAIT ? (const uint64_t*)c->r8 : nullptr, c->r9);
        case SYSCALL_EPOLL_PWAIT2:
            return sys_epoll_pwait2((int)c->rdi, (void*)c->rsi, (int)c->rdx, (const struct timespec_k*)c->r10,
                                    (const uint64_t*)c->r8, c->r9);
        case SYSCALL_EVENTFD:
            return sys_eventfd2((unsigned int)c->rdi, 0);
        case SYSCALL_EVENTFD2:
            return sys_eventfd2((unsigned int)c->rdi, (int)c->rsi);
        case SYSCALL_STATFS:
            return sys_statfs((const char*)c->rdi, (void*)c->rsi);
        case SYSCALL_FSTATFS:
            return sys_fstatfs((int)c->rdi, (void*)c->rsi);
        // TODO: For CPython: os.mknod/os.mkfifo (mkfifo needs a FIFO inode type, FAT32 can't store one)
        case SYSCALL_MKNOD:
            return sys_mknodat(AT_FDCWD, (const char*)c->rdi, (uint32_t)c->rsi, (uint32_t)c->rdx);
        case SYSCALL_MKNODAT:
            return sys_mknodat((int)c->rdi, (const char*)c->rsi, (uint32_t)c->rdx, (uint32_t)c->r10);
        case SYSCALL_FALLOCATE:
            return sys_fallocate((int)c->rdi, (int)c->rsi, (int64_t)c->rdx, (int64_t)c->r10);
        // TODO: For CPython: os.preadv/os.pwritev; the 2 variants only need flags == 0
        case SYSCALL_PREADV:
        case SYSCALL_PWRITEV:
        case SYSCALL_PREADV2:
        case SYSCALL_PWRITEV2:
        // TODO: For CPython: os.splice and os.copy_file_range
        case SYSCALL_SPLICE:
        case SYSCALL_COPY_FILE_RANGE:
        // TODO: For CPython: os.closerange and subprocess close_fds use close_range, falling back on -ENOSYS
        case SYSCALL_CLOSE_RANGE:
        // TODO: For CPython: os.memfd_create
        case SYSCALL_MEMFD_CREATE:
        // TODO: For CPython: os.chroot
        case SYSCALL_CHROOT:
            return -ENOSYS;
        default:
            return -ENOSYS;
    }
}

static bool root_ids(const uint64_t id)
{
    return (uint32_t)id == 0 || (uint32_t)id == (uint32_t)-1;
}

static long put_root_ids(uint32_t* a, uint32_t* b, uint32_t* c)
{
    constexpr uint32_t zero = 0;
    uint32_t* const ids[] = {a, b, c};
    for (size_t i = 0; i < 3; i++)
        if (ids[i] && copy_to_user(ids[i], &zero, sizeof(zero)) != 0)
            return -EFAULT;
    return 0;
}

static long process_syscall(interrupt_context* c, process_t* current)
{
    switch (c->rax) {
        case SYSCALL_MMAP:
            return sys_mmap(c->rdi, c->rsi, (int)c->rdx, (int)c->r10, (int)c->r8, (int64_t)c->r9);
        case SYSCALL_MUNMAP:
            return sys_munmap(c->rdi, c->rsi);
        case SYSCALL_MPROTECT:
            return sys_mprotect(c->rdi, c->rsi, (int)c->rdx);
        case SYSCALL_BRK:
            return sys_brk(c->rdi);
        case SYSCALL_MADVISE:
            return sys_madvise(c->rdi, c->rsi, (int)c->rdx);
        case SYSCALL_MSYNC:
            return 0;
        case SYSCALL_MREMAP:
            return sys_mremap(c->rdi, c->rsi, c->rdx, (int)c->r10, c->r8);
        case SYSCALL_CLONE:
            return process_clone(c->rdi, c->rsi, (int*)c->rdx, (int*)c->r10, c->r8);
        case SYSCALL_FORK:
            return process_fork();
        case SYSCALL_VFORK:
            return process_clone(CLONE_VFORK | CLONE_VM | SIGCHLD, 0, nullptr, nullptr, 0);
        case SYSCALL_EXECVE:
            return sys_execve((const char*)c->rdi, (char* const*)c->rsi, (char* const*)c->rdx);
        case SYSCALL_EXIT:
            return sys_exit((int)c->rdi);
        case SYSCALL_EXIT_GROUP:
            return sys_exit_group((int)c->rdi);
        case SYSCALL_WAIT4:
            return sys_wait4((int)c->rdi, (int*)c->rsi, (int)c->rdx, (void*)c->r10);
        case SYSCALL_GETPID:
            return current->tgid;
        case SYSCALL_GETTID:
            return current->pid;
        case SYSCALL_GETPPID:
            return current->ppid;
        case SYSCALL_GETUID:
        case SYSCALL_GETEUID:
        case SYSCALL_GETGID:
        case SYSCALL_GETEGID:
            return 0;
        case SYSCALL_SETUID:
        case SYSCALL_SETGID:
            return c->rdi == 0 ? 0 : -EPERM;
        case SYSCALL_SETREUID:
        case SYSCALL_SETREGID:
            return root_ids(c->rdi) && root_ids(c->rsi) ? 0 : -EPERM;
        case SYSCALL_SETRESUID:
        case SYSCALL_SETRESGID:
            return root_ids(c->rdi) && root_ids(c->rsi) && root_ids(c->rdx) ? 0 : -EPERM;
        case SYSCALL_GETRESUID:
        case SYSCALL_GETRESGID:
            return put_root_ids((uint32_t*)c->rdi, (uint32_t*)c->rsi, (uint32_t*)c->rdx);
        case SYSCALL_GETGROUPS:
            return sys_getgroups((int)c->rdi, (uint32_t*)c->rsi);
        case SYSCALL_GETPGRP:
        case SYSCALL_SETSID:
            return current->tgid;
        case SYSCALL_GETPGID:
        case SYSCALL_GETSID:
            return c->rdi ? (process_find((int)c->rdi) ? (long)c->rdi : -ESRCH) : current->tgid;
        case SYSCALL_SETPGID:
            return 0;
        case SYSCALL_SET_TID_ADDRESS:
            return sys_set_tid_address((int*)c->rdi);
        case SYSCALL_SET_ROBUST_LIST:
            return 0;
        case SYSCALL_FUTEX:
            return sys_futex((uint32_t*)c->rdi, (int)c->rsi, (uint32_t)c->rdx, (const void*)c->r10,
                             (uint32_t*)c->r8, (uint32_t)c->r9);
        case SYSCALL_ARCH_PRCTL:
            return sys_arch_prctl((int)c->rdi, c->rsi);
        case SYSCALL_PRCTL:
            return sys_prctl((int)c->rdi, c->rsi);
        case SYSCALL_SCHED_YIELD:
            process_yield();
            return 0;
        case SYSCALL_SCHED_GETAFFINITY:
            return sys_sched_getaffinity((int)c->rdi, c->rsi, (void*)c->rdx);
        case SYSCALL_GETPRIORITY:
            return 20;
        case SYSCALL_SETPRIORITY:
            return (int)c->rdx == 0 ? 0 : -EPERM;
        case SYSCALL_SCHED_GETSCHEDULER:
            return 0;
        case SYSCALL_SCHED_GET_PRIORITY_MAX:
        case SYSCALL_SCHED_GET_PRIORITY_MIN:
            return c->rdi == 0 ? 0 : -EINVAL;
        case SYSCALL_SCHED_GETPARAM:
            return put_root_ids((uint32_t*)c->rsi, nullptr, nullptr);
        case SYSCALL_UNAME:
            return sys_uname((void*)c->rdi);
        case SYSCALL_SYSINFO:
            return sys_sysinfo((void*)c->rdi);
        case SYSCALL_GETRLIMIT:
            return sys_prlimit64(0, (int)c->rdi, nullptr, (void*)c->rsi);
        case SYSCALL_SETRLIMIT:
            return sys_prlimit64(0, (int)c->rdi, (const void*)c->rsi, nullptr);
        case SYSCALL_PRLIMIT64:
            return sys_prlimit64((int)c->rdi, (int)c->rsi, (const void*)c->rdx, (void*)c->r10);
        case SYSCALL_GETRUSAGE:
            return sys_getrusage((int)c->rdi, (void*)c->rsi);
        case SYSCALL_GETRANDOM:
            return sys_getrandom((void*)c->rdi, c->rsi, (unsigned int)c->rdx);
        case SYSCALL_CLOCK_GETTIME:
            return sys_clock_gettime((int)c->rdi, (struct timespec_k*)c->rsi);
        case SYSCALL_CLOCK_GETRES:
            return sys_clock_getres((int)c->rdi, (struct timespec_k*)c->rsi);
        case SYSCALL_GETTIMEOFDAY:
            return sys_gettimeofday((struct timeval_k*)c->rdi, (void*)c->rsi);
        case SYSCALL_TIME:
            return sys_time((int64_t*)c->rdi);
        case SYSCALL_NANOSLEEP:
            return sys_nanosleep((const struct timespec_k*)c->rdi, (struct timespec_k*)c->rsi);
        case SYSCALL_CLOCK_NANOSLEEP:
            return sys_clock_nanosleep((int)c->rdi, (int)c->rsi, (const struct timespec_k*)c->rdx,
                                       (struct timespec_k*)c->r10);
        case SYSCALL_ALARM:
            return sys_alarm((unsigned int)c->rdi);
        case SYSCALL_SETITIMER:
            return sys_setitimer((int)c->rdi, (const void*)c->rsi, (void*)c->rdx);
        case SYSCALL_GETITIMER:
            return sys_getitimer((int)c->rdi, (void*)c->rsi);
        case SYSCALL_TIMES:
            return sys_times((void*)c->rdi);
        case SYSCALL_RT_SIGACTION:
            return sys_rt_sigaction((int)c->rdi, (const struct k_sigaction*)c->rsi, (struct k_sigaction*)c->rdx,
                                    c->r10);
        case SYSCALL_RT_SIGPROCMASK:
            return sys_rt_sigprocmask((int)c->rdi, (const uint64_t*)c->rsi, (uint64_t*)c->rdx, c->r10);
        case SYSCALL_RT_SIGSUSPEND:
            return sys_rt_sigsuspend((const uint64_t*)c->rdi, c->rsi);
        case SYSCALL_RT_SIGPENDING:
            return sys_rt_sigpending((uint64_t*)c->rdi, c->rsi);
        case SYSCALL_RT_SIGTIMEDWAIT:
            return sys_rt_sigtimedwait((const uint64_t*)c->rdi, (struct siginfo_s*)c->rsi, (const void*)c->rdx,
                                       c->r10);
        case SYSCALL_SIGALTSTACK:
            return sys_sigaltstack((const struct sigaltstack_s*)c->rdi, (struct sigaltstack_s*)c->rsi, c->rsp);
        case SYSCALL_PAUSE:
            return sys_pause();
        case SYSCALL_KILL:
            return sys_kill((int)c->rdi, (int)c->rsi);
        case SYSCALL_TKILL:
            return sys_tgkill(0, (int)c->rdi, (int)c->rsi);
        case SYSCALL_TGKILL:
            return sys_tgkill((int)c->rdi, (int)c->rsi, (int)c->rdx);
        // TODO: For CPython: os.setgroups/initgroups; needs a group list per process for getgroups to report
        case SYSCALL_SETGROUPS:
        // TODO: For CPython: os.sched_setscheduler/sched_setparam (accept SCHED_OTHER with priority 0),
        // os.sched_rr_get_interval and os.sched_setaffinity
        case SYSCALL_SCHED_SETPARAM:
        case SYSCALL_SCHED_SETSCHEDULER:
        case SYSCALL_SCHED_RR_GET_INTERVAL:
        case SYSCALL_SCHED_SETAFFINITY:
        // TODO: For CPython: time.clock_settime
        case SYSCALL_CLOCK_SETTIME:
        // TODO: For CPython: os.waitid (subprocess and multiprocessing use wait4)
        case SYSCALL_WAITID:
        // TODO: For CPython: os.pidfd_open; asyncio falls back to another child watcher on error
        case SYSCALL_PIDFD_OPEN:
            return -ENOSYS;
        default:
            return file_syscall(c);
    }
}

void handle_syscall(interrupt_context* context)
{
    process_t* current = scheduler_get()->current;
    if (current == nullptr || current->pid == IDLE_PROC_PID) {
        context->rax = (uint64_t)-ENOSYS;
        return;
    }
    current->syscall_nr = (int64_t)context->rax;
    current->frame = context;

    switch (context->rax) {
        case SYSCALL_RT_SIGRETURN:
            sys_rt_sigreturn(context);
            break;
        case SYSCALL_SOCKET ... SYSCALL_GETSOCKOPT:
        case SYSCALL_ACCEPT4:
            context->rax = (uint64_t)socket_syscall(context);
            netisr_poll();
            break;
        case SYSCALL_WRITE:
        case SYSCALL_WRITEV:
        case SYSCALL_CLOSE:
        case SYSCALL_SENDFILE:
            context->rax = (uint64_t)file_syscall(context);
            netisr_poll();
            break;
        default:
            context->rax = (uint64_t)process_syscall(context, current);
            break;
    }
}
