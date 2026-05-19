#include "bochs.h"

#include "hardware/memory/mmu.h"
#include "hardware/port/ports.h"

int bochs_detect(pci_device *dev) {
    return dev->vendor_id == 0x1234 && dev->device_id == 0x1111;
}

static void* map_mmio_region(uint64_t phys, size_t size, uint64_t virt_base) {
    const size_t page_size = 0x1000;
    size_t pages = (size + page_size - 1) / page_size;

    for (size_t i = 0; i < pages; i++) {
        map_page(
            virt_base + i * page_size,
            phys + i * page_size,
            MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE
        );
    }
    return (void*)virt_base;
}

int bochs_init(pci_device *dev) {
    port_write_u16(0x01CE, 0x04); // VBE_DISPI_INDEX_ENABLE
    port_write_u16(0x01CF, 0x00); // disable first
    port_write_u16(0x01CE, 0x01); // VBE_DISPI_INDEX_XRES
    port_write_u16(0x01CF, 1024);
    port_write_u16(0x01CE, 0x02); // VBE_DISPI_INDEX_YRES
    port_write_u16(0x01CF, 768);
    port_write_u16(0x01CE, 0x03); // VBE_DISPI_INDEX_BPP
    port_write_u16(0x01CF, 32);
    port_write_u16(0x01CE, 0x04); // VBE_DISPI_INDEX_ENABLE
    port_write_u16(0x01CF, 0x41); // enable + LFB

    // Read BAR0 for framebuffer
    uint32_t bar0 = pci_config_read_u32(dev->bus, dev->device, dev->function, PCI_BAR0);
    if (bar0 & 1) return -1;

    uint64_t fb_phys = (uint64_t)(bar0 & ~0xFULL);

    const size_t fb_size = 1024 * 768 * 4;
    const uint64_t fb_virt = 0xFFFF800020000000ULL; // choose a safe VA for your kernel
    uint32_t *framebuffer = (uint32_t*)map_mmio_region(fb_phys, fb_size, fb_virt);

    for (int i = 0; i < 1024 * 768; ++i)
        framebuffer[i] = 0x00000000;

    return 0;
}