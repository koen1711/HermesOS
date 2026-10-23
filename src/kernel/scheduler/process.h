#ifndef PROCESS_H
#define PROCESS_H

#include <os/stdint.h>
#include <hardware/memory/mmu.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/gdt/tss.h>
#include <utils/queue/queue.h>

#define STACK_SIZE      PAGE_SIZE
#define MAXIMUM_NUMBER_OF_PROCESSES 10
#define USER_STACK_START    (USER_VIRTUAL_BASE + USER_MEMORY_SIZE)
#define NORMAL_PROC_ID -1
#define INIT_PROC_ID 1
#define IDLE_PROC_PID 0
#define WAIT_KEY_INT -2
#define PROC_MAX_FILE_DESCRIPTORS 100

typedef enum {
    PROC_SLOT_UNUSED = 0,
    PROC_SLOT_INIT,
    PROC_SLOT_READY,
    PROC_SLOT_RUNNING,
    PROC_SLOT_SLEEPING,
    PROC_SLOT_KILLED
} process_state_t;

typedef struct {
    struct dlist_node link;     // in one of the scheduler lists
    int pid;
    int wait_id;
    process_state_t state;
    uint64_t page_map;
    uint64_t context;           // kernel stack pointer saved by context_switch
    uint64_t stack;
    interrupt_context *frame;   // user registers, restored by trap_return
    struct file_s *file[PROC_MAX_FILE_DESCRIPTORS];
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
void process_exit();
void process_wait(int pid);
int process_fork();
int process_exec(process_t *proc, const char *filename);

#endif //PROCESS_H
