#include "ata.h"

#include <stdint.h>

#include "hardware/io_ports/pci.h"
#include "hardware/memory/alloc.h"
#include "hardware/vga/vga.h"

uintptr_t pci_ata_controller = 0;
static char ata_drive_char = 'a';
static uint32_t cdrom_number = 0;

void ata_io_wait(const struct ata_device * dev) {
    pci_read_u8(dev->io_base + ATA_REG_ALTSTATUS);
    pci_read_u8(dev->io_base + ATA_REG_ALTSTATUS);
    pci_read_u8(dev->io_base + ATA_REG_ALTSTATUS);
    pci_read_u8(dev->io_base + ATA_REG_ALTSTATUS);
}


void ata_soft_reset(const struct ata_device * dev) {
    pci_write_u8(dev->control, 0x04);
    ata_io_wait(dev);
    pci_write_u8(dev->control, 0x00);
}

int ata_status_wait(const struct ata_device * dev, const int timeout) {
    int status;
    if (timeout > 0) {
        int i = 0;
        while ((status = pci_read_u8(dev->io_base + ATA_REG_STATUS)) & ATA_SR_BSY && (i < timeout)) i++;
    } else {
        while ((status = pci_read_u8(dev->io_base + ATA_REG_STATUS)) & ATA_SR_BSY);
    }
    return status;
}

static int ata_wait(const struct ata_device * dev, const int advanced) {
    uint8_t status = 0;

    ata_io_wait(dev);

    status = ata_status_wait(dev, -1);

    if (advanced) {
        status = pci_read_u8(dev->io_base + ATA_REG_STATUS);
        if (status   & ATA_SR_ERR)  return 1;
        if (status   & ATA_SR_DF)   return 1;
        if (!(status & ATA_SR_DRQ)) return 1;
    }

    return 0;
}

static void ata_device_init(struct ata_device * dev) {
    pci_write_u8(dev->io_base + 1, 1);
    pci_write_u8(dev->control, 0);

    if (!dev->slave)
        pci_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4);
    else
        pci_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xB0 | dev->slave << 4);
    ata_io_wait(dev);

    pci_write_u8(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait(dev);

    pci_read_u8(dev->io_base + ATA_REG_COMMAND);

    ata_wait(dev, 0);

    uint16_t * buf = (uint16_t *)&dev->identity;
    for (int i = 0; i < 256; ++i) {
        buf[i] = pci_read_u16(dev->io_base);
    }

    uint8_t * ptr = (uint8_t *)&dev->identity.model;
    for (int i = 0; i < 39; i+=2) {
        const uint8_t tmp = ptr[i+1];
        ptr[i+1] = ptr[i];
        ptr[i] = tmp;
    }

    dev->is_atapi = false;
    dev->dma_prdt = (prdt_t *)malloc(sizeof(prdt_t));
    dev->dma_prdt_phys = (uintptr_t)dev->dma_prdt;
    dev->dma_start = (uint8_t *)malloc(ATA_CACHE_SIZE);
    dev->dma_start_phys = (uintptr_t)dev->dma_start;
    dev->dma_prdt[0].offset = dev->dma_start_phys;
    dev->dma_prdt[0].bytes = ATA_CACHE_SIZE;
    dev->dma_prdt[0].last = 0x8000;

    const uint16_t command_reg = pci_read_u32(pci_ata_controller + PCI_COMMAND);
    if (!(command_reg & 1 << 2))
        pci_write_u32(pci_ata_controller + PCI_COMMAND, command_reg | 1 << 2);

    dev->bar4 = pci_read_u32(pci_ata_controller + PCI_BAR4);

    if (dev->bar4 & 0x00000001)
        dev->bar4 = dev->bar4 & 0xFFFFFFFC;
}

static uint64_t ata_max_offset(const struct ata_device * dev) {
    uint64_t sectors = dev->identity.sectors_48;

    if (!sectors)
        sectors = dev->identity.sectors_28;

    return sectors * ATA_SECTOR_SIZE;
}


void ata_device_detect(struct ata_device * dev) {
    ata_soft_reset(dev);
    ata_io_wait(dev);
    pci_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4); // Select the drive
    ata_io_wait(dev);
    ata_status_wait(dev, 10000);

    const uint8_t cl = pci_read_u8(dev->io_base + ATA_REG_LBA1); // CYL_LO
    const uint8_t ch = pci_read_u8(dev->io_base + ATA_REG_LBA2); // CYL_HI

    // Non-existent
    if (cl == 0xFF && ch == 0xFF) return;
    // Parallel ATA device, or emulated SATA
    if ((cl == 0x00 && ch == 0x00) ||
        (cl == 0x3C && ch == 0xC3)) {
        ata_device_init(dev);

        const uint64_t sectors = ata_max_offset(dev);
        if (sectors == 0) return;

        vga_text_write_string("ATA device found, inited");
        vga_text_write_string(" sectors: ");
        vga_text_write_number(sectors);

        ata_drive_char++;
        return;
    }
    // ATAPI device
    if ((cl == 0x14 && ch == 0xEB) ||
        (cl == 0x69 && ch == 0x96)) {

        vga_text_write_string("ATAPI device found, inited");

        // if (atapi_device_init(dev)) {
        //     return;
        // }
        // fs_node_t * node = atapi_device_create(dev);
        // vfs_mount(devname, node);

        cdrom_number++;
    }
    // SATA device
    if (cl == 0x3C && ch == 0xC3)
        vga_text_write_string("SATA device found, not inited");
    // SATAPI device
    if (cl == 0x69 && ch == 0x96)
        vga_text_write_string("SATAPI device found, not inited");
}

void ata_register_device(const uint32_t address)
{
    const struct pci_address addr = pci_convert_address(address);

}

void ata_initialize() {


    // Register the ATAPI disks
    const struct scan_result *result = pci_scan(-1);

    for (int i = 0; i < result->count; i++)
    {
        if (result->devices[i] == 0)
            continue;
        const struct pci_address addr = pci_convert_address(result->devices[i]);
        const uint16_t vendor_id = pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_VENDOR_ID);
        const uint16_t device_id = pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_DEVICE_ID);
        if (vendor_id == 0x8086 && (device_id == 0x7010 || device_id == 0x7111))
            pci_ata_controller = result->devices[i];
    }

    ata_device_detect(&ata_primary_master);
    ata_device_detect(&ata_primary_slave);
    ata_device_detect(&ata_secondary_master);
    ata_device_detect(&ata_secondary_slave);
}
