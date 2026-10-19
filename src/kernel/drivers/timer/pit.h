#ifndef PIT_H
#define PIT_H

#include <os/stdint.h>

#define PIT_IRQ           0
#define PIT_BASE_HZ       1193182   // input clock of the 8253/8254
#define PIT_DEFAULT_HZ    1000      // 1 ms ticks

#define PIT_CHANNEL0_PORT 0x40
#define PIT_COMMAND_PORT  0x43

// Program channel 0 to fire IRQ 0 at `hz` and register the timer handler
int pit_initialize(uint32_t hz);
uint32_t pit_frequency();
void handle_timer_interrupt(uint8_t irq, const void* ctx);

#endif //PIT_H
