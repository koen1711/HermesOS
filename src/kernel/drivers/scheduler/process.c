#include "process.h"
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <hardware/gdt/tss.h>
#include <drivers/fs/vfs/vfs.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

#include "drivers/terminal/terminal.h"


#define INIT_PROC_PATH "/shell"

static process_t processes[MAXIMUM_NUMBER_OF_PROCESSES];
static int next_pid = 1;
static scheduler_t scheduler;

static process_t *find_free_process_slot();
static process_t *create_process();

static void set_tss(const process_t *proc);
static void schedule();
static void switch_process(process_t *prev, process_t *next);

static process_t *remove_waiting_process(struct dlist *list, int wait_id);
static process_t *remove_process_by_pid(struct dlist *list, int pid);

static void init_idle_process();
static void init_shell_process();

void process_initialize() {
    init_idle_process();
    init_shell_process();
}

scheduler_t *scheduler_get() {
    return &scheduler;
}

void process_yield() {
    if (dlist_empty(&scheduler.ready)) {
        return;
    }

    process_t *proc = scheduler.current;
    proc->state = PROC_SLOT_READY;

    if (proc->pid != IDLE_PROC_PID) {
        dlist_insert_tail(&scheduler.ready, &proc->link);
    }

    schedule();
}

void process_sleep(const int wait_id) {
    process_t *proc = scheduler.current;
    proc->state = PROC_SLOT_SLEEPING;
    proc->wait_id = wait_id;

    dlist_insert_tail(&scheduler.waiting, &proc->link);

    schedule();
}

void process_wakeup(const int wait_id) {
    process_t *proc = remove_waiting_process(&scheduler.waiting, wait_id);

    while (proc != nullptr) {
        proc->state = PROC_SLOT_READY;
        dlist_insert_tail(&scheduler.ready, &proc->link);

        proc = remove_waiting_process(&scheduler.waiting, wait_id);
    }
}

int process_sleep_interruptible(const int wait_id) {
    process_t *proc = scheduler.current;
    if (proc->sig_pending & ~proc->sig_blocked) {
        return -EINTR;
    }
    proc->interruptible = true;
    process_sleep(wait_id);
    proc->interruptible = false;
    return proc->sig_pending & ~proc->sig_blocked ? -EINTR : 0;
}

void process_interrupt(process_t *proc) {
    if (proc->state != PROC_SLOT_SLEEPING || !proc->interruptible) {
        return;
    }
    dlist_remove(&scheduler.waiting, &proc->link);
    proc->state = PROC_SLOT_READY;
    dlist_insert_tail(&scheduler.ready, &proc->link);
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

void process_exit() {
    process_t *proc = scheduler.current;
    proc->state = PROC_SLOT_KILLED;

    dlist_insert_tail(&scheduler.killed, &proc->link);

    process_wakeup(INIT_PROC_ID);
    schedule();
}

void process_wait(const int pid) {
    while (1) {
        if (!dlist_empty(&scheduler.killed)) {
            process_t *proc = remove_process_by_pid(&scheduler.killed, pid);
            if (proc != nullptr) {
                free_physical_page((void*)proc->stack);
                free_vm(proc->page_map);

                for (int i = 0; i<PROC_MAX_FILE_DESCRIPTORS; i++) {
                    if (proc->file[i] != nullptr) {
                        vfs_close((int)proc->file[i]->id);
                        proc->file[i] = nullptr;
                    }
                }
                memset(proc, 0, sizeof(process_t));
                break;
            }
        }
        else {
            process_sleep(INIT_PROC_ID);
        }
    }
}

int process_fork() {
    process_t *current = scheduler.current;

    process_t *proc = create_process();
    if (proc == nullptr) return -1;

    if (!copy_uvm(proc->page_map, current->page_map)) {
        free_physical_page((void*)proc->stack);
        free_vm(proc->page_map);
        memset(proc, 0, sizeof(process_t));
        return -1;
    }

    memcpy(proc->file, current->file, sizeof(struct file_s*)*PROC_MAX_FILE_DESCRIPTORS);

    for (int i = 0; i<PROC_MAX_FILE_DESCRIPTORS; i++) {
        if (proc->file[i] != nullptr) {
            proc->file[i]->ref_count++;
        }
    }
    memcpy(proc->frame, current->frame, sizeof(interrupt_context));

    // the child inherits handlers and mask, not pending signals
    memcpy(proc->sig_actions, current->sig_actions, sizeof(proc->sig_actions));
    proc->sig_blocked = current->sig_blocked;
    proc->altstack_sp = current->altstack_sp;
    proc->altstack_size = current->altstack_size;
    proc->altstack_flags = current->altstack_flags;

    proc->frame->rax = 0;
    proc->state = PROC_SLOT_READY;
    dlist_insert_tail(&scheduler.ready, &proc->link);

    return proc->pid;
}

int process_exec(process_t *proc, const char *filename) {
    vfs_stat_t stat = {0};
    size_t program_size = 0;

    // the handlers are gone with the old image; ignored signals stay ignored (POSIX exec)
    for (int i = 0; i<SIGNAL_MAX; i++) {
        if (proc->sig_actions[i].handler != SIG_IGN) {
            memset(&proc->sig_actions[i], 0, sizeof(proc->sig_actions[i]));
        }
    }
    proc->altstack_sp = proc->altstack_size = 0;
    proc->altstack_flags = 0;

    const int fd = vfs_open(filename, O_RDONLY);
    if (fd<0) {
        process_exit();
    }

    if (vfs_stat(fd, &stat)<0 || stat.type != INODE_FILE || stat.size > USER_MEMORY_SIZE - STACK_SIZE) {
        vfs_close(fd);
        process_exit();
    }

    memset((void*)USER_VIRTUAL_BASE, 0, USER_MEMORY_SIZE);
    while (program_size < stat.size) {
        const ssize_t read_size = vfs_read(fd, (void*)(USER_VIRTUAL_BASE + program_size), stat.size - program_size);
        if (read_size <= 0) {
            break;
        }
        program_size += (size_t)read_size;
    }
    vfs_close(fd);

    if (program_size != stat.size) {
        process_exit();
    }

    memset(proc->frame, 0, sizeof(interrupt_context));
    proc->frame->cs = 0x20 | 3;
    proc->frame->rip = USER_VIRTUAL_BASE;
    proc->frame->ss = 0x18 | 3;
    proc->frame->rsp = USER_STACK_START;
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
    task_state_segment.rsp0 = proc->stack + STACK_SIZE;
}

static void schedule() {
    process_t *prev = scheduler.current;
    process_t *next;

    if (dlist_empty(&scheduler.ready)) {
        next = &processes[IDLE_PROC_PID];
    }
    else {
        struct dlist_node *node = dlist_first(&scheduler.ready);
        dlist_remove(&scheduler.ready, node);
        next = dlist_entry(node, process_t, link);
    }

    next->state = PROC_SLOT_RUNNING;
    scheduler.current = next;

    switch_process(prev, next);
}

static void switch_process(process_t *prev, process_t *next) {
    set_tss(next);
    switch_vm(next->page_map);
    context_switch(&prev->context, next->context);
}

static process_t *remove_waiting_process(struct dlist *list, const int wait_id) {
    for (struct dlist_node *node = dlist_first(list); node; node = dlist_next(node)) {
        process_t *proc = dlist_entry(node, process_t, link);
        if (proc->wait_id == wait_id) {
            dlist_remove(list, node);
            return proc;
        }
    }
    return nullptr;
}

static process_t *remove_process_by_pid(struct dlist *list, const int pid) {
    for (struct dlist_node *node = dlist_first(list); node; node = dlist_next(node)) {
        process_t *proc = dlist_entry(node, process_t, link);
        if (proc->pid == pid) {
            dlist_remove(list, node);
            return proc;
        }
    }
    return nullptr;
}

static void init_idle_process() {
    process_t *proc = find_free_process_slot();
    proc->pid = IDLE_PROC_PID;
    proc->page_map = read_cr3();
    proc->state = PROC_SLOT_RUNNING;
    scheduler.current = proc;
}

static void init_shell_process() {
    vfs_stat_t stat = {0};
    size_t program_size = 0;
    uint8_t *program = nullptr;

    process_t *proc = create_process();
    if (proc == nullptr) {
        terminal_printf("init_shell_process: cannot create process\n");
        return;
    }


    const int fd = vfs_open(INIT_PROC_PATH, O_RDONLY);
    if (fd>=0 && vfs_stat(fd, &stat)==0 && stat.type == INODE_FILE && stat.size <= USER_MEMORY_SIZE - STACK_SIZE) {
        program = malloc(stat.size);
    }
    while (program != nullptr && program_size < stat.size) {
        const ssize_t read_size = vfs_read(fd, program + program_size, stat.size - program_size);
        if (read_size <= 0) {
            break;
        }
        program_size += (size_t)read_size;
    }
    if (fd>=0) {
        vfs_close(fd);
    }

    if (program == nullptr || program_size != stat.size || !setup_uvm(proc->page_map, (uint64_t)program, program_size)) {
        terminal_printf("init_shell_process: cannot load %s\n", INIT_PROC_PATH);
        free(program);
        free_physical_page((void*)proc->stack);
        free_vm(proc->page_map);
        memset(proc, 0, sizeof(process_t));
        return;
    }
    free(program);

    proc->state = PROC_SLOT_READY;
    dlist_insert_tail(&scheduler.ready, &proc->link);
}

process_t *process_spawn(const void *program, const size_t size) {
    if (size > USER_MEMORY_SIZE - STACK_SIZE) {
        return nullptr;
    }
    process_t *proc = create_process();
    if (proc == nullptr) {
        return nullptr;
    }
    if (!setup_uvm(proc->page_map, (uint64_t)program, size)) {
        free_physical_page((void*)proc->stack);
        free_vm(proc->page_map);
        memset(proc, 0, sizeof(process_t));
        return nullptr;
    }
    proc->state = PROC_SLOT_READY;
    dlist_insert_tail(&scheduler.ready, &proc->link);
    return proc;
}

static process_t *create_process() {
    process_t *proc = find_free_process_slot();

    if (proc == nullptr) {
        return nullptr;
    }
    proc->stack = (uint64_t)alloc_physical_page();
    if (proc->stack == 0) {
        return nullptr;
    }

    proc->state = PROC_SLOT_INIT;
    proc->pid = next_pid++;
    proc->wait_id = 0;

    memset((void*)proc->stack, 0, STACK_SIZE);
    const uint64_t stack_top = proc->stack + STACK_SIZE;

    proc->context = stack_top - sizeof(interrupt_context) - 7*8;
    *(uint64_t*)(proc->context + 6*8) = (uint64_t)trap_return;

    proc->frame = (interrupt_context*)(stack_top - sizeof(interrupt_context));

    proc->frame->cs = 0x20 | 3;
    proc->frame->rip = USER_VIRTUAL_BASE;
    proc->frame->ss = 0x18 | 3;
    proc->frame->rsp = USER_STACK_START;
    proc->frame->rflags = 0x202;

    proc->page_map = setup_kvm();
    if (proc->page_map == 0) {
        free_vm(proc->page_map);
        memset(proc, 0, sizeof(process_t));
        return nullptr;
    }
    return proc;
}
