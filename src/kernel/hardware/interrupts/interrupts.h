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

// Built on the stack by every interrupt stub in interrupt.asm
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    uint64_t interrupt_number;
    uint64_t error_code;

    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_context;

_Static_assert(sizeof(interrupt_context) == 22 * 8, "interrupt_context layout must match interrupt.asm");

extern idt_entry idt[IDT_SIZE];


void idt_initialize();
void idt_set_entry(uint8_t index, uintptr_t handler);

void trap_handler(interrupt_context* context);

extern void trap_return();

#endif //INTERRUPTS_H
