#include "signal.h"

#include <drivers/scheduler/process.h>
#include <drivers/syscalls/uaccess.h>
#include <hardware/interrupts/irq.h>
#include <hardware/memory/alloc.h>
#include <drivers/timer/clock.h>
#include <os/errno.h>

#define USER_CS 0x23
#define USER_SS 0x1B
#define RFLAGS_IF 0x200
#define RFLAGS_TF 0x100
#define RFLAGS_DF 0x400

#define RFLAGS_USER 0x50DD5
#define USER_ADDR_LIMIT 0x0000800000000000ULL
#define RED_ZONE 128
#define INT80_LEN 2

static const uint64_t unblockable = (1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1));

struct signal_frame {
    uint64_t pretcode;
    struct ucontext_s uc;
    struct siginfo_s info;
    uint8_t trampoline[8];
};

static uint64_t signal_bit(const int sig)
{
    return 1ULL << (sig - 1);
}

static bool signal_default_ignored(const int sig)
{
    switch (sig) {
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
    case SIGCONT:
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return true;
    default:
        return false;
    }
}

static bool signal_ignored(const process_t* proc, const int sig)
{
    const uint64_t h = proc->sig_actions[sig - 1].handler;
    return h == SIG_IGN || (h == SIG_DFL && signal_default_ignored(sig));
}

static process_t* signal_current_user()
{
    process_t* current = scheduler_get()->current;
    return current && current->pid != IDLE_PROC_PID ? current : nullptr;
}

static int signal_post(const int pid, const int sig, const struct signal_info info)
{
    if (sig < 0 || sig > SIGNAL_MAX)
        return -EINVAL;
    const uint64_t flags = irq_save();
    process_t* proc = process_find(pid);
    if (proc == nullptr) {
        irq_restore(flags);
        return -ESRCH;
    }
    if (sig != 0 && !signal_ignored(proc, sig)) {
        proc->sig_pending |= signal_bit(sig);
        proc->sig_info[sig - 1] = info;
        if (!(proc->sig_blocked & signal_bit(sig)) || (proc->sig_waiting & signal_bit(sig)))
            process_interrupt(proc);
    }
    irq_restore(flags);
    return 0;
}

int signal_send(const int pid, const int sig)
{
    return signal_post(pid, sig, (struct signal_info){ .code = SI_KERNEL });
}

long sys_kill(int pid, const int sig)
{
    const process_t* current = signal_current_user();
    if (pid == 0 && current)
        pid = current->tgid;   // TODO: process groups (0, -pgid) and -1 (everyone)
    if (pid <= 0)
        return -EINVAL;
    return signal_post(pid, sig, (struct signal_info){ .code = SI_USER, .pid = current ? current->tgid : 0 });
}

long sys_tgkill(const int tgid, const int tid, const int sig)
{
    const process_t* target = process_find(tid);
    if (target == nullptr || (tgid > 0 && target->tgid != tgid))
        return -ESRCH;
    const process_t* current = signal_current_user();
    return signal_post(tid, sig, (struct signal_info){ .code = SI_USER, .pid = current ? current->tgid : 0 });
}

void signal_kill_thread(process_t* proc)
{
    proc->sig_pending |= signal_bit(SIGKILL);
    proc->sig_info[SIGKILL - 1] = (struct signal_info){ .code = SI_KERNEL };
    process_interrupt(proc);
}

int signal_for_exception(const uint64_t vector)
{
    switch (vector) {
    case 0:     // divide error
    case 16:    // x87
    case 19:    // SIMD
        return SIGFPE;
    case 1:
    case 3:
        return SIGTRAP;
    case 6:
        return SIGILL;
    case 17:
        return SIGBUS;
    default:
        return SIGSEGV;
    }
}

bool signal_fault(const interrupt_context* context, const uint64_t cr2)
{
    process_t* current = signal_current_user();
    if (current == nullptr)
        return false;
    const int sig = signal_for_exception(context->interrupt_number);
    struct signal_info info = { .code = SI_KERNEL, .addr = context->rip };
    switch (sig) {
    case SIGFPE:
        info.code = FPE_INTDIV;
        break;
    case SIGTRAP:
        info.code = TRAP_BRKPT;
        break;
    case SIGILL:
        info.code = ILL_ILLOPN;
        break;
    case SIGBUS:
        info.code = BUS_ADRALN;
        break;
    default:
        if (context->interrupt_number == 14) {
            info.code = context->error_code & 1 ? SEGV_ACCERR : SEGV_MAPERR;
            info.addr = cr2;
        }
        break;
    }

    const uint64_t h = current->sig_actions[sig - 1].handler;
    if (h == SIG_DFL || h == SIG_IGN || (current->sig_blocked & signal_bit(sig)))
        return false;
    current->sig_pending |= signal_bit(sig);
    current->sig_info[sig - 1] = info;
    return true;
}

static void signal_restart(interrupt_context* context, process_t* current)
{
    context->rax = (uint64_t)current->syscall_nr;
    context->rip -= INT80_LEN;
}

static void signal_save_context(const interrupt_context* c, struct sigcontext_s* m)
{
    memset(m, 0, sizeof(*m));
    m->r8 = c->r8;
    m->r9 = c->r9;
    m->r10 = c->r10;
    m->r11 = c->r11;
    m->r12 = c->r12;
    m->r13 = c->r13;
    m->r14 = c->r14;
    m->r15 = c->r15;
    m->rdi = c->rdi;
    m->rsi = c->rsi;
    m->rbp = c->rbp;
    m->rbx = c->rbx;
    m->rdx = c->rdx;
    m->rax = c->rax;
    m->rcx = c->rcx;
    m->rsp = c->rsp;
    m->rip = c->rip;
    m->eflags = c->rflags;
    m->cs = (uint16_t)c->cs;
    m->ss = (uint16_t)c->ss;
    m->err = c->error_code;
    m->trapno = c->interrupt_number;
}

static bool signal_on_altstack(const process_t* proc, const uint64_t sp)
{
    return proc->altstack_size && sp > proc->altstack_sp && sp - proc->altstack_sp <= proc->altstack_size;
}

static struct sigaltstack_s signal_altstack_get(const process_t* proc, const uint64_t sp)
{
    struct sigaltstack_s ss = { .ss_sp = proc->altstack_sp, .ss_size = proc->altstack_size };
    if (!proc->altstack_size)
        ss.ss_flags = SS_DISABLE;
    else
        ss.ss_flags = (signal_on_altstack(proc, sp) ? SS_ONSTACK : 0) | proc->altstack_flags;
    return ss;
}

static long signal_altstack_set(process_t* proc, const struct sigaltstack_s* ss, const uint64_t sp)
{
    if (signal_on_altstack(proc, sp))
        return -EPERM;
    const int32_t mode = ss->ss_flags & ~SS_AUTODISARM;
    if (mode != 0 && mode != SS_ONSTACK && mode != SS_DISABLE)
        return -EINVAL;
    if (mode == SS_DISABLE) {
        proc->altstack_sp = proc->altstack_size = 0;
        proc->altstack_flags = 0;
        return 0;
    }
    if (ss->ss_size < MINSIGSTKSZ)
        return -ENOMEM;
    if (ss->ss_sp + ss->ss_size < ss->ss_sp || ss->ss_sp + ss->ss_size > USER_ADDR_LIMIT)
        return -EINVAL;
    proc->altstack_sp = ss->ss_sp;
    proc->altstack_size = ss->ss_size;
    proc->altstack_flags = ss->ss_flags & SS_AUTODISARM;
    return 0;
}

static bool signal_setup_frame(interrupt_context* context, process_t* current, const int sig)
{
    const struct k_sigaction* act = &current->sig_actions[sig - 1];
    if (act->handler >= USER_ADDR_LIMIT)
        return false;

    const bool on_alt = signal_on_altstack(current, context->rsp);
    const bool to_alt = (act->flags & SA_ONSTACK) && current->altstack_size && !on_alt;
    uint64_t sp = to_alt ? current->altstack_sp + current->altstack_size : context->rsp - RED_ZONE;
    sp -= sizeof(struct signal_frame);
    sp = (sp & ~15ULL) - 8;

    if ((to_alt || on_alt) && sp < current->altstack_sp)
        return false;
    const auto uframe = (struct signal_frame*)sp;

    struct signal_frame frame = {0};
    frame.uc.uc_stack = signal_altstack_get(current, context->rsp);
    signal_save_context(context, &frame.uc.uc_mcontext);

    const uint64_t return_mask = current->sig_restore_mask ? current->sig_saved_blocked : current->sig_blocked;
    current->sig_restore_mask = false;
    frame.uc.uc_mcontext.oldmask = return_mask;
    frame.uc.uc_sigmask = return_mask;
    frame.info.si_signo = sig;
    frame.info.si_code = current->sig_info[sig - 1].code;
    if (frame.info.si_code == SI_USER || frame.info.si_code == SI_KERNEL)
        frame.info.kill.si_pid = current->sig_info[sig - 1].pid;
    else
        frame.info.si_addr = current->sig_info[sig - 1].addr;

    const uint8_t tramp[] = { 0xB8, 0x0F, 0x00, 0x00, 0x00, 0xCD, 0x80, 0x90 };
    memcpy(frame.trampoline, tramp, sizeof(tramp));
    frame.pretcode = act->flags & SA_RESTORER ? act->restorer : (uint64_t)uframe->trampoline;

    if (copy_to_user(uframe, &frame, sizeof(frame)) < 0)
        return false;

    current->sig_blocked |= act->mask & ~unblockable;
    if (!(act->flags & SA_NODEFER))
        current->sig_blocked |= signal_bit(sig);
    current->sig_blocked &= ~unblockable;

    context->rip = act->handler;
    context->rsp = sp;
    context->rdi = (uint64_t)sig;
    context->rsi = (uint64_t)&uframe->info;
    context->rdx = (uint64_t)&uframe->uc;
    context->rax = 0;
    context->rflags &= ~(RFLAGS_DF | RFLAGS_TF);

    if (act->flags & SA_RESETHAND)
        memset(&current->sig_actions[sig - 1], 0, sizeof(struct k_sigaction));

    if (to_alt && (current->altstack_flags & SS_AUTODISARM)) {
        current->altstack_sp = current->altstack_size = 0;
        current->altstack_flags = 0;
    }
    return true;
}

void signal_deliver(interrupt_context* context)
{
    if ((context->cs & 3) != 3)
        return;
    process_t* current = signal_current_user();
    if (current == nullptr)
        return;

    const bool restartable = context->interrupt_number == SYSCALL_VECTOR && current->syscall_nr >= 0
                             && (int64_t)context->rax == -ERESTARTSYS;
    current->syscall_nr = -1;

    for (;;) {
        const uint64_t deliverable = current->sig_pending & ~current->sig_blocked;
        if (!deliverable) {
            if (restartable)
                signal_restart(context, current);
            if (current->sig_restore_mask) {
                current->sig_blocked = current->sig_saved_blocked;
                current->sig_restore_mask = false;
            }
            return;
        }
        const int sig = __builtin_ctzll(deliverable) + 1;
        current->sig_pending &= ~signal_bit(sig);

        if (signal_ignored(current, sig))
            continue;
        if (current->sig_actions[sig - 1].handler == SIG_DFL) {
            // TODO: core dumps, stop/continue
            current->sig_pending = 0;
            if (current->group_exit)
                process_exit();
            else
                process_exit_group(sig);
            return;
        }

        if (restartable) {
            if (current->sig_actions[sig - 1].flags & SA_RESTART)
                signal_restart(context, current);
            else
                context->rax = (uint64_t)-EINTR;
        }
        if (!signal_setup_frame(context, current, sig)) {
            process_exit_group(SIGSEGV);
        }
        return;
    }
}

long sys_rt_sigaction(const int sig, const struct k_sigaction* act, struct k_sigaction* oact, const size_t sigsetsize)
{
    process_t* current = signal_current_user();
    if (current == nullptr)
        return -ESRCH;
    if (sigsetsize != sizeof(uint64_t) || sig < 1 || sig > SIGNAL_MAX)
        return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP))
        return -EINVAL;

    struct k_sigaction next;
    if (act && copy_from_user(&next, act, sizeof(next)) < 0)
        return -EFAULT;

    if (act && next.handler != SIG_DFL && next.handler != SIG_IGN && next.handler >= USER_ADDR_LIMIT)
        return -EFAULT;
    const struct k_sigaction old = current->sig_actions[sig - 1];
    if (act) {
        next.mask &= ~unblockable;
        const uint64_t flags = irq_save();
        current->sig_actions[sig - 1] = next;

        if (signal_ignored(current, sig))
            current->sig_pending &= ~signal_bit(sig);
        irq_restore(flags);
    }
    if (oact && copy_to_user(oact, &old, sizeof(old)) < 0)
        return -EFAULT;
    return 0;
}

long sys_rt_sigprocmask(const int how, const uint64_t* set, uint64_t* oset, const size_t sigsetsize)
{
    process_t* current = signal_current_user();
    if (current == nullptr)
        return -ESRCH;
    if (sigsetsize != sizeof(uint64_t))
        return -EINVAL;
    const uint64_t old = current->sig_blocked;
    if (set) {
        uint64_t s;
        if (copy_from_user(&s, set, sizeof(s)) < 0)
            return -EFAULT;
        switch (how) {
        case SIG_BLOCK:
            current->sig_blocked |= s;
            break;
        case SIG_UNBLOCK:
            current->sig_blocked &= ~s;
            break;
        case SIG_SETMASK:
            current->sig_blocked = s;
            break;
        default:
            return -EINVAL;
        }
        current->sig_blocked &= ~unblockable;
    }
    if (oset && copy_to_user(oset, &old, sizeof(old)) < 0)
        return -EFAULT;
    return 0;
}

void sys_rt_sigreturn(interrupt_context* context)
{
    process_t* current = signal_current_user();
    if (current == nullptr)
        return;

    struct ucontext_s uc;
    if (copy_from_user(&uc, (const void*)context->rsp, sizeof(uc)) < 0) {
        process_exit_group(SIGSEGV);
        return;
    }
    const struct sigcontext_s* m = &uc.uc_mcontext;
    if (m->rip >= USER_ADDR_LIMIT || m->rsp >= USER_ADDR_LIMIT) {
        process_exit_group(SIGSEGV);
        return;
    }
    context->r8 = m->r8;
    context->r9 = m->r9;
    context->r10 = m->r10;
    context->r11 = m->r11;
    context->r12 = m->r12;
    context->r13 = m->r13;
    context->r14 = m->r14;
    context->r15 = m->r15;
    context->rdi = m->rdi;
    context->rsi = m->rsi;
    context->rbp = m->rbp;
    context->rbx = m->rbx;
    context->rdx = m->rdx;
    context->rax = m->rax;
    context->rcx = m->rcx;
    context->rsp = m->rsp;
    context->rip = m->rip;
    context->rflags = (context->rflags & ~RFLAGS_USER) | (m->eflags & RFLAGS_USER) | RFLAGS_IF;
    context->cs = USER_CS;
    context->ss = USER_SS;
    current->sig_blocked = uc.uc_sigmask & ~unblockable;
    current->syscall_nr = -1;

    signal_altstack_set(current, &uc.uc_stack, context->rsp);
}

long sys_sigaltstack(const struct sigaltstack_s* ss, struct sigaltstack_s* oss, const uint64_t user_rsp)
{
    process_t* current = signal_current_user();
    if (current == nullptr)
        return -ESRCH;
    const struct sigaltstack_s old = signal_altstack_get(current, user_rsp);
    if (ss) {
        struct sigaltstack_s next;
        if (copy_from_user(&next, ss, sizeof(next)) < 0)
            return -EFAULT;
        const long err = signal_altstack_set(current, &next, user_rsp);
        if (err)
            return err;
    }
    if (oss && copy_to_user(oss, &old, sizeof(old)) < 0)
        return -EFAULT;
    return 0;
}

long sys_rt_sigsuspend(const uint64_t* mask, const size_t sigsetsize)
{
    process_t* current = signal_current_user();
    if (current == nullptr)
        return -ESRCH;
    if (sigsetsize != sizeof(uint64_t))
        return -EINVAL;
    uint64_t m;
    if (copy_from_user(&m, mask, sizeof(m)) < 0)
        return -EFAULT;
    current->sig_saved_blocked = current->sig_blocked;
    current->sig_restore_mask = true;
    current->sig_blocked = m & ~unblockable;
    int err;
    do {
        err = process_sleep_interruptible(WAIT_CHANNEL_TIMER | current->pid);
    } while (err == 0);
    return -EINTR;
}

long sys_pause()
{
    process_t* current = signal_current_user();
    if (current == nullptr)
        return -ESRCH;
    int err;
    do {
        err = process_sleep_interruptible(WAIT_CHANNEL_TIMER | current->pid);
    } while (err == 0);
    return -EINTR;
}

long sys_rt_sigpending(uint64_t* set, const size_t sigsetsize)
{
    const process_t* current = signal_current_user();
    if (current == nullptr)
        return -ESRCH;
    if (sigsetsize != sizeof(uint64_t))
        return -EINVAL;
    const uint64_t pending = current->sig_pending & current->sig_blocked;
    return copy_to_user(set, &pending, sizeof(pending));
}

long sys_rt_sigtimedwait(const uint64_t* uset, struct siginfo_s* uinfo, const void* timeout, const size_t sigsetsize)
{
    process_t* current = signal_current_user();
    if (current == nullptr)
        return -ESRCH;
    if (sigsetsize != sizeof(uint64_t))
        return -EINVAL;
    uint64_t set;
    if (copy_from_user(&set, uset, sizeof(set)) < 0)
        return -EFAULT;
    set &= ~unblockable;

    uint64_t ticks = 0;
    bool poll_only = false;
    if (timeout) {
        struct timespec_k ts;
        if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
            return -EFAULT;
        if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 || ts.tv_sec < 0)
            return -EINVAL;
        poll_only = ts.tv_sec == 0 && ts.tv_nsec == 0;
        ticks = poll_only ? 0 : clock_ticks_from_timespec(&ts);
    }

    for (;;) {
        const uint64_t ready = current->sig_pending & set;
        if (ready) {
            const int sig = __builtin_ctzll(ready) + 1;
            current->sig_pending &= ~signal_bit(sig);
            if (uinfo) {
                struct siginfo_s info = {0};
                info.si_signo = sig;
                info.si_code = current->sig_info[sig - 1].code;
                info.kill.si_pid = current->sig_info[sig - 1].pid;
                if (copy_to_user(uinfo, &info, sizeof(info)) < 0)
                    return -EFAULT;
            }
            return sig;
        }
        if (poll_only)
            return -EAGAIN;
        current->sig_waiting = set;
        const int err = process_sleep_timeout(WAIT_CHANNEL_TIMER | current->pid, ticks);
        current->sig_waiting = 0;
        if (err == -ETIMEDOUT)
            return -EAGAIN;
        if (err && !(current->sig_pending & set))
            return -EINTR;
    }
}
