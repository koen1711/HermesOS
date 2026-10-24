#ifndef HARDWARE_INTERRUPTS_IRQ_H
#define HARDWARE_INTERRUPTS_IRQ_H

#include <os/stdint.h>
#include <os/stdbool.h>

#define IRQ_MAX_LINES 16

typedef void (*irq_handler_t)(uint8_t irq, void* ctx);

int irq_register_handler(uint8_t irq, irq_handler_t handler, void* ctx);
void irq_unregister_handler(uint8_t irq, irq_handler_t handler, const void* ctx);

void irq_dispatch(uint8_t irq);

typedef void (*softintr_handler_t)();
int irq_register_softintr(softintr_handler_t handler);
bool irq_in_interrupt();

uint64_t irq_save();
void irq_restore(uint64_t flags);

#endif //HARDWARE_INTERRUPTS_IRQ_H
