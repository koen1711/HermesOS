#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#define IDT_ENTRY_SIZE 	16
#define IDT_SIZE 		256
#define IDT_ALIGNMENT 	16

#define IRQ_OFFSET 0x20
#define SYSCALL_VECTOR (IRQ_OFFSET + 0x80)

#include <os/stdint.h>

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry;


typedef struct TrapFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;


    uint64_t trapno;
    uint64_t errorcode;


    uint64_t rip, cs, rflags, rsp, ss;
} TrapFrame;

_Static_assert(sizeof(TrapFrame) == 22 * 8, "TrapFrame layout must match interrupt.asm");

typedef TrapFrame interrupt_context;

extern idt_entry idt[IDT_SIZE];


void idt_initialize();


void trap_handler(TrapFrame* tf);

extern void trap_return(void);

#endif //INTERRUPTS_H
