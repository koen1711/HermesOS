#ifndef HARDWARE_INTERRUPTS_IRQ_H
#define HARDWARE_INTERRUPTS_IRQ_H

#include <os/stdint.h>
#include <os/stdbool.h>

// IRQ handler registration, so drivers don't need a hard-coded case in pic.c
// FreeBSD equivalent: bus_setup_intr(9) / kern/kern_intr.c

#define IRQ_MAX_LINES 16

typedef void (*irq_handler_t)(uint8_t irq, void* ctx);

int irq_register_handler(uint8_t irq, irq_handler_t handler, void* ctx);
void irq_unregister_handler(uint8_t irq, irq_handler_t handler);
// Called by handle_pic_interrupt for every IRQ line
void irq_dispatch(uint8_t irq);

// Disable interrupts and return the previous RFLAGS
uint64_t irq_save();
// Restore RFLAGS (and so the interrupt flag) saved by irq_save
void irq_restore(uint64_t flags);

#endif //HARDWARE_INTERRUPTS_IRQ_H
