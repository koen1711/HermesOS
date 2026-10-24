#include <drivers/timer/tsc.h>

#include <drivers/timer/pit.h>
#include <hardware/port/ports.h>

#define TSC_PIT_GATE_PORT  0x61
#define TSC_PIT_CH2_PORT   0x42
#define TSC_CALIBRATE_MS   10

static uint64_t tsc_per_us;

uint64_t tsc_read()
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

void tsc_initialize()
{
    uint8_t gate = port_read_u8(TSC_PIT_GATE_PORT);
    port_write_u8(TSC_PIT_GATE_PORT, (gate & ~0x02) | 0x01);
    port_write_u8(PIT_COMMAND_PORT, 0xB0);
    constexpr uint16_t count = PIT_BASE_HZ * TSC_CALIBRATE_MS / 1000;
    port_write_u8(TSC_PIT_CH2_PORT, count & 0xFF);
    port_write_u8(TSC_PIT_CH2_PORT, count >> 8);
    gate = port_read_u8(TSC_PIT_GATE_PORT);
    port_write_u8(TSC_PIT_GATE_PORT, gate & ~0x01);
    port_write_u8(TSC_PIT_GATE_PORT, gate | 0x01);
    const uint64_t t0 = tsc_read();
    while (!(port_read_u8(TSC_PIT_GATE_PORT) & 0x20)) {}
    const uint64_t t1 = tsc_read();
    const uint64_t per_us = (t1 - t0) / (TSC_CALIBRATE_MS * 1000);
    tsc_per_us = per_us ? per_us : 1000;
}

uint64_t tsc_micros()
{
    if (!tsc_per_us)
        tsc_initialize();
    return tsc_read() / tsc_per_us;
}
