#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#define IDT_ENTRY_SIZE 	16
#define IDT_SIZE 		256
#define IDT_ALIGNMENT 	16

#define IRQ_OFFSET 0x20          // PIC lines 0..15 -> vectors 0x20..0x2F
#define SYSCALL_VECTOR 0x80

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

typedef struct {
    uint64_t rax, rbx, rcx, rdx, rbp, rsp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags;
    uint64_t interrupt_number;
} interrupt_context;

extern idt_entry idt[IDT_SIZE];


void idt_initialize();
void idt_set_entry(uint8_t index, uintptr_t handler);

#endif //INTERRUPTS_H
