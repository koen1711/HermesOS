#ifndef SMP_ACPI_H
#define SMP_ACPI_H

#include <os/stdint.h>

#define ACPI_MAX_IOAPICS 4
#define ACPI_ISA_IRQS 16

#define ACPI_POLARITY_MASK 0x3
#define ACPI_POLARITY_LOW 0x3
#define ACPI_TRIGGER_MASK 0xC
#define ACPI_TRIGGER_LEVEL 0xC

struct acpi_ioapic {
    uint8_t id;
    uint32_t phys;
    uint32_t gsi_base;
};

struct acpi_isa_override {
    uint32_t gsi;
    uint16_t flags;
};

struct acpi_irq_routing {
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    int nioapics;
    struct acpi_isa_override isa[ACPI_ISA_IRQS];
};

int acpi_find_cpus(uint8_t* apic_ids, int max, uint64_t* lapic_phys);
int acpi_find_irq_routing(struct acpi_irq_routing* out);

#endif //SMP_ACPI_H
