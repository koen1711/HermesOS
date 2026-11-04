#include "process.h"
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <hardware/gdt/tss.h>
#include <hardware/smp/smp.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/elf/ELF.h>
#include <drivers/scheduler/futex.h>
#include <drivers/syscalls/uaccess.h>
#include <drivers/terminal/tty.h>
#include <hardware/memory/alloc.h>
#include <utils/random/random.h>
#include <utils/str/str.h>
#include <utils/sync/spinlock.h>
#include <hardware/interrupts/irq.h>
#include <os/errno.h>

#include "drivers/terminal/terminal.h"


#define INIT_PROC_PATH "/shell"

#define MSR_FS_BASE 0xC0000100
#define EXEC_ARG_MAX (128 * 1024)
#define WNOHANG 1
#define WNOWAIT 0x01000000

enum clone_flags {
    CLONE_VM = 0x00000100,
    CLONE_FS = 0x00000200,
    CLONE_FILES = 0x00000400,
    CLONE_SIGHAND = 0x00000800,
    CLONE_VFORK = 0x00004000,
    CLONE_PARENT = 0x00008000,
    CLONE_THREAD = 0x00010000,
    CLONE_SETTLS = 0x00080000,
    CLONE_PARENT_SETTID = 0x00100000,
    CLONE_CHILD_CLEARTID = 0x00200000,
    CLONE_CHILD_SETTID = 0x01000000
};

enum auxv_type {
    AT_NULL = 0,
    AT_PHDR = 3,
    AT_PHENT = 4,
    AT_PHNUM = 5,
    AT_PAGESZ = 6,
    AT_BASE = 7,
    AT_FLAGS = 8,
    AT_ENTRY = 9,
    AT_UID = 11,
    AT_EUID = 12,
    AT_GID = 13,
    AT_EGID = 14,
    AT_HWCAP = 16,
    AT_CLKTCK = 17,
    AT_SECURE = 23,
    AT_RANDOM = 25,
    AT_EXECFN = 31
};

static process_t processes[MAXIMUM_NUMBER_OF_PROCESSES];
static int next_pid = 1;

static scheduler_t scheduler;
static spinlock_t sched_lock = { .name = "sched" };
static scheduler_t per_cpu[MAX_CPUS];
static uint8_t fpu_default[512] __attribute__((aligned(16)));

static process_t *find_free_process_slot();
static process_t *create_process();
static void process_free(process_t *proc);

static void set_tss(const process_t *proc);
static void schedule();
static void sched_switch(uint64_t flags);
static volatile uint64_t poll_wake_seq;
volatile uint64_t poll_wake_calls;
volatile uint64_t poll_wake_woken;

static void reap_detached();
static void switch_process(process_t *prev, process_t *next);

static void waiting_insert(process_t *proc);
static process_t *waiting_first(int wait_id);
static process_t *remove_waiting_process(int wait_id);

static void init_idle_process();
static void init_shell_process();

static void wrmsr(const uint32_t msr, const uint64_t value) {
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

void process_initialize() {
    asm volatile("fninit");
    const uint32_t mxcsr = 0x1F80;
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    asm volatile("fxsave64 %0" : "=m"(fpu_default));

    init_idle_process();
    init_shell_process();
}

scheduler_t *scheduler_get() {
    return &per_cpu[cpu_this()->index];
}

void process_yield() {
    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    if (dlist_empty(&scheduler.ready)) {
        spinlock_release_irqrestore(&sched_lock, flags);
        return;
    }

    process_t *proc = scheduler_get()->current;
    proc->state = PROC_SLOT_READY;

    if (proc->pid != IDLE_PROC_PID) {
        dlist_insert_tail(&scheduler.ready, &proc->link);
    }

    sched_switch(flags);
}

void process_sleep(const int wait_id) {
    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    process_t *proc = scheduler_get()->current;
    proc->state = PROC_SLOT_SLEEPING;
    proc->wait_id = wait_id;

    waiting_insert(proc);

    sched_switch(flags);
}

static void kick_idle_cpu() {
    const int self = cpu_this()->index;
    if (per_cpu[self].current == per_cpu[self].idle) {
        return;
    }
    for (int i = 0; i < cpu_count(); i++) {
        if (i != self && per_cpu[i].current != nullptr && per_cpu[i].current == per_cpu[i].idle) {
            smp_kick(i);
            return;
        }
    }
}

void process_wakeup(const int wait_id) {
    process_wakeup_n(wait_id, -1);
}

int process_wakeup_n(const int wait_id, const int count) {
    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    int woken = 0;
    while (count < 0 || woken < count) {
        process_t *proc = remove_waiting_process(wait_id);
        if (proc == nullptr) {
            break;
        }
        proc->state = PROC_SLOT_READY;
        dlist_insert_tail(&scheduler.ready, &proc->link);
        woken++;
    }
    spinlock_release_irqrestore(&sched_lock, flags);
    if (woken && irq_in_interrupt()) {
        kick_idle_cpu();
    }
    return woken;
}

void process_wake(process_t *proc) {
    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    const bool woke = proc->state == PROC_SLOT_SLEEPING;
    if (woke) {
        rb_remove(&scheduler.waiting, &proc->wait_node);
        proc->state = PROC_SLOT_READY;
        dlist_insert_tail(&scheduler.ready, &proc->link);
    }
    spinlock_release_irqrestore(&sched_lock, flags);
    if (woke && irq_in_interrupt()) {
        kick_idle_cpu();
    }
}

static bool signal_wakes(const process_t *proc) {
    return proc->sig_pending & (~proc->sig_blocked | proc->sig_waiting);
}

static int sleep_interruptible(const int wait_id, const uint64_t *seq) {
    process_t *proc = scheduler_get()->current;
    proc->interruptible = true;
    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    if (signal_wakes(proc) || (seq && *seq != __atomic_load_n(&poll_wake_seq, __ATOMIC_SEQ_CST))) {
        spinlock_release_irqrestore(&sched_lock, flags);
    } else {
        proc->state = PROC_SLOT_SLEEPING;
        proc->wait_id = wait_id;
        waiting_insert(proc);
        sched_switch(flags);
    }
    proc->interruptible = false;
    return signal_wakes(proc) ? -EINTR : 0;
}

int process_sleep_interruptible(const int wait_id) {
    return sleep_interruptible(wait_id, nullptr);
}

uint64_t process_poll_seq() {
    return __atomic_load_n(&poll_wake_seq, __ATOMIC_SEQ_CST);
}

void process_wakeup_poll(const uint64_t interest) {
    __atomic_add_fetch(&poll_wake_seq, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&poll_wake_calls, 1, __ATOMIC_RELAXED);
    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    int woken = 0;
    process_t *proc = waiting_first(WAIT_CHANNEL_POLL);
    while (proc != nullptr && proc->wait_id == WAIT_CHANNEL_POLL) {
        const struct rb_node *next = rb_next(&proc->wait_node);
        if (proc->poll_interest & interest) {
            rb_remove(&scheduler.waiting, &proc->wait_node);
            proc->state = PROC_SLOT_READY;
            dlist_insert_tail(&scheduler.ready, &proc->link);
            woken++;
        }
        proc = next ? rb_entry(next, process_t, wait_node) : nullptr;
    }
    spinlock_release_irqrestore(&sched_lock, flags);
    __atomic_add_fetch(&poll_wake_woken, woken, __ATOMIC_RELAXED);
    if (woken && irq_in_interrupt()) {
        kick_idle_cpu();
    }
}

static void sleep_timeout(void *arg) {
    process_t *proc = arg;
    proc->timed_out = true;
    process_wake(proc);
}

static int sleep_timeout_checked(const int wait_id, const uint64_t ticks, const uint64_t *seq) {
    process_t *proc = scheduler_get()->current;
    proc->timed_out = false;
    struct callout timer;
    callout_init(&timer);
    timer.mpsafe = true;
    if (ticks) {
        callout_reset(&timer, ticks, sleep_timeout, proc);
    }
    const int err = sleep_interruptible(wait_id, seq);
    callout_stop(&timer);
    if (err) {
        return err;
    }
    return proc->timed_out ? -ETIMEDOUT : 0;
}

int process_sleep_timeout(const int wait_id, const uint64_t ticks) {
    return sleep_timeout_checked(wait_id, ticks, nullptr);
}

int process_sleep_poll(const uint64_t seq, const uint64_t ticks) {
    return sleep_timeout_checked(WAIT_CHANNEL_POLL, ticks, &seq);
}

void process_interrupt(process_t *proc) {
    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    const bool woke = proc->state == PROC_SLOT_SLEEPING && proc->interruptible;
    if (woke) {
        rb_remove(&scheduler.waiting, &proc->wait_node);
        proc->state = PROC_SLOT_READY;
        dlist_insert_tail(&scheduler.ready, &proc->link);
    }
    spinlock_release_irqrestore(&sched_lock, flags);
    if (woke && irq_in_interrupt()) {
        kick_idle_cpu();
    }
}

process_t *process_find(const int pid) {
    for (int i = 0; i<MAXIMUM_NUMBER_OF_PROCESSES; i++) {
        process_t *proc = &processes[i];
        if (proc->pid == pid && pid != IDLE_PROC_PID && proc->state != PROC_SLOT_UNUSED
            && proc->state != PROC_SLOT_KILLED) {
            return proc;
        }
    }
    return nullptr;
}

int process_count() {
    int count = 0;
    for (int i = 0; i<MAXIMUM_NUMBER_OF_PROCESSES; i++) {
        if (processes[i].pid != IDLE_PROC_PID && processes[i].state != PROC_SLOT_UNUSED
            && processes[i].state != PROC_SLOT_KILLED) {
            count++;
        }
    }
    return count;
}

void process_set_fs_base(process_t *proc, const uint64_t base) {
    proc->fs_base = base;
    if (proc == scheduler_get()->current) {
        wrmsr(MSR_FS_BASE, base);
    }
}

struct proc_files *files_create() {
    struct proc_files *files = malloc(sizeof(struct proc_files));
    if (files == nullptr) return nullptr;
    memset(files, 0, sizeof(*files));
    files->ref_count = 1;
    spinlock_init(&files->lock, "files");
    files->cwd[0] = '/';
    files->root[0] = '/';
    files->umask = 022;
    return files;
}

struct proc_files *files_clone(struct proc_files *src) {
    struct proc_files *files = malloc(sizeof(struct proc_files));
    if (files == nullptr) return nullptr;
    uint64_t flags;
    spinlock_acquire_irqsave(&src->lock, &flags);
    memcpy(files, src, sizeof(*files));
    for (int i = 0; i<PROC_MAX_FILE_DESCRIPTORS; i++) {
        if (files->file[i] != nullptr) {
            __atomic_fetch_add(&files->file[i]->ref_count, 1, __ATOMIC_RELAXED);
        }
    }
    spinlock_release_irqrestore(&src->lock, flags);
    files->ref_count = 1;
    spinlock_init(&files->lock, "files");
    return files;
}

void files_release(struct proc_files *files) {
    if (files == nullptr || --files->ref_count > 0) {
        return;
    }
    for (int i = 0; i<PROC_MAX_FILE_DESCRIPTORS; i++) {
        if (files->file[i] != nullptr) {
            vfs_close((int)files->file[i]->id);
        }
    }
    free(files);
}

static bool parent_ignores_children(const process_t *parent) {
    const struct k_sigaction *act = &parent->sig_actions[SIGCHLD - 1];
    return act->handler == SIG_IGN || (act->flags & SA_NOCLDWAIT);
}

void process_exit() {
    process_t *proc = scheduler_get()->current;

    proc->itimer_interval = 0;
    callout_drain(&proc->itimer);

    if (proc->clear_child_tid && proc->vm != nullptr) {
        constexpr int zero = 0;
        if (vm_write(proc->vm, proc->clear_child_tid, &zero, sizeof(zero)) == 0) {
            futex_wake(proc->vm, proc->clear_child_tid, 1);
        }
    }
    files_release(proc->files);
    proc->files = nullptr;

    if (proc->vm != nullptr) {
        proc->page_map = mmu_kernel_page_map();
        switch_vm(proc->page_map);
        vm_release(proc->vm);
        proc->vm = nullptr;
    }

    const bool leader = proc->pid == proc->tgid;
    if (leader) {
        for (int i = 0; i<MAXIMUM_NUMBER_OF_PROCESSES; i++) {
            process_t *child = &processes[i];
            if (child->state != PROC_SLOT_UNUSED && child->ppid == proc->tgid && child != proc) {
                child->ppid = 0;
                child->detached = true;
            }
        }
    }
    const process_t *parent = process_find(proc->ppid);
    if (!leader || parent == nullptr || parent_ignores_children(parent)) {
        proc->detached = true;
    }

    if (!proc->detached) {
        signal_send(proc->ppid, SIGCHLD);
        process_wakeup(WAIT_CHANNEL_CHILD | proc->ppid);
    }
    if (leader)
        process_wakeup_poll(~0ULL);

    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    proc->state = PROC_SLOT_KILLED;
    dlist_insert_tail(&scheduler.killed, &proc->link);
    sched_switch(flags);
}

void process_exit_status(const int status) {
    scheduler_get()->current->exit_status = status;
    process_exit();
}

static void kill_other_threads(const process_t *proc, const int status) {
    for (int i = 0; i<MAXIMUM_NUMBER_OF_PROCESSES; i++) {
        process_t *other = &processes[i];
        if (other == proc || other->tgid != proc->tgid || other->state == PROC_SLOT_UNUSED
            || other->state == PROC_SLOT_KILLED || other->pid == IDLE_PROC_PID) {
            continue;
        }
        other->group_exit = true;
        other->exit_status = status;
        signal_kill_thread(other);
    }
}

void process_exit_group(const int status) {
    process_t *proc = scheduler_get()->current;
    proc->group_exit = true;
    kill_other_threads(proc, status);
    process_exit_status(status);
}

long process_wait4(const int pid, int *status, const int options) {
    const process_t *current = scheduler_get()->current;
    for (;;) {
        bool have_child = false;
        for (int i = 0; i<MAXIMUM_NUMBER_OF_PROCESSES; i++) {
            process_t *proc = &processes[i];
            if (proc->state == PROC_SLOT_UNUSED || proc->detached || proc->pid != proc->tgid
                || proc->ppid != current->tgid || (pid > 0 && proc->pid != pid)) {
                continue;
            }
            have_child = true;
            if (proc->state == PROC_SLOT_KILLED) {
                const int child = proc->pid;
                if (status != nullptr) {
                    *status = proc->exit_status;
                }

                if (options & WNOWAIT)
                    return child;

                uint64_t flags;
                spinlock_acquire_irqsave(&sched_lock, &flags);
                dlist_remove(&scheduler.killed, &proc->link);
                spinlock_release_irqrestore(&sched_lock, flags);
                process_free(proc);
                return child;
            }
        }
        if (!have_child) {
            return -ECHILD;
        }
        if (options & WNOHANG) {
            return 0;
        }
        if (process_sleep_interruptible(WAIT_CHANNEL_CHILD | current->tgid) < 0) {
            return -ERESTARTSYS;
        }
    }
}

long process_clone(const uint64_t flags, const uint64_t stack, int *parent_tid, int *child_tid, const uint64_t tls) {
    process_t *current = scheduler_get()->current;
    if (current->vm == nullptr || current->files == nullptr) {
        return -EINVAL;
    }
    if ((flags & CLONE_THREAD) && !(flags & CLONE_VM)) {
        return -EINVAL;
    }

    process_t *proc = create_process();
    if (proc == nullptr) {
        return -EAGAIN;
    }

    const bool share_vm = (flags & CLONE_VM) && !(flags & CLONE_VFORK);
    if (share_vm) {
        vm_acquire(current->vm);
        proc->vm = current->vm;
    }
    else {
        proc->vm = vm_clone(current->vm);
    }
    if (flags & CLONE_FILES) {
        current->files->ref_count++;
        proc->files = current->files;
    }
    else {
        proc->files = files_clone(current->files);
    }
    if (proc->vm == nullptr || proc->files == nullptr) {
        process_free(proc);
        return -ENOMEM;
    }
    proc->page_map = proc->vm->page_map;

    memcpy(proc->frame, current->frame, sizeof(interrupt_context));
    proc->frame->rax = 0;
    if (stack) {
        proc->frame->rsp = stack;
    }
    asm volatile("fxsave64 %0" : "=m"(proc->fpu_state));
    proc->fs_base = flags & CLONE_SETTLS ? tls : current->fs_base;

    memcpy(proc->sig_actions, current->sig_actions, sizeof(proc->sig_actions));
    proc->sig_blocked = current->sig_blocked;
    memcpy(proc->groups, current->groups, sizeof(proc->groups));
    proc->group_count = current->group_count;
    proc->affinity = current->affinity;
    proc->tty = current->tty;
    if (!share_vm) {
        proc->altstack_sp = current->altstack_sp;
        proc->altstack_size = current->altstack_size;
        proc->altstack_flags = current->altstack_flags;
    }

    if (flags & CLONE_THREAD) {
        proc->tgid = current->tgid;
        proc->ppid = current->ppid;
    }
    else {
        proc->ppid = flags & CLONE_PARENT ? current->ppid : current->tgid;
    }
    if (flags & CLONE_CHILD_CLEARTID) {
        proc->clear_child_tid = (uint64_t)child_tid;
    }
    if (flags & CLONE_CHILD_SETTID) {
        vm_write(proc->vm, (uint64_t)child_tid, &proc->pid, sizeof(proc->pid));
    }
    if (flags & CLONE_PARENT_SETTID) {
        copy_to_user(parent_tid, &proc->pid, sizeof(proc->pid));
    }

    uint64_t irq;
    spinlock_acquire_irqsave(&sched_lock, &irq);
    proc->state = PROC_SLOT_READY;
    dlist_insert_tail(&scheduler.ready, &proc->link);
    spinlock_release_irqrestore(&sched_lock, irq);
    kick_idle_cpu();

    return proc->pid;
}

process_t *process_create_kernel(void (*fn)(void *), void *arg) {
    process_t *proc = create_process();
    if (proc == nullptr) {
        return nullptr;
    }
    uint64_t *regs = (uint64_t *)proc->context;
    regs[2] = (uint64_t)arg;
    regs[3] = (uint64_t)fn;
    regs[6] = (uint64_t)kthread_entry;

    uint64_t irq;
    spinlock_acquire_irqsave(&sched_lock, &irq);
    proc->state = PROC_SLOT_READY;
    dlist_insert_tail(&scheduler.ready, &proc->link);
    spinlock_release_irqrestore(&sched_lock, irq);
    kick_idle_cpu();
    return proc;
}

int process_fork() {
    return (int)process_clone(SIGCHLD, 0, nullptr, nullptr, 0);
}

static size_t count_strings(char *const list[], size_t *bytes) {
    size_t n = 0;
    for (; list != nullptr && list[n] != nullptr; n++) {
        *bytes += strlen(list[n]) + 1;
    }
    return n;
}

static uint64_t push_string(uint8_t *image, const uint64_t image_base, uint64_t *top, const char *s) {
    const size_t len = strlen(s) + 1;
    *top -= len;
    memcpy(image + (*top - image_base), s, len);
    return *top;
}

static int setup_stack(struct vm_space *vm, const char *path, char *const argv[], char *const envp[],
                       const struct elf_image *img, uint64_t *sp_out) {
    size_t bytes = strlen(path) + 1 + 16;
    const size_t argc = count_strings(argv, &bytes);
    const size_t envc = count_strings(envp, &bytes);
    if (bytes > EXEC_ARG_MAX) {
        return -E2BIG;
    }
    constexpr size_t auxc = 18;
    const size_t words = 1 + argc + 1 + envc + 1 + auxc * 2;
    const size_t total = PAGE_ALIGN_UP(bytes + words * 8 + 64);

    int err = vm_map(vm, USER_STACK_TOP - USER_STACK_LIMIT, USER_STACK_LIMIT, VM_READ | VM_WRITE, VMA_STACK, true,
                     nullptr);
    if (err) {
        return err;
    }
    uint8_t *image = malloc(total);
    if (image == nullptr) {
        return -ENOMEM;
    }

    const uint64_t base = USER_STACK_TOP - total;
    uint64_t top = USER_STACK_TOP;

    const uint64_t execfn = push_string(image, base, &top, path);
    top -= 16;
    const uint64_t random = top;
    random_fill(image + (top - base), 16);
    uint64_t *arg_ptrs = malloc((argc + envc + 1) * sizeof(uint64_t));
    if (arg_ptrs == nullptr) {
        free(image);
        return -ENOMEM;
    }

    for (size_t i = 0; i<argc; i++) {
        arg_ptrs[i] = push_string(image, base, &top, argv[i]);
    }
    for (size_t i = 0; i<envc; i++) {
        arg_ptrs[argc + i] = push_string(image, base, &top, envp[i]);
    }

    const uint64_t strings_end = top;
    top &= ~15ULL;
    if (words % 2) {
        top -= 8;
    }
    top -= words * 8;
    memset(image + (top + words * 8 - base), 0, strings_end - (top + words * 8));
    const auto w = (uint64_t *)(image + (top - base));
    size_t k = 0;
    w[k++] = argc;
    for (size_t i = 0; i<argc; i++) w[k++] = arg_ptrs[i];
    w[k++] = 0;
    for (size_t i = 0; i<envc; i++) w[k++] = arg_ptrs[argc + i];
    w[k++] = 0;
    const uint64_t auxv[][2] = {
        { AT_PHDR, img->phdr }, { AT_PHENT, img->phent }, { AT_PHNUM, img->phnum },
        { AT_PAGESZ, PAGE_SIZE }, { AT_BASE, img->interp_base }, { AT_FLAGS, 0 },
        { AT_ENTRY, img->program_entry }, { AT_UID, 0 }, { AT_EUID, 0 }, { AT_GID, 0 }, { AT_EGID, 0 },
        { AT_HWCAP, 0 }, { AT_CLKTCK, 100 }, { AT_SECURE, 0 }, { AT_RANDOM, random },
        { AT_EXECFN, execfn }, { AT_NULL, 0 }, { AT_NULL, 0 }
    };
    for (size_t i = 0; i<auxc; i++) {
        w[k++] = auxv[i][0];
        w[k++] = auxv[i][1];
    }

    err = vm_write(vm, top, image + (top - base), USER_STACK_TOP - top);
    free(arg_ptrs);
    free(image);
    *sp_out = top;
    return err;
}

int process_exec(process_t *proc, const char *path, char *const argv[], char *const envp[]) {
    vfs_stat_t stat = {0};
    uint8_t magic[4] = {0};

    const int fd = vfs_open(path, O_RDONLY);
    if (fd<0) {
        return fd;
    }
    if (vfs_stat(fd, &stat)<0 || stat.type != INODE_FILE || !(stat.perm & 0111)) {
        vfs_close(fd);
        return -EACCES;
    }
    struct vm_space *vm = vm_create();
    if (vm == nullptr) {
        vfs_close(fd);
        return -ENOMEM;
    }

    int err = vfs_read(fd, magic, sizeof(magic)) == sizeof(magic) ? 0 : -ENOEXEC;
    uint64_t entry = 0;
    uint64_t sp = 0;
    if (err == 0 && elf_is_elf(magic, sizeof(magic))) {
        struct elf_image img = {0};
        err = elf_load(vm, fd, proc->files ? proc->files->root : "/", &img);
        if (err == 0) {
            err = setup_stack(vm, path, argv, envp, &img, &sp);
        }
        entry = img.entry;
        vm->brk_start = vm->brk = img.brk_start;
    }
    else if (err == 0) {
        err = -ENOEXEC;
    }
    vfs_close(fd);
    if (err) {
        vm_release(vm);
        return err;
    }

    kill_other_threads(proc, SIGKILL);
    struct vm_space *old = proc->vm;
    proc->vm = vm;
    proc->page_map = vm->page_map;
    if (proc == scheduler_get()->current) {
        switch_vm(vm->page_map);
    }
    vm_release(old);

    if (proc->files != nullptr && proc->files->ref_count > 1) {
        struct proc_files *own = files_clone(proc->files);
        if (own != nullptr) {
            files_release(proc->files);
            proc->files = own;
        }
    }
    for (int i = 0; proc->files != nullptr && i<PROC_MAX_FILE_DESCRIPTORS; i++) {
        if (proc->files->file[i] != nullptr && proc->files->cloexec[i]) {
            uint64_t flags;
            spinlock_acquire_irqsave(&proc->files->lock, &flags);
            const int id = (int)proc->files->file[i]->id;
            proc->files->file[i] = nullptr;
            proc->files->cloexec[i] = false;
            spinlock_release_irqrestore(&proc->files->lock, flags);
            vfs_close(id);
        }
    }

    for (int i = 0; i<SIGNAL_MAX; i++) {
        if (proc->sig_actions[i].handler != SIG_IGN) {
            memset(&proc->sig_actions[i], 0, sizeof(proc->sig_actions[i]));
        }
    }
    proc->altstack_sp = proc->altstack_size = 0;
    proc->altstack_flags = 0;
    proc->clear_child_tid = 0;
    process_set_fs_base(proc, 0);
    memcpy(proc->fpu_state, fpu_default, sizeof(fpu_default));
    if (proc == scheduler_get()->current) {
        asm volatile("fxrstor64 %0" : : "m"(proc->fpu_state));
    }

    memset(proc->frame, 0, sizeof(interrupt_context));
    proc->frame->cs = 0x20 | 3;
    proc->frame->rip = entry;
    proc->frame->ss = 0x18 | 3;
    proc->frame->rsp = sp;
    proc->frame->rflags = 0x202;

    return 0;
}

static process_t *find_free_process_slot() {
    for (int i = 0; i<MAXIMUM_NUMBER_OF_PROCESSES; i++) {
        if (processes[i].state == PROC_SLOT_UNUSED) {
            return &processes[i];
        }
    }
    return nullptr;
}

static void set_tss(const process_t *proc) {
    struct cpu *cpu = cpu_this();
    cpu->tss->rsp0 = cpu->kernel_rsp = proc->stack + KERNEL_STACK_SIZE;
}

static void reap_detached() {
    while (!dlist_empty(&scheduler.killed)) {
        process_t *victim = nullptr;
        uint64_t flags;
        spinlock_acquire_irqsave(&sched_lock, &flags);
        for (struct dlist_node *node = dlist_first(&scheduler.killed); node; node = dlist_next(node)) {
            const auto proc = dlist_entry(node, process_t, link);
            if (proc->detached) {
                dlist_remove(&scheduler.killed, node);
                victim = proc;
                break;
            }
        }
        spinlock_release_irqrestore(&sched_lock, flags);
        if (victim == nullptr) {
            return;
        }
        process_free(victim);
    }
}

static void schedule() {
    scheduler_t *cpu = scheduler_get();
    process_t *prev = cpu->current;
    process_t *next;

    if (dlist_empty(&scheduler.ready)) {
        next = cpu->idle;
    }
    else {
        struct dlist_node *node = dlist_first(&scheduler.ready);
        dlist_remove(&scheduler.ready, node);
        next = dlist_entry(node, process_t, link);
    }

    next->state = PROC_SLOT_RUNNING;
    cpu->current = next;

    if (prev != next) {
        switch_process(prev, next);
    }
}

static void sched_switch(const uint64_t flags) {
    const bool had_net_lock = net_lock_drop();
    const bool had_kernel_lock = kernel_lock_drop();
    schedule();
    spinlock_release_irqrestore(&sched_lock, flags);
    if (had_kernel_lock) {
        kernel_lock_enter();
        reap_detached();
    }
    if (had_net_lock) {
        net_lock_enter();
    }
}

void process_entry_unlock() {
    spinlock_release(&sched_lock);
}

static void switch_process(process_t *prev, process_t *next) {
    set_tss(next);
    asm volatile("fxsave64 %0" : "=m"(prev->fpu_state));
    asm volatile("fxrstor64 %0" : : "m"(next->fpu_state));
    wrmsr(MSR_FS_BASE, next->fs_base);
    switch_vm(next->page_map);
    context_switch(&prev->context, next->context);
}

static void waiting_insert(process_t *proc) {
    struct rb_node *parent = nullptr;
    struct rb_node **link = &scheduler.waiting.root;
    while (*link) {
        parent = *link;
        const process_t *other = rb_entry(parent, process_t, wait_node);
        link = &parent->child[other->wait_id <= proc->wait_id ? RB_RIGHT : RB_LEFT];
    }
    rb_insert(&scheduler.waiting, parent, link, &proc->wait_node);
}

static process_t *waiting_first(const int wait_id) {
    process_t *found = nullptr;
    struct rb_node *node = scheduler.waiting.root;
    while (node) {
        const auto proc = rb_entry(node, process_t, wait_node);
        if (proc->wait_id < wait_id) {
            node = node->child[RB_RIGHT];
        } else {
            if (proc->wait_id == wait_id) {
                found = proc;
            }
            node = node->child[RB_LEFT];
        }
    }
    return found;
}

static process_t *remove_waiting_process(const int wait_id) {
    process_t *proc = waiting_first(wait_id);
    if (proc != nullptr) {
        rb_remove(&scheduler.waiting, &proc->wait_node);
    }
    return proc;
}

static void init_idle_process() {
    process_t *proc = find_free_process_slot();
    proc->pid = IDLE_PROC_PID;
    proc->page_map = read_cr3();
    proc->state = PROC_SLOT_RUNNING;
    memcpy(proc->fpu_state, fpu_default, sizeof(fpu_default));
    per_cpu[0].current = per_cpu[0].idle = proc;
}

bool process_create_idle(const int cpu_index, const uint64_t stack) {
    process_t *proc = find_free_process_slot();
    if (proc == nullptr) {
        return false;
    }
    memset(proc, 0, sizeof(process_t));
    proc->pid = IDLE_PROC_PID;
    proc->stack = stack;
    proc->page_map = mmu_kernel_page_map();
    proc->state = PROC_SLOT_RUNNING;
    memcpy(proc->fpu_state, fpu_default, sizeof(fpu_default));
    per_cpu[cpu_index].current = per_cpu[cpu_index].idle = proc;
    return true;
}

int process_spawn_shell(const int tty) {
    char *argv[] = { "sh", nullptr };
    char *envp[] = { "PATH=/bin:/usr/bin", "HOME=/", "TERM=vt100", nullptr };

    process_t *proc = create_process();
    if (proc == nullptr) {
        terminal_printf("process_spawn_shell: cannot create process\n");
        return -EAGAIN;
    }
    proc->tty = tty + 1;
    proc->files = files_create();
    const int err = proc->files == nullptr ? -ENOMEM : tty_install(proc->files, tty);
    const int exec_err = err ? err : process_exec(proc, INIT_PROC_PATH, argv, envp);
    if (exec_err) {
        terminal_printf("process_spawn_shell: cannot load %s (error %d)\n", INIT_PROC_PATH, -exec_err);
        process_free(proc);
        return exec_err;
    }

    const int pid = proc->pid;
    uint64_t flags;
    spinlock_acquire_irqsave(&sched_lock, &flags);
    proc->state = PROC_SLOT_READY;
    dlist_insert_tail(&scheduler.ready, &proc->link);
    spinlock_release_irqrestore(&sched_lock, flags);
    kick_idle_cpu();
    return pid;
}

static void init_shell_process() {
    process_spawn_shell(0);
}

static void process_free(process_t *proc) {
    files_release(proc->files);
    vm_release(proc->vm);
    free_kernel_pages((void*)proc->stack, KERNEL_STACK_PAGES);
    memset(proc, 0, sizeof(process_t));
}

static process_t *create_process() {
    reap_detached();
    process_t *proc = find_free_process_slot();

    if (proc == nullptr) {
        return nullptr;
    }
    const uint64_t stack = (uint64_t)alloc_kernel_pages(KERNEL_STACK_PAGES);
    if (stack == 0) {
        return nullptr;
    }

    memset(proc, 0, sizeof(process_t));
    proc->stack = stack;
    proc->state = PROC_SLOT_INIT;
    proc->pid = next_pid++;
    proc->tgid = proc->pid;
    proc->wait_id = 0;
    callout_init(&proc->itimer);
    memcpy(proc->fpu_state, fpu_default, sizeof(fpu_default));

    memset((void*)proc->stack, 0, KERNEL_STACK_SIZE);
    const uint64_t stack_top = proc->stack + KERNEL_STACK_SIZE;

    proc->context = stack_top - sizeof(interrupt_context) - 7*8;
    *(uint64_t*)(proc->context + 6*8) = (uint64_t)process_entry;

    proc->frame = (interrupt_context*)(stack_top - sizeof(interrupt_context));

    proc->frame->cs = 0x20 | 3;
    proc->frame->rip = USER_VIRTUAL_BASE;
    proc->frame->ss = 0x18 | 3;
    proc->frame->rsp = USER_STACK_START;
    proc->frame->rflags = 0x202;

    proc->page_map = mmu_kernel_page_map();
    return proc;
}
