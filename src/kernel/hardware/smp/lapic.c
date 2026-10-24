#include "lapic.h"

#include <hardware/memory/mmu.h>
#include <drivers/timer/tsc.h>

enum lapic_register {
    LAPIC_ID = 0x020,
    LAPIC_EOI = 0x0B0,
    LAPIC_SVR = 0x0F0,
    LAPIC_ICR_LOW = 0x300,
    LAPIC_ICR_HIGH = 0x310,
    LAPIC_LVT_TIMER = 0x320,
    LAPIC_LVT_LINT0 = 0x350,
    LAPIC_TIMER_INITIAL = 0x380,
    LAPIC_TIMER_CURRENT = 0x390,
    LAPIC_TIMER_DIVIDE = 0x3E0
};

#define LAPIC_SVR_ENABLE 0x100
#define LAPIC_MASKED 0x10000
#define LAPIC_TIMER_PERIODIC 0x20000
#define ICR_PENDING 0x1000
#define ICR_INIT 0x4500
#define ICR_STARTUP 0x4600
#define ICR_ALL_BUT_SELF (3 << 18)

static volatile uint32_t* lapic;
static uint32_t timer_count;

static uint32_t lapic_read(const uint32_t reg)
{
    return lapic[reg / 4];
}

static void lapic_write(const uint32_t reg, const uint32_t value)
{
    lapic[reg / 4] = value;
}

static void icr_wait()
{
    while (lapic_read(LAPIC_ICR_LOW) & ICR_PENDING)
        asm volatile("pause");
}

bool lapic_initialize(const uint64_t phys)
{
    const uint64_t virt = vmm_alloc_mmio_region(0x1000);
    if (!virt || map_page(virt, phys, MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE) < 0)
        return false;
    lapic = (volatile uint32_t*)virt;
    return true;
}

bool lapic_present()
{
    return lapic != nullptr;
}

void lapic_enable(const bool mask_lint0)
{
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
    if (mask_lint0)
        lapic_write(LAPIC_LVT_LINT0, LAPIC_MASKED);
}

uint32_t lapic_id()
{
    return lapic ? lapic_read(LAPIC_ID) >> 24 : 0;
}

void lapic_eoi()
{
    lapic_write(LAPIC_EOI, 0);
}

void lapic_send_init(const uint32_t apic_id)
{
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, ICR_INIT);
    icr_wait();
}

void lapic_send_startup(const uint32_t apic_id, const uint32_t vector)
{
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, ICR_STARTUP | (vector & 0xFF));
    icr_wait();
}

void lapic_send_others(const uint32_t vector)
{
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, ICR_ALL_BUT_SELF | (vector & 0xFF));
    icr_wait();
}

void lapic_timer_calibrate(const uint32_t hz)
{
    lapic_write(LAPIC_TIMER_DIVIDE, 0x3);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_MASKED);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFF);
    const uint64_t start = tsc_micros();
    while (tsc_micros() - start < 10000)
        asm volatile("pause");
    const uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INITIAL, 0);
    timer_count = (uint32_t)((uint64_t)elapsed * 100 / hz);
    if (timer_count == 0)
        timer_count = 1;
}

void lapic_timer_start()
{
    lapic_write(LAPIC_TIMER_DIVIDE, 0x3);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INITIAL, timer_count);
}
