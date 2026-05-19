#include <hardware/interrupts/interrupts.h>
#include <drivers/drivers.h>
#include <drivers/terminal/terminal.h>
#include <hardware/port/pci.h>

#include <hardware/disks/ata.h>
#include <drivers/fs/fs.h>

#include <multiboot.h>



void kernel_main(void* multiboot_info_ptr)
{
    const multiboot_info_t pmm_info = parse_multiboot_info(multiboot_info_ptr);
    memory_initialize(pmm_info);

    terminal_initialize(VGA_TEXT_MODE);

    pci_initialize();
    fs_init();
    ata_initialize();

    idt_initialize();

    register_drivers();

    while (1)
    {
        asm volatile ("cli; hlt");
    }
}
