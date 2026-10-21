#include "pit.h"

#include <hardware/port/ports.h>

#define PIT_BASE_FREQUENCY 1193182
#define PIT_CHANNEL0_DATA  0x40
#define PIT_COMMAND        0x43
#define PIT_CHANNEL0_RATE  0x36

static volatile uint64_t s_ticks = 0;

void pit_initialize(void)
{
    const uint16_t divisor = PIT_BASE_FREQUENCY / PIT_FREQUENCY;

    port_write_u8(PIT_COMMAND, PIT_CHANNEL0_RATE);
    port_write_u8(PIT_CHANNEL0_DATA, divisor & 0xFF);
    port_write_u8(PIT_CHANNEL0_DATA, divisor >> 8);
}

uint64_t pit_ticks(void)
{
    return s_ticks;
}


void handle_timer_interrupt()
{
    s_ticks++;
}
