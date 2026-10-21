#include "process.h"
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <drivers/fs/vfs/vfs.h>
#include <hardware/memory/alloc.h>

#include "drivers/terminal/terminal.h"
#include "utils/str/vsnprintf.h"


#define IDLE_PROC_PID 0
#define INIT_PROC_PATH "/shell"

static Process s_process_manager[MAXIMUM_NUMBER_OF_PROCESSES];
static int s_pid_num = 1;
static Scheduler s_scheduler;

static Process *FindFreeProcSlot(void);
static Process *CreateNewProc(void);

static void SetTSS(Process *proc);
static void Schedule(void);
static void SwitchProc(Process *prev,Process *new);

List *WaitListRemReadyProc(HeadList *list, int wait_id);
List *RemProcPID(HeadList *list, int pid);

static void InitIDLEProc(void);
static void InitShellProc(void);

void ProcInit(void) {
    InitIDLEProc();
    InitShellProc();
}

Scheduler *GetScheduler(void) {
    return &s_scheduler;
}

void Yield(void) {
    Process *proc = nullptr;
    Scheduler *scheduler = GetScheduler();
    HeadList *list = &scheduler->ready_proc_list;

    if (ListIsEmpty(list)) {
        return;
    }

    proc = scheduler->current_proc;
    proc->state = PROC_SLOT_READY;

    if (proc->pid != IDLE_PROC_PID) {
        ListPushBack(list, (List*)proc);
    }

    Schedule();
}

void Sleep(int wait_id) {
    Process *proc = nullptr;
    Scheduler *scheduler = GetScheduler();
    HeadList *list = &scheduler->wait_proc_list;

    proc = scheduler->current_proc;
    proc->state = PROC_SLOT_SLEEPING;
    proc->wait_id = wait_id;

    ListPushBack(list, (List*)proc);

    Schedule();
}

void Wakeup(int wait_id) {
    Process *proc = nullptr;
    Scheduler *scheduler = GetScheduler();
    HeadList *wait_list = &scheduler->wait_proc_list;
    HeadList *ready_list = &scheduler->ready_proc_list;

    proc = (Process*)WaitListRemReadyProc(wait_list, wait_id);

    while (proc != nullptr) {
        proc->state = PROC_SLOT_READY;
        ListPushBack(ready_list, (List*)proc);

        proc = (Process*)WaitListRemReadyProc(wait_list, wait_id);
    }
}

void Exit(void) {
    Process *proc = nullptr;
    Scheduler *scheduler = GetScheduler();
    HeadList *list = &scheduler->kill_proc_list;

    proc = scheduler->current_proc;
    proc->state = PROC_SLOT_KILLED;

    ListPushBack(list, (List*)proc);

    Wakeup(INIT_PROC_ID);
    Schedule();
}

void Wait(int pid) {
    Process *proc = nullptr;
    Scheduler *scheduler = GetScheduler();
    HeadList *list = &scheduler->kill_proc_list;

    while (1) {
        if (!ListIsEmpty(list)) {
            proc = (Process*)RemProcPID(list, pid);
            if (proc != nullptr) {
                free_physical_page((void*)proc->stack);
                free_vm(proc->page_map);

                for (int i = 0; i<PROC_MAX_FILE_DESCRIPTORS; i++) {
                    if (proc->file[i] != nullptr) {
                        vfs_close((int)proc->file[i]->id);
                        proc->file[i] = nullptr;
                    }
                }
                memset(proc, 0, sizeof(Process));
                break;
            }
        }
        else {
            Sleep(INIT_PROC_ID);
        }
    }
}

int Fork(void) {
    Process *proc = nullptr;
    Scheduler *scheduler = GetScheduler();
    HeadList *list = &scheduler->ready_proc_list;
    Process *current_proc = scheduler->current_proc;

    proc = CreateNewProc();
    if (proc == nullptr) return -1;

    if (!copy_uvm(proc->page_map, current_proc->page_map)) {
        free_physical_page((void*)proc->stack);
        free_vm(proc->page_map);
        memset(proc, 0, sizeof(Process));
        return -1;
    }

    memcpy(proc->file, current_proc->file, sizeof(struct file_s*)*PROC_MAX_FILE_DESCRIPTORS);

    for (int i = 0; i<PROC_MAX_FILE_DESCRIPTORS; i++) {
        if (proc->file[i] != nullptr) {
            proc->file[i]->ref_count++;
        }
    }
    memcpy(proc->tf, current_proc->tf, sizeof(TrapFrame));

    proc->tf->rax = 0;
    proc->state = PROC_SLOT_READY;
    ListPushBack(list, (List*)proc);

    return proc->pid;
}

int Exec(Process *proc, const char *filename) {
    int fd = 0;
    vfs_stat_t stat = {0};
    size_t program_size = 0;
    ssize_t read_size = 0;

    fd = vfs_open(filename, O_RDONLY);
    if (fd<0) {
        Exit();
    }

    if (vfs_stat(fd, &stat)<0 || stat.type != INODE_FILE || stat.size > USER_MEMORY_SIZE - STACK_SIZE) {
        vfs_close(fd);
        Exit();
    }

    memset((void*)USER_VIRTUAL_BASE, 0, USER_MEMORY_SIZE);
    while (program_size < stat.size) {
        read_size = vfs_read(fd, (void*)(USER_VIRTUAL_BASE + program_size), stat.size - program_size);
        if (read_size <= 0) {
            break;
        }
        program_size += (size_t)read_size;
    }
    vfs_close(fd);

    if (program_size != stat.size) {
        Exit();
    }

    memset(proc->tf, 0, sizeof(TrapFrame));
    proc->tf->cs = 0x20 | 3;
    proc->tf->rip = USER_VIRTUAL_BASE;
    proc->tf->ss = 0x18 | 3;
    proc->tf->rsp = USER_STACK_START;
    proc->tf->rflags = 0x202;

    return 0;

}

static Process *FindFreeProcSlot(void) {
    Process *proc = nullptr;

    for (int i = 0; i<MAXIMUM_NUMBER_OF_PROCESSES; i++) {
        if (s_process_manager[i].state == PROC_SL0T_UNUSED) {
            proc = &s_process_manager[i];
            break;
        }
    }
    return proc;
}

static void SetTSS(Process *proc) {
    TaskStateSegment.rsp0 = proc->stack + STACK_SIZE;
}

static void Schedule(void) {
    Process *prev_proc = nullptr;
    Process *curr_proc = nullptr;

    Scheduler *scheduler = GetScheduler();
    HeadList *list = &scheduler->ready_proc_list;
    prev_proc = scheduler->current_proc;

    if (ListIsEmpty(list)) {
        curr_proc = &s_process_manager[IDLE_PROC_PID];
    }
    else {
        curr_proc = (Process*)ListPopFront(list);
    }

    curr_proc->state = PROC_SLOT_RUNNING;
    scheduler->current_proc = curr_proc;

    SwitchProc(prev_proc, curr_proc);
}

static void SwitchProc(Process *prev, Process *new) {
    SetTSS(new);
    switch_vm(new->page_map);
    ContextSwitch(&prev->context, new->context);
}

List *WaitListRemReadyProc(HeadList *list, int wait_id) {
    List *curr = list->next;
    List *prev = (List*)list;
    List *item = nullptr;

    while (curr != nullptr) {
        if (((Process*)curr)->wait_id == wait_id) {
            prev->next = curr->next;
            item = curr;

            if (list->next == nullptr) {
                list->tail = nullptr;
            }
            else if (curr->next == nullptr) {
                list->tail = prev;
            }
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    return item;
}

List *RemProcPID(HeadList *list, int pid) {
    List *curr = list->next;
    List *prev = (List*)list;
    List *item = nullptr;

    while (curr != nullptr) {
        if (((Process*)curr)->pid == pid) {
            prev->next = curr->next;
            item = curr;

            if (list->next == nullptr) {
                list->tail = nullptr;
            }
            else if (curr->next == nullptr) {
                list->tail = prev;
            }
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    return item;
}

static void InitIDLEProc(void) {
    Process *proc;
    proc = FindFreeProcSlot();
    proc->pid = IDLE_PROC_PID;
    proc->page_map = read_cr3();
    proc->state = PROC_SLOT_RUNNING;
    GetScheduler()->current_proc = proc;
}

static void InitShellProc(void){
    Scheduler *scheduler = GetScheduler();
    HeadList *list = &scheduler->ready_proc_list;

    vfs_stat_t stat = {0};
    size_t program_size = 0;
    ssize_t read_size = 0;
    uint8_t *program = nullptr;
    int fd = 0;

    Process *proc = CreateNewProc();
    if (proc == nullptr) {
        terminal_printf("InitShellProc: cannot create process\n");
        return;
    }


    fd = vfs_open(INIT_PROC_PATH, O_RDONLY);
    if (fd>=0 && vfs_stat(fd, &stat)==0 && stat.type == INODE_FILE && stat.size <= USER_MEMORY_SIZE - STACK_SIZE) {
        program = malloc(stat.size);
    }
    while (program != nullptr && program_size < stat.size) {
        read_size = vfs_read(fd, program + program_size, stat.size - program_size);
        if (read_size <= 0) {
            break;
        }
        program_size += (size_t)read_size;
    }
    if (fd>=0) {
        vfs_close(fd);
    }

    if (program == nullptr || program_size != stat.size || !setup_uvm(proc->page_map, (uint64_t)program, program_size)) {
        terminal_printf("InitShellProc: cannot load %s\n", INIT_PROC_PATH);
        free(program);
        free_physical_page((void*)proc->stack);
        free_vm(proc->page_map);
        memset(proc, 0, sizeof(Process));
        return;
    }
    free(program);

    proc->state = PROC_SLOT_READY;
    ListPushBack(list, (List*)proc);
}

Process *CreateNewProc(void) {
    uint64_t stack_top = 0;
    Process *proc = FindFreeProcSlot();

    if (proc == nullptr) {
        return nullptr;
    }
    proc->stack = (uint64_t)alloc_physical_page();
    if (proc->stack == 0) {
        return nullptr;
    }

    proc->state = PROC_SLOT_INIT;
    proc->pid = s_pid_num++;
    proc->wait_id = 0;

    memset((void*)proc->stack, 0, STACK_SIZE);
    stack_top = proc->stack + STACK_SIZE;

    proc->context = stack_top - sizeof(TrapFrame) - 7*8;
    *(uint64_t*)(proc->context + 6*8) = (uint64_t)trap_return;

    proc->tf = (TrapFrame*)(stack_top - sizeof(TrapFrame));

    proc->tf->cs = 0x20 | 3;
    proc->tf->rip = USER_VIRTUAL_BASE;
    proc->tf->ss = 0x18 | 3;
    proc->tf->rsp = USER_STACK_START;
    proc->tf->rflags = 0x202;

    proc->page_map = setup_kvm();
    if (proc->page_map == 0) {
        free_vm(proc->page_map);
        memset(proc, 0, sizeof(Process));
        return nullptr;
    }
    return proc;
}