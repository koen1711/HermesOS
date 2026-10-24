#ifndef SMP_LAPIC_H
#define SMP_LAPIC_H

#include <os/stdint.h>
#include <os/stdbool.h>

#define LAPIC_TIMER_VECTOR 0x40
#define TLB_SHOOTDOWN_VECTOR 0x41
#define LAPIC_SPURIOUS_VECTOR 0xFF

bool lapic_initialize(uint64_t phys);
bool lapic_present();
void lapic_enable(bool mask_lint0);
uint32_t lapic_id();
void lapic_eoi();
void lapic_send_init(uint32_t apic_id);
void lapic_send_startup(uint32_t apic_id, uint32_t vector);
void lapic_send_others(uint32_t vector);
void lapic_timer_calibrate(uint32_t hz);
void lapic_timer_start();

#endif //SMP_LAPIC_H
