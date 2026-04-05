#include <hardware/memory/mmu.h>
#include <hardware/interrupts/interrupts.h>
#include <drivers/drivers.h>
#include <hardware/terminal/stdio.h>
#include <hardware/port/pci.h>

#include <hardware/disks/ata.h>

#include <multiboot.h>
#include <hardware/memory/pmm.h>
#include <hardware/terminal/terminal.h>

void kernel_main(void* multiboot_info_ptr)
{
    terminal_initialize(VGA_TEXT_MODE);
    multiboot_info_t pmm_info = parse_multiboot_info(multiboot_info_ptr);
    pmm_initialize(pmm_info);
    mmu_initialize();
    memory_initialize();
    pci_initialize();
    ata_initialize();

    register_drivers();

    idt_initialize();

    while (1)
    {
    }
}
