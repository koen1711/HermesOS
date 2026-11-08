#ifndef HARDWARE_INTERRUPTS_IOAPIC_H
#define HARDWARE_INTERRUPTS_IOAPIC_H

#include <os/stdint.h>

bool ioapic_initialize();
void ioapic_route(uint8_t irq, uint8_t vector, uint32_t dest_apic_id);
void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);

#endif //HARDWARE_INTERRUPTS_IOAPIC_H
