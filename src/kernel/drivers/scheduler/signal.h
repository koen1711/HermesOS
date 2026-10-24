#ifndef SCHEDULER_SIGNAL_H
#define SCHEDULER_SIGNAL_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <os/stddef.h>
#include <hardware/interrupts/interrupts.h>

enum signal_number
{
    SIGHUP = 1,
    SIGINT = 2,
    SIGQUIT = 3,
    SIGILL = 4,
    SIGTRAP = 5,
    SIGABRT = 6,
    SIGBUS = 7,
    SIGFPE = 8,
    SIGKILL = 9,
    SIGUSR1 = 10,
    SIGSEGV = 11,
    SIGUSR2 = 12,
    SIGPIPE = 13,
    SIGALRM = 14,
    SIGTERM = 15,
    SIGSTKFLT = 16,
    SIGCHLD = 17,
    SIGCONT = 18,
    SIGSTOP = 19,
    SIGTSTP = 20,
    SIGTTIN = 21,
    SIGTTOU = 22,
    SIGURG = 23,
    SIGXCPU = 24,
    SIGXFSZ = 25,
    SIGVTALRM = 26,
    SIGPROF = 27,
    SIGWINCH = 28,
    SIGIO = 29,
    SIGPWR = 30,
    SIGSYS = 31
};

#define SIGNAL_MAX 64

#define SIG_DFL 0
#define SIG_IGN 1

enum sigaction_flags
{
    SA_NOCLDSTOP = 0x00000001,
    SA_NOCLDWAIT = 0x00000002,
    SA_SIGINFO = 0x00000004,
    SA_RESTORER = 0x04000000,
    SA_ONSTACK = 0x08000000,
    SA_RESTART = 0x10000000,
    SA_NODEFER = 0x40000000,
    SA_RESETHAND = 0x80000000
};

enum sigprocmask_how
{
    SIG_BLOCK = 0,
    SIG_UNBLOCK = 1,
    SIG_SETMASK = 2
};

enum signal_code
{
    SI_USER = 0,
    SI_KERNEL = 0x80,
    SEGV_MAPERR = 1,
    SEGV_ACCERR = 2,
    ILL_ILLOPN = 2,
    FPE_INTDIV = 1,
    BUS_ADRALN = 1,
    TRAP_BRKPT = 1
};

struct k_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct siginfo_s {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad;
    union {
        struct {
            int32_t si_pid;
            uint32_t si_uid;
        } kill;
        uint64_t si_addr;
    };
    uint8_t rest[104];
};

struct sigcontext_s {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved[8];
};

struct sigaltstack_s {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t pad;
    uint64_t ss_size;
};

enum sigaltstack_flags
{
    SS_ONSTACK = 1,
    SS_DISABLE = 2,
    SS_AUTODISARM = (int32_t)0x80000000
};

#define MINSIGSTKSZ 2048

struct ucontext_s {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct sigaltstack_s uc_stack;
    struct sigcontext_s uc_mcontext;
    uint64_t uc_sigmask;
};

struct signal_info {
    int32_t code;
    int32_t pid;
    uint64_t addr;
};

int signal_send(int pid, int sig);
int signal_for_exception(uint64_t vector);
bool signal_fault(const interrupt_context* context, uint64_t cr2);
void signal_deliver(interrupt_context* context);

long sys_kill(int pid, int sig);
long sys_tgkill(int tgid, int tid, int sig);
long sys_rt_sigsuspend(const uint64_t* mask, size_t sigsetsize);
long sys_rt_sigpending(uint64_t* set, size_t sigsetsize);
long sys_rt_sigtimedwait(const uint64_t* set, struct siginfo_s* info, const void* timeout, size_t sigsetsize);
long sys_pause();
long sys_rt_sigaction(int sig, const struct k_sigaction* act, struct k_sigaction* oact, size_t sigsetsize);
long sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oset, size_t sigsetsize);

void sys_rt_sigreturn(interrupt_context* context);
long sys_sigaltstack(const struct sigaltstack_s* ss, struct sigaltstack_s* oss, uint64_t user_rsp);

#endif //SCHEDULER_SIGNAL_H
