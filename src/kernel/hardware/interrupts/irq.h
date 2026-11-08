#ifndef HARDWARE_INTERRUPTS_IRQ_H
#define HARDWARE_INTERRUPTS_IRQ_H

#include <os/stdint.h>

#define IRQ_ISA_LINES 16
#define IRQ_MAX_LINES 24

typedef void (*irq_handler_t)(uint8_t irq, void* ctx);

int irq_register_handler(uint8_t irq, irq_handler_t handler, void* ctx);
int irq_register_handler_mpsafe(uint8_t irq, irq_handler_t handler, void* ctx);
void irq_unregister_handler(uint8_t irq, irq_handler_t handler, const void* ctx);

void irq_dispatch(uint8_t irq);
void irq_softintr();
void irq_eoi(uint8_t irq);
bool irq_switch_to_ioapic();
bool irq_ioapic_active();
#define IRQ_MAX_MSI_HOOKS 4
typedef void (*irq_msi_hook_t)();
int irq_on_msi_ready(irq_msi_hook_t hook);
void irq_msi_ready();
int irq_msi_alloc();
void irq_msi_free(uint8_t irq);
void irq_msi_message(uint8_t irq, uint64_t* address, uint32_t* data);

typedef void (*softintr_handler_t)();
int irq_register_softintr(softintr_handler_t handler);
bool irq_in_interrupt();

uint64_t irq_save();
void irq_restore(uint64_t flags);

#endif //HARDWARE_INTERRUPTS_IRQ_H
