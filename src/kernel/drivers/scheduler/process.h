#ifndef PROCESS_H
#define PROCESS_H

#include <os/stdint.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/vm.h>
#include <hardware/interrupts/interrupts.h>
#include <utils/queue/queue.h>
#include <drivers/scheduler/signal.h>
#include <drivers/timer/callout.h>
#include <drivers/fs/vfs/vfs.h>

#define STACK_SIZE      PAGE_SIZE
#define KERNEL_STACK_PAGES 4
#define KERNEL_STACK_SIZE (KERNEL_STACK_PAGES * PAGE_SIZE)
#define MAXIMUM_NUMBER_OF_PROCESSES 64
#define USER_STACK_START    (USER_VIRTUAL_BASE + USER_MEMORY_SIZE)
#define NORMAL_PROC_ID -1
#define INIT_PROC_ID 1
#define IDLE_PROC_PID 0
#define WAIT_KEY_INT -2
#define PROC_MAX_FILE_DESCRIPTORS 256

#define WAIT_CHANNEL_FUTEX 0x10000000
#define WAIT_CHANNEL_CHILD 0x20000000
#define WAIT_CHANNEL_TIMER 0x30000000
#define WAIT_CHANNEL_POLL  0x38000000

typedef enum {
    PROC_SLOT_UNUSED = 0,
    PROC_SLOT_INIT,
    PROC_SLOT_READY,
    PROC_SLOT_RUNNING,
    PROC_SLOT_SLEEPING,
    PROC_SLOT_KILLED
} process_state_t;

struct proc_files {
    int ref_count;
    struct file_s *file[PROC_MAX_FILE_DESCRIPTORS];
    bool cloexec[PROC_MAX_FILE_DESCRIPTORS];
    char cwd[VFS_PATH_MAX];
    uint32_t umask;
};

typedef struct {
    struct dlist_node link;
    int pid;
    int tgid;
    int ppid;
    int wait_id;
    process_state_t state;
    uint64_t page_map;
    uint64_t context;
    uint64_t stack;
    interrupt_context *frame;
    struct vm_space *vm;
    struct proc_files *files;
    int exit_status;
    bool detached;
    bool group_exit;
    bool timed_out;
    uint64_t fs_base;
    uint64_t clear_child_tid;
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t sig_saved_blocked;
    bool sig_restore_mask;
    uint64_t sig_waiting;
    struct k_sigaction sig_actions[SIGNAL_MAX];
    struct signal_info sig_info[SIGNAL_MAX];
    int64_t syscall_nr;
    uint64_t altstack_sp;
    uint64_t altstack_size;
    int32_t altstack_flags;
    bool interruptible;
    struct callout itimer;
    uint64_t itimer_interval;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
} process_t;

typedef struct {
    process_t *current;
    struct dlist ready;
    struct dlist waiting;
    struct dlist killed;
} scheduler_t;

void process_initialize();
scheduler_t *scheduler_get();

void process_yield();
void context_switch(uint64_t *old, uint64_t new);

void process_sleep(int wait_id);
void process_wakeup(int wait_id);
int process_wakeup_n(int wait_id, int count);
void process_wake(process_t *proc);

int process_sleep_interruptible(int wait_id);
int process_sleep_timeout(int wait_id, uint64_t ticks);
void process_interrupt(process_t *proc);
process_t *process_find(int pid);
int process_count();

void process_exit();
void process_exit_status(int status);
void process_exit_group(int status);
long process_wait4(int pid, int *status, int options);
long process_clone(uint64_t flags, uint64_t stack, int *parent_tid, int *child_tid, uint64_t tls);
int process_fork();
int process_exec(process_t *proc, const char *path, char *const argv[], char *const envp[]);
void process_set_fs_base(process_t *proc, uint64_t base);
void signal_kill_thread(process_t *proc);

struct proc_files *files_create();
struct proc_files *files_clone(const struct proc_files *src);
void files_release(struct proc_files *files);

#endif //PROCESS_H
