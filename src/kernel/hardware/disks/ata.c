#include "ata.h"

#include <os/stdint.h>

#include <drivers/fs/fs.h>
#include <drivers/fs/fat/fat32.h>
#include <hardware/port/pci.h>
#include <hardware/port/ports.h>
#include <hardware/memory/alloc.h>
#include <hardware/terminal/stdio.h>
#include <utils/list/list.h>
#include <utils/str/str.h>

#include "disk.h"
#include "hardware/interrupts/panic.h"

/* ── block_device adapters for ATA ─────────────────────────────── */

static int ata_bdev_read(struct block_device *bdev, uint64_t off,
                         uint32_t len, void *buf)
{
    uint64_t abs_off = bdev->part_offset + off;
    return ata_read(bdev->unique_id, abs_off, abs_off + len, buf);
}

static int ata_bdev_write(struct block_device *bdev, uint64_t off,
                          uint32_t len, const void *buf)
{
    uint64_t abs_off = bdev->part_offset + off;
    return ata_write(bdev->unique_id, abs_off, abs_off + len, (const uint8_t *)buf);
}

static const struct block_device_ops ata_bdev_ops = {
    .read_bytes  = ata_bdev_read,
    .write_bytes = ata_bdev_write,
};

// Create a linked list for the registry
full_map* ata_disk_registry = NULL;
uintptr_t pci_ata_controller = 0;

void ata_io_wait(const ata_device * dev) {
    port_read_u8(dev->io_base + ATA_REG_ALTSTATUS);
    port_read_u8(dev->io_base + ATA_REG_ALTSTATUS);
    port_read_u8(dev->io_base + ATA_REG_ALTSTATUS);
    port_read_u8(dev->io_base + ATA_REG_ALTSTATUS);
}

void ata_soft_reset(const ata_device * dev) {
    port_write_u8(dev->control, ATA_REG_LBA1);
    ata_io_wait(dev);
    port_write_u8(dev->control, ATA_REG_DATA);
}

int ata_status_wait(const ata_device * dev, const int timeout) {
    int status;
    if (timeout > 0) {
        int i = 0;
        while ((status = port_read_u8(dev->io_base + ATA_REG_STATUS)) & ATA_SR_BSY && (i < timeout)) i++;
    } else {
        while ((status = port_read_u8(dev->io_base + ATA_REG_STATUS)) & ATA_SR_BSY);
    }
    return status;
}

int ata_read_sector(const ata_device * dev, const uint32_t lba, uint16_t * buffer)
{
    if (buffer == NULL) return -1;

    port_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xE0 | (dev->slave << 4) | ((lba >> 24) & 0x0F));
    port_io_wait();

    port_write_u8(dev->io_base + ATA_REG_SECCOUNT0, 1);
    port_write_u8(dev->io_base + ATA_REG_LBA0, lba & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_LBA1, (lba >> 8) & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_LBA2, (lba >> 16) & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    while (1) {
        const uint8_t status = port_read_u8(dev->io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return -2;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) break;
    }

    for (int i = 0; i < 256; i++) {
        const uint16_t data = port_read_u16(dev->io_base + ATA_REG_DATA);
        buffer[i] = data;
    }

    const uint8_t status = port_read_u8(dev->io_base + ATA_REG_STATUS);
    if (status & ATA_SR_ERR) return -4;

    return 0;
}

int ata_write_sector(const ata_device * dev, const uint32_t lba, const uint16_t * buffer)
{
    if (buffer == NULL) return -1;

    port_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xE0 | (dev->slave << 4) | ((lba >> 24) & 0x0F));
    port_io_wait();

    port_write_u8(dev->io_base + ATA_REG_SECCOUNT0, 1);
    port_write_u8(dev->io_base + ATA_REG_LBA0, lba & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_LBA1, (lba >> 8) & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_LBA2, (lba >> 16) & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    // Write data
    for (int i = 0; i < 256; i++) {
        port_write_u16(dev->io_base + ATA_REG_DATA, buffer[i]);
    }

    // Verify successful completion
    const uint8_t status = port_read_u8(dev->io_base + ATA_REG_STATUS);
    if (status & ATA_SR_ERR) return -4;

    return 0;
}

int ata_read_until(const ata_device * dev, const uint32_t lba, const uint32_t n_bytes, uint8_t *buffer)
{
    uint16_t* full_sector = malloc(ATA_SECTOR_SIZE);
    if (full_sector == NULL) return -1;

    const uint8_t sectors_to_read = (n_bytes % ATA_SECTOR_SIZE != 0 ? 1 : 0);

    for (uint32_t i = 0; i < sectors_to_read; i++) {
        const uint32_t current_lba = lba + i;
        const int result = ata_read_sector(dev, current_lba, full_sector);
        if (result != 0) {
            free(full_sector);
            return result;
        }

        const size_t copy_offset = i * ATA_SECTOR_SIZE;
        size_t copy_size = ATA_SECTOR_SIZE;

        if (i == sectors_to_read - 1) {
            const size_t last_byte_offset = (n_bytes - 1) % ATA_SECTOR_SIZE;

            if (i == 0) copy_size = last_byte_offset - lba % ATA_SECTOR_SIZE + 1;
            else copy_size = last_byte_offset + 1;
        }

        memcpy(buffer + (i * ATA_SECTOR_SIZE), full_sector + copy_offset, copy_size);
    }

    return 0;
}

int ata_read(const uint32_t unique_id, const uint64_t begin_byte, const uint64_t end_byte, uint8_t* buffer)
{
    const ata_device* dev = map_get(ata_disk_registry, (void*)unique_id);
    if (dev == NULL) return -2;

    if (begin_byte >= end_byte) return 0; // Nothing to read

    const uint64_t start_lba = begin_byte / ATA_SECTOR_SIZE;
    const uint64_t end_lba = (end_byte - 1) / ATA_SECTOR_SIZE;
    const uint64_t n_sectors = end_lba - start_lba + 1;

    size_t buffer_pos = 0;

    for (uint64_t i = 0; i < n_sectors; i++) {
        uint8_t* temp_buffer = malloc(ATA_SECTOR_SIZE);
        if (temp_buffer == NULL) return -1;

        const int result = ata_read_sector(dev, start_lba + i, (uint16_t*)temp_buffer);
        if (result != 0) {
            free(temp_buffer);
            return result;
        }

        size_t copy_offset = 0;
        size_t copy_size = ATA_SECTOR_SIZE;

        if (i == 0) {
            copy_offset = begin_byte % ATA_SECTOR_SIZE;
            copy_size = ATA_SECTOR_SIZE - copy_offset;
        }

        if (i == n_sectors - 1) {
            const size_t last_byte_offset = (end_byte - 1) % ATA_SECTOR_SIZE;

            if (i == 0) {
                // If this is both the first and last sector
                copy_size = last_byte_offset - begin_byte % ATA_SECTOR_SIZE + 1;
            } else {
                // Just the last sector
                copy_size = last_byte_offset + 1;
            }
        }

        if (buffer_pos + copy_size > (end_byte - begin_byte)) {
            copy_size = (end_byte - begin_byte) - buffer_pos;
        }

        memcpy(buffer + buffer_pos, temp_buffer + copy_offset, copy_size);
        buffer_pos += copy_size;

        free(temp_buffer);
    }

    return 0;
}

int ata_write(const uint32_t unique_id, const uint64_t begin_byte, const uint64_t end_byte, const uint8_t *buffer)
{
    const ata_device* dev = map_get(ata_disk_registry, (void*)unique_id);

    const uint64_t n_sectors = (end_byte - begin_byte + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    const uint64_t lba = begin_byte / ATA_SECTOR_SIZE;

    for (uint64_t i = 0; i < n_sectors; i++) {
        uint8_t* temp_buffer = malloc(ATA_SECTOR_SIZE);
        if (temp_buffer == NULL) return -1;

        size_t copy_offset = 0;
        size_t copy_size = ATA_SECTOR_SIZE;

        if (i == 0) {
            copy_offset = begin_byte % ATA_SECTOR_SIZE;
            copy_size = ATA_SECTOR_SIZE - copy_offset;
        }
        if (i == n_sectors - 1) {
            copy_size = end_byte % ATA_SECTOR_SIZE;
            if (copy_size == 0) copy_size = ATA_SECTOR_SIZE; // Handle full-sector case
        }

        memcpy(temp_buffer + copy_offset,
               buffer + (i * ATA_SECTOR_SIZE),
               copy_size);

        ata_write_sector(dev, lba + i, (uint16_t*)temp_buffer);

        free(temp_buffer);
    }

    return 0;
}

boot_type detect_boot_type(const ata_device* dev) {
    uint8_t* sector = malloc(512);  // Allocate buffer for a full ISO sector
    if (!sector) return BOOT_UNKNOWN;

    // Read sector 17 for El Torito check
    if (ata_read_sector(dev, 17, (uint16_t*)sector) == 0) {
        if (is_el_torito(sector)) {
            free(sector);
            return BOOT_EL_TORITO;
        }
    }

    // Read sector 1 for GPT check (512-byte sector offset)
    if (ata_read_sector(dev, 1, (uint16_t*)sector) == 0) {
        if (is_gpt(sector)) {
            free(sector);
            return BOOT_GPT;
        }
    }

    free(sector);
    return BOOT_UNKNOWN;
}

void ata_get_gpt(const ata_device * dev, full_gpt* gpt)
{
    if (gpt == NULL) return;

    gpt->boot_record = (master_boot_record*)malloc(sizeof(master_boot_record) + 8);
    gpt->header = (gpt_header*)malloc(sizeof(gpt_header) + 8);

    ata_read_until(dev, 0, sizeof(master_boot_record), (uint8_t*)gpt->boot_record);
    ata_read_until(dev, 1, sizeof(gpt_header), (uint8_t*)gpt->header);

    uint16_t* sector_buffer = malloc(ATA_SECTOR_SIZE);
    if (sector_buffer == NULL) panic("Failed to allocate memory for GPT sector buffer", __FILE__, __LINE__);
    const uint64_t entries_lba = gpt->header->lba_guid_partition_start;
    const uint32_t entry_size = gpt->header->partition_entry_size;
    const uint32_t entries_per_sector = ATA_SECTOR_SIZE / entry_size;

    for (uint32_t i = 0; i < 128; i++) {
        const uint32_t sector_offset = i / entries_per_sector;
        const uint32_t entry_offset = i % entries_per_sector;

        // Only read a new sector when needed
        if (entry_offset == 0)
            ata_read_sector(dev, entries_lba + sector_offset, sector_buffer);

        // Copy the entry from the buffer
        gpt_entry* tmp_entry = malloc(sizeof(gpt_entry));
        if (tmp_entry == NULL) panic("Failed to allocate memory for GPT entry", __FILE__, __LINE__);
        memcpy(tmp_entry,
               (uint8_t*)sector_buffer + entry_offset * entry_size,
               entry_size);
        tmp_entry->part_type = FS_UNKNOWN;

        if (!is_valid_gpt_entry(tmp_entry))
            continue;
        tmp_entry->part_type = gpt_partition_fs_type(tmp_entry);
        gpt->entries[i] = tmp_entry;

        if (gpt->entries[i]->part_type == FS_UNKNOWN) {
            // Print the partition GUID
            printf("Unknown partition type: ");
            for (int j = 0; j < 16; j++) {
                printf("%02X", gpt->entries[i]->partition_guid[j]);
            }
            printf("\n");
        }
    }

    free(sector_buffer);
}

void ata_device_initialize(ata_device * dev)
{
    dev->is_atapi = false;
    dev->dma_prdt = (prdt *)malloc(sizeof(prdt));
    dev->dma_prdt_phys = (uintptr_t)dev->dma_prdt;
    dev->dma_start = (uint8_t *)malloc(ATA_CACHE_SIZE);
    dev->dma_start_phys = (uintptr_t)dev->dma_start;
    dev->dma_prdt[0].offset = dev->dma_start_phys;
    dev->dma_prdt[0].bytes = ATA_CACHE_SIZE;
    dev->dma_prdt[0].last = 0x8000;

    port_write_u8(dev->io_base + 1, 1);
    port_write_u8(dev->control, 0);

    ata_select_device(dev);
    ata_io_wait(dev);

    const uint16_t command_reg = port_read_u32(pci_ata_controller + PCI_COMMAND);
    if (!(command_reg & 1 << 2))
        port_write_u32(pci_ata_controller + PCI_COMMAND, command_reg | 1 << 2);

    dev->bar4 = port_read_u32(pci_ata_controller + PCI_BAR4);

    if (dev->bar4 & 0x00000001)
        dev->bar4 = dev->bar4 & 0xFFFFFFFC;

    full_gpt* gpt = malloc(sizeof(full_gpt));

    const boot_type boot_type = detect_boot_type(dev);

    if (boot_type == BOOT_GPT)
    {
        ata_get_gpt(dev, gpt);

        for (int i = 0; i < 128; i++) {
            if (gpt->entries[i] != NULL) {
                disk_node *node = malloc(sizeof(disk_node));
                node->unique_id = (uint32_t)(uintptr_t)dev;
                node->length = gpt->entries[i]->lba_end - gpt->entries[i]->lba_start;
                node->offset = gpt->entries[i]->lba_start;
                node->part_type = gpt_partition_fs_type(gpt->entries[i]);
                node->disk_type = DISK_ATA;
                node->name = str_utf16_to_utf8(gpt->entries[i]->partition_name);

                map_put(ata_disk_registry, (void*)(uintptr_t)node->unique_id, dev);

                /* Create a block_device for this partition */
                uint64_t part_off = node->offset * ATA_SECTOR_SIZE;
                uint64_t part_len = node->length * ATA_SECTOR_SIZE;
                struct block_device *bdev = fat32_create_bdev(
                    node->unique_id, part_off, part_len, &ata_bdev_ops, dev);

                const char *fs_name = NULL;
                switch (node->part_type) {
                case FS_FAT32:
                case FS_EFI_SYSTEM:
                    fs_name = "fat32";
                    break;
                default:
                    break;
                }

                if (fs_name && bdev) {
                    fs_mount_partition(fs_name, node->name, bdev);
                } else {
                    free(bdev);
                }
            }
        }
    } else if (boot_type == BOOT_EL_TORITO)
        printf("El Torito boot detected\n");
    else
        printf("Unknown boot type\n");

    free(gpt);
}

void atapi_device_initialize(ata_device * dev)
{
}

void ata_device_detect(ata_device * dev)
{
    port_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4); // Select the drive
    ata_io_wait(dev);

    // Send the identify command
    port_write_u8(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait(dev);

    if (port_read_u8(dev->io_base + ATA_REG_STATUS) == 0) return;

    bool could_be_atapi = false;
    while (1) {
        const uint8_t status = port_read_u8(dev->io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) { could_be_atapi = true; break; }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) break;
    }

    if (could_be_atapi) {
        const uint8_t cl = port_read_u8(dev->io_base + ATA_REG_LBA1); // CYL_LO
        const uint8_t ch = port_read_u8(dev->io_base + ATA_REG_LBA2); // CYL_HI

        if ((cl == 0x14 && ch == 0xEB) || (cl == 0x69 && ch == 0x96))
            dev->is_atapi = true;
        else return;

        port_write_u8(dev->io_base + ATA_REG_COMMAND, 0xA1);
        ata_io_wait(dev);
    }

    uint16_t * identity = (uint16_t *)&dev->identity;
    for (int i = 0; i < 256; ++i)
        identity[i] = port_read_u16(dev->io_base);

    // Validate identity data
    // For ATA devices, words 0 and 1 must not be zero
    if (!dev->is_atapi && (identity[0] == 0 || identity[1] == 0)) return; // Invalid identification data
    // For ATAPI devices, word 0 should have bit 15 set (0x8000) and bits 8-15 should be 0x85
    if (dev->is_atapi && ((identity[0] & 0x8000) == 0 || (identity[0] & 0xFF00) != 0x8500)) return; // Invalid ATAPI identification data

    dev->identity = *(ata_identify *)identity; // Copy the identification data

    if (!(dev->identity.capabilities[0] & 0x200)) return; // Check if the device supports LBA, else return (we don't support CHS)

    if (dev->is_atapi) atapi_device_initialize(dev);
    else ata_device_initialize(dev);
}

void ata_initialize() {
    ata_disk_registry = map_create(16, NULL, NULL, NULL, NULL);

    // Register the ATAPI disks
    const scan_result *result = pci_scan(-1);

    for (int i = 0; i < result->count; i++)
    {
        if (result->devices[i] == 0)
            continue;
        const pci_address addr = pci_convert_address(result->devices[i]);
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
