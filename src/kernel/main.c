#include <hardware/memory/mmu.h>
#include <hardware/interrupts/interrupts.h>
#include <hardware/gdt/tss.h>
#include <drivers/scheduler/process.h>
#include <drivers/drivers.h>

#include "drivers/terminal/terminal.h"
#include "drivers/terminal/tty.h"
#include "drivers/video/LFB.h"

#include <hardware/port/pci.h>

#include <hardware/disks/ata.h>
#include <drivers/fs/fs.h>

#include <multiboot.h>
#include <hardware/memory/pmm.h>
#include <hardware/smp/smp.h>
#include <drivers/net/net.h>
#include <hardware/interrupts/irq.h>
#include <hardware/disks/virtio_blk.h>



void kernel_main(void* multiboot_info_ptr)
{
    smp_early_initialize();
    const multiboot_info_t pmm_info = parse_multiboot_info(multiboot_info_ptr);
    memory_initialize(pmm_info);
    terminal_initialize(VGA_TEXT_MODE);
    tss_initialize();
    idt_initialize();
    pci_initialize();
    fs_init();
    ata_initialize();
    virtio_blk_initialize();
    
    register_drivers();
    net_initialize();

    process_initialize();
    tty_initialize();

    lfb_initialize();
    smp_initialize();

    irq_restore(0x200);
    while (1)
    {
        asm volatile("hlt");
    }
}
