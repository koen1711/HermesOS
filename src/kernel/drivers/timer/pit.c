#include "pit.h"

#include <drivers/timer/callout.h>
#include <hardware/interrupts/irq.h>
#include <hardware/port/ports.h>
#include <os/errno.h>

static uint32_t frequency;

int pit_initialize(const uint32_t hz)
{
    if (hz < 19 || hz > PIT_BASE_HZ)
        return -EINVAL;

    const uint16_t divisor = PIT_BASE_HZ / hz;
    frequency = PIT_BASE_HZ / divisor;

    port_write_u8(PIT_COMMAND_PORT, 0x36);
    port_write_u8(PIT_CHANNEL0_PORT, divisor & 0xFF);
    port_write_u8(PIT_CHANNEL0_PORT, divisor >> 8);

    return irq_register_handler(PIT_IRQ, handle_timer_interrupt, nullptr);
}

uint32_t pit_frequency()
{
    return frequency;
}

void handle_timer_interrupt(const uint8_t irq, const void* ctx)
{
    (void)irq;
    (void)ctx;
    callout_tick();
}
