#ifndef PROCESS_H
#define PROCESS_H

#pragma once
#include <os/stdint.h>
#include <hardware/memory/mmu.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/gdt/tss.h>
#include <utils/list/linked_list.h>

#define STACK_SIZE      PAGE_SIZE
#define MAXIMUM_NUMBER_OF_PROCESSES 10
#define USER_STACK_START    (USER_VIRTUAL_BASE + USER_MEMORY_SIZE)
#define NORMAL_PROC_ID -1
#define INIT_PROC_ID 1
#define IDLE_PROC_PID 0
#define WAIT_KEY_INT -2
#define PROC_MAX_FILE_DESCRIPTORS 100

typedef enum {
    PROC_SL0T_UNUSED = 0,
    PROC_SLOT_INIT,
    PROC_SLOT_READY,
    PROC_SLOT_RUNNING,
    PROC_SLOT_SLEEPING,
    PROC_SLOT_KILLED
}ProcState;




typedef struct {
    List *next;
    int pid;
    int wait_id;
    ProcState state;
    uint64_t page_map;
    uint64_t context;
    uint64_t stack;
    interrupt_context *tf;
    struct file_s *file[PROC_MAX_FILE_DESCRIPTORS];
}Process;

typedef struct {
    Process *current_proc;
    HeadList ready_proc_list;
    HeadList wait_proc_list;
    HeadList kill_proc_list;
} Scheduler;

void ProcInit(void);
void ProcStart(TrapFrame *tf);
Scheduler *GetScheduler(void);

void Yield(void);
void ContextSwitch(uint64_t *old, uint64_t new);

void Sleep(int wait_id);
void Wakeup(int wait_id);
void Exit(void);
void Wait(int pid);
int Fork(void);
int Exec(Process *proc, const char *filename);



#endif //PROCESS_h
