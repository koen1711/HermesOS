#include "hardware/interrupts/interrupts.h"
#include <drivers/drivers.h>
#include <hardware/vga/vga.h>
#include <hardware/io_ports/pci.h>

#include "hardware/disks/ata.h"

void kernel_main(void)
{
    vga_text_initialize();
    pci_initialize();
    ata_initialize();

    register_drivers();

    idt_initialize();




    while (1)
    {
    }
}
