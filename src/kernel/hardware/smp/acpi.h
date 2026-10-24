#ifndef SMP_ACPI_H
#define SMP_ACPI_H

#include <os/stdint.h>

int acpi_find_cpus(uint8_t* apic_ids, int max, uint64_t* lapic_phys);

#endif //SMP_ACPI_H
