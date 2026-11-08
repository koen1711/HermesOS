#include "ioapic.h"

#include <hardware/memory/mmu.h>
#include <hardware/smp/acpi.h>
#include <utils/sync/spinlock.h>

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REG_REDIR 0x10

#define REDIR_ACTIVE_LOW (1u << 13)
#define REDIR_LEVEL (1u << 15)
#define REDIR_MASKED (1u << 16)

struct ioapic {
    volatile uint32_t* base;
    uint32_t gsi_base;
    uint32_t pins;
};

static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static int nioapics;
static struct acpi_isa_override isa[ACPI_ISA_IRQS];
static spinlock_t ioapic_lock = { .name = "ioapic" };

static uint32_t ioapic_read(const struct ioapic* io, const uint32_t reg)
{
    io->base[IOAPIC_REGSEL / 4] = reg;
    return io->base[IOAPIC_WINDOW / 4];
}

static void ioapic_write(const struct ioapic* io, const uint32_t reg, const uint32_t value)
{
    io->base[IOAPIC_REGSEL / 4] = reg;
    io->base[IOAPIC_WINDOW / 4] = value;
}

static struct ioapic* ioapic_for(const uint8_t irq, uint32_t* pin)
{
    const uint32_t gsi = irq < ACPI_ISA_IRQS ? isa[irq].gsi : irq;
    for (int i = 0; i < nioapics; i++) {
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].pins) {
            *pin = gsi - ioapics[i].gsi_base;
            return &ioapics[i];
        }
    }
    return nullptr;
}

bool ioapic_initialize()
{
    struct acpi_irq_routing routing;
    if (!acpi_find_irq_routing(&routing))
        return false;
    for (int i = 0; i < ACPI_ISA_IRQS; i++)
        isa[i] = routing.isa[i];

    for (int i = 0; i < routing.nioapics; i++) {
        const uint64_t virt = vmm_alloc_mmio_region(0x1000);
        if (!virt || map_page(virt, routing.ioapics[i].phys, MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE) < 0)
            continue;
        struct ioapic* io = &ioapics[nioapics];
        io->base = (volatile uint32_t*)virt;
        io->gsi_base = routing.ioapics[i].gsi_base;
        io->pins = ((ioapic_read(io, IOAPIC_REG_VER) >> 16) & 0xFF) + 1;
        for (uint32_t pin = 0; pin < io->pins; pin++) {
            ioapic_write(io, IOAPIC_REG_REDIR + pin * 2 + 1, 0);
            ioapic_write(io, IOAPIC_REG_REDIR + pin * 2, REDIR_MASKED);
        }
        nioapics++;
    }
    return nioapics != 0;
}

void ioapic_route(const uint8_t irq, const uint8_t vector, const uint32_t dest_apic_id)
{
    uint32_t pin;
    struct ioapic* io = ioapic_for(irq, &pin);
    if (!io)
        return;

    const uint16_t flags = irq < ACPI_ISA_IRQS ? isa[irq].flags : 0;
    uint32_t low = vector | REDIR_MASKED;
    if ((flags & ACPI_POLARITY_MASK) == ACPI_POLARITY_LOW)
        low |= REDIR_ACTIVE_LOW;
    if ((flags & ACPI_TRIGGER_MASK) == ACPI_TRIGGER_LEVEL)
        low |= REDIR_LEVEL;

    uint64_t irq_flags;
    spinlock_acquire_irqsave(&ioapic_lock, &irq_flags);
    ioapic_write(io, IOAPIC_REG_REDIR + pin * 2, low);
    ioapic_write(io, IOAPIC_REG_REDIR + pin * 2 + 1, dest_apic_id << 24);
    spinlock_release_irqrestore(&ioapic_lock, irq_flags);
}

static void ioapic_set_mask(const uint8_t irq, const bool masked)
{
    uint32_t pin;
    struct ioapic* io = ioapic_for(irq, &pin);
    if (!io)
        return;
    uint64_t flags;
    spinlock_acquire_irqsave(&ioapic_lock, &flags);
    const uint32_t low = ioapic_read(io, IOAPIC_REG_REDIR + pin * 2);
    ioapic_write(io, IOAPIC_REG_REDIR + pin * 2, masked ? low | REDIR_MASKED : low & ~REDIR_MASKED);
    spinlock_release_irqrestore(&ioapic_lock, flags);
}

void ioapic_mask(const uint8_t irq)
{
    ioapic_set_mask(irq, true);
}

void ioapic_unmask(const uint8_t irq)
{
    ioapic_set_mask(irq, false);
}
