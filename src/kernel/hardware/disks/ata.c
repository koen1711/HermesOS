#include "ata.h"

#include <os/stdint.h>
#include <os/errno.h>

#include <drivers/fs/fs.h>
#include <drivers/fs/fat/fat32.h>
#include <drivers/fs/bcache/bcache.h>
#include <hardware/port/pci.h>
#include <hardware/port/ports.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/pmm.h>
#include <drivers/scheduler/process.h>
#include <drivers/timer/callout.h>
#include <hardware/interrupts/irq.h>
#include <drivers/terminal/terminal.h>
#include <utils/list/list.h>
#include <utils/str/str.h>

#include "disk.h"
#include "hardware/interrupts/panic.h"

static int ata_bdev_read(struct block_device *bdev, const uint64_t off,
                         const uint32_t len, void *buf)
{
    const uint64_t abs_off = bdev->part_offset + off;
    return ata_read(bdev->unique_id, abs_off, abs_off + len, buf) == 0 ? 0 : -EIO;
}

static int ata_bdev_write(struct block_device *bdev, const uint64_t off,
                          const uint32_t len, const void *buf)
{
    const uint64_t abs_off = bdev->part_offset + off;
    return ata_write(bdev->unique_id, abs_off, abs_off + len, (const uint8_t *)buf) == 0 ? 0 : -EIO;
}

static int ata_bdev_flush(struct block_device *bdev)
{
    return ata_flush_device(bdev->unique_id) == 0 ? 0 : -EIO;
}

static const struct block_device_ops ata_bdev_ops = {
    .read_bytes  = ata_bdev_read,
    .write_bytes = ata_bdev_write,
    .flush       = ata_bdev_flush,
};

full_map* ata_disk_registry = nullptr;
static uint32_t ata_bar4 = 0;
static struct dma_tag* ata_prdt_tag = nullptr;
static struct dma_tag* ata_data_tag = nullptr;

#define ATA_PRD_BOUNDARY   0x10000
#define ATA_DMA_TIMEOUT    10000000
#define ATA_DMA_TIMEOUT_MS 30000
#define ATA_RETRIES        3
#define ATA_CTRL_SRST      0x04
#define WAIT_CHANNEL_ATA   0x3A000000

struct ata_channel {
    int index;
    uint16_t io_base;
    uint16_t bmide;
    bool owned;
    bool irq_wait;
    bool irq_done;
    bool timed_out;
    uint8_t bm_status;
    struct callout timer;
};

static struct ata_channel ata_channels[2] = { { .index = 0, .io_base = 0x1F0 }, { .index = 1, .io_base = 0x170 } };

static struct ata_channel* ata_channel_of(const ata_device * dev)
{
    return &ata_channels[dev->io_base == 0x170];
}

static int ata_irq_wait_id(const struct ata_channel * ch)
{
    return WAIT_CHANNEL_ATA | (ch->index * 2);
}

static int ata_owner_wait_id(const struct ata_channel * ch)
{
    return WAIT_CHANNEL_ATA | (ch->index * 2 + 1);
}

static bool ata_can_sleep()
{
    const process_t * current = scheduler_get()->current;
    return current != nullptr && current->pid != IDLE_PROC_PID;
}

static void ata_channel_acquire(struct ata_channel * ch)
{
    while (ch->owned && ata_can_sleep())
        process_sleep(ata_owner_wait_id(ch));
    ch->owned = true;
}

static void ata_channel_release(struct ata_channel * ch)
{
    ch->owned = false;
    process_wakeup(ata_owner_wait_id(ch));
}

static void ata_irq(uint8_t irq, void * ctx)
{
    (void)irq;
    struct ata_channel * ch = ctx;
    const uint8_t status = port_read_u8(ch->bmide + ATA_BM_STATUS);
    port_read_u8(ch->io_base + ATA_REG_STATUS);
    if (!ch->irq_wait || !(status & ATA_BM_SR_IRQ)) return;
    ch->bm_status = status;
    ch->irq_done = true;
    process_wakeup(ata_irq_wait_id(ch));
}

static void ata_dma_timeout(void * arg)
{
    struct ata_channel * ch = arg;
    ch->timed_out = true;
    process_wakeup(ata_irq_wait_id(ch));
}

static bool ata_dma_setup(ata_device * dev)
{
    if (!ata_prdt_tag || !ata_data_tag || !(dev->identity.capabilities[0] & 0x100)) return false;

    void* mem;
    if (dma_mem_alloc(ata_prdt_tag, DMA_ZERO, &mem, &dev->dma_prdt_map) != 0) return false;
    if (dma_map_load(dev->dma_prdt_map, mem, ATA_PRD_MAX * sizeof(prdt), 0) != 0) {
        dma_mem_free(mem, dev->dma_prdt_map);
        return false;
    }
    if (dma_map_create(ata_data_tag, 0, &dev->dma_data_map) != 0) {
        dma_mem_free(mem, dev->dma_prdt_map);
        return false;
    }

    dev->dma_prdt = mem;
    dev->dma_prdt_phys = dev->dma_prdt_map->segments[0].addr;
    dev->bar4 = ata_bar4;
    dev->bmide = (ata_bar4 & 0xFFFC) + (dev->io_base == 0x170 ? 8 : 0);
    return true;
}

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

#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_MAX_SECTORS_PER_CMD (ATA_DMA_MAX_SIZE / ATA_SECTOR_SIZE)

static int ata_wait_ready(const ata_device * dev, const bool need_drq, const uint32_t limit)
{
    for (uint32_t i = 0; limit == 0 || i < limit; i++) {
        const uint8_t status = port_read_u8(dev->io_base + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) return -2;
        if (!(status & ATA_SR_BSY) && (!need_drq || (status & ATA_SR_DRQ))) return 0;
    }
    return -5;
}

static bool ata_supports_lba48(const ata_device * dev)
{
    return ((const uint16_t *)&dev->identity)[83] & (1 << 10);
}

static int ata_issue(const ata_device * dev, const uint64_t lba, const uint32_t count, const uint8_t cmd28, const uint8_t cmd48)
{
    const bool lba48 = ata_supports_lba48(dev);
    if (!lba48 && lba + count > 0x10000000ULL) return -3;

    if (lba48) {
        port_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0x40 | (dev->slave << 4));
        port_io_wait();
        port_write_u8(dev->io_base + ATA_REG_SECCOUNT0, (count >> 8) & 0xFF);
        port_write_u8(dev->io_base + ATA_REG_LBA0, (lba >> 24) & 0xFF);
        port_write_u8(dev->io_base + ATA_REG_LBA1, (lba >> 32) & 0xFF);
        port_write_u8(dev->io_base + ATA_REG_LBA2, (lba >> 40) & 0xFF);
    } else {
        port_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xE0 | (dev->slave << 4) | ((lba >> 24) & 0x0F));
        port_io_wait();
    }

    port_write_u8(dev->io_base + ATA_REG_SECCOUNT0, count & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_LBA0, lba & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_LBA1, (lba >> 8) & 0xFF);
    port_write_u8(dev->io_base + ATA_REG_LBA2, (lba >> 16) & 0xFF);

    port_write_u8(dev->io_base + ATA_REG_COMMAND, lba48 ? cmd48 : cmd28);
    ata_io_wait(dev);
    return 0;
}

static int ata_pio_transfer(const ata_device * dev, const uint64_t lba, const uint32_t count, uint8_t * buffer, const bool write)
{
    const int issued = write ? ata_issue(dev, lba, count, ATA_CMD_WRITE_PIO, ATA_CMD_WRITE_PIO_EXT)
                             : ata_issue(dev, lba, count, ATA_CMD_READ_PIO, ATA_CMD_READ_PIO_EXT);
    if (issued != 0) return issued;

    const uint16_t data_port = dev->io_base + ATA_REG_DATA;
    for (uint32_t i = 0; i < count; i++) {
        if (ata_wait_ready(dev, true, ATA_DMA_TIMEOUT) != 0) return -2;
        uint8_t * sector = buffer + i * ATA_SECTOR_SIZE;
        uint64_t words = ATA_SECTOR_SIZE / 2;
        if (write) asm volatile("rep outsw" : "+S"(sector), "+c"(words) : "d"(data_port) : "memory");
        else asm volatile("rep insw" : "+D"(sector), "+c"(words) : "d"(data_port) : "memory");
    }

    return ata_wait_ready(dev, false, ATA_DMA_TIMEOUT) == 0 ? 0 : -4;
}

static int ata_dma_wait(const ata_device * dev)
{
    for (uint32_t i = 0; i < ATA_DMA_TIMEOUT; i++) {
        const uint8_t status = port_read_u8(dev->bmide + ATA_BM_STATUS);
        if (status & ATA_BM_SR_ERR) return -2;
        if (status & ATA_BM_SR_IRQ) return 0;
        if (!(status & ATA_BM_SR_ACTIVE) && !(port_read_u8(dev->control) & ATA_SR_BSY)) return 0;
    }
    return -5;
}

static void ata_dma_clear(const ata_device * dev)
{
    port_write_u8(dev->bmide + ATA_BM_STATUS, port_read_u8(dev->bmide + ATA_BM_STATUS) | ATA_BM_SR_ERR | ATA_BM_SR_IRQ);
}

static void ata_recover(const ata_device * dev)
{
    if (dev->bmide) {
        port_write_u8(dev->bmide + ATA_BM_COMMAND, 0);
        ata_dma_clear(dev);
    }
    port_write_u8(dev->control, ATA_CTRL_SRST);
    for (int i = 0; i < 4; i++) ata_io_wait(dev);
    port_write_u8(dev->control, 0);
    for (int i = 0; i < 4; i++) ata_io_wait(dev);
    for (uint32_t i = 0; i < ATA_DMA_TIMEOUT && (port_read_u8(dev->io_base + ATA_REG_STATUS) & ATA_SR_BSY); i++);
    if (dev->bmide) ata_dma_clear(dev);
}

static int ata_dma_sleep(struct ata_channel * ch)
{
    ch->timed_out = false;
    callout_reset(&ch->timer, ticks_from_ms(ATA_DMA_TIMEOUT_MS), ata_dma_timeout, ch);
    while (!ch->irq_done && !ch->timed_out)
        process_sleep(ata_irq_wait_id(ch));
    callout_stop(&ch->timer);
    ch->irq_wait = false;
    if (!ch->irq_done) return -5;
    return ch->bm_status & ATA_BM_SR_ERR ? -2 : 0;
}

static void ata_dma_pin(const struct dma_map * map, const bool pin)
{
    if (map->bounced) return;
    for (int i = 0; i < map->num_segments; i++) {
        const uint64_t end = map->segments[i].addr + map->segments[i].len;
        for (uint64_t page = map->segments[i].addr & ~0xFFFULL; page < end; page += 0x1000) {
            if (pin) pmm_share_page((void *)page);
            else free_physical_page((void *)page);
        }
    }
}

static int ata_dma_transfer(const ata_device * dev, const uint64_t lba, const uint32_t count, const bool write)
{
    struct dma_map * map = dev->dma_data_map;
    struct ata_channel * ch = ata_channel_of(dev);
    const bool sleep = ata_can_sleep();
    for (int i = 0; i < map->num_segments; i++) {
        dev->dma_prdt[i].offset = (uint32_t)map->segments[i].addr;
        dev->dma_prdt[i].bytes = (uint16_t)map->segments[i].len;
        dev->dma_prdt[i].last = i == map->num_segments - 1 ? ATA_PRD_LAST : 0;
    }
    dma_map_sync(dev->dma_prdt_map, DMA_SYNC_PREWRITE);
    dma_map_sync(map, write ? DMA_SYNC_PREWRITE : DMA_SYNC_PREREAD);

    const uint8_t direction = write ? 0 : ATA_BM_CMD_READ;
    port_write_u8(dev->bmide + ATA_BM_COMMAND, 0);
    port_write_u32(dev->bmide + ATA_BM_PRDT, (uint32_t)dev->dma_prdt_phys);
    ata_dma_clear(dev);
    port_write_u8(dev->bmide + ATA_BM_COMMAND, direction);

    if (sleep) {
        ata_dma_pin(map, true);
        ch->irq_done = false;
        ch->irq_wait = true;
    }
    int result = write ? ata_issue(dev, lba, count, ATA_CMD_WRITE_DMA, ATA_CMD_WRITE_DMA_EXT)
                       : ata_issue(dev, lba, count, ATA_CMD_READ_DMA, ATA_CMD_READ_DMA_EXT);
    if (result == 0) {
        port_write_u8(dev->bmide + ATA_BM_COMMAND, direction | ATA_BM_CMD_START);
        result = sleep ? ata_dma_sleep(ch) : ata_dma_wait(dev);
        port_write_u8(dev->bmide + ATA_BM_COMMAND, direction);
        if (result == 0 && ata_wait_ready(dev, false, ATA_DMA_TIMEOUT) != 0) result = -4;
    }
    if (result != 0) ata_recover(dev);
    ch->irq_wait = false;
    ata_dma_clear(dev);

    dma_map_sync(map, write ? DMA_SYNC_POSTWRITE : DMA_SYNC_POSTREAD);
    if (sleep) ata_dma_pin(map, false);
    dma_map_unload(map);
    return result;
}

static int ata_transfer(const ata_device * dev, const uint64_t lba, const uint32_t count, uint8_t * buffer, const bool write)
{
    if (buffer == nullptr || count == 0 || count > ATA_MAX_SECTORS_PER_CMD) return -1;

    for (int attempt = 0;; attempt++) {
        int result;
        if (dev->bmide && dma_map_load(dev->dma_data_map, buffer, (dma_size_t)count * ATA_SECTOR_SIZE, DMA_NOWAIT) == 0)
            result = ata_dma_transfer(dev, lba, count, write);
        else {
            result = ata_pio_transfer(dev, lba, count, buffer, write);
            if (result != 0 && result != -3) ata_recover(dev);
        }
        if (result == 0 || result == -3 || attempt == ATA_RETRIES) return result;
    }
}

int ata_read_sector(const ata_device * dev, const uint32_t lba, uint16_t * buffer)
{
    return ata_transfer(dev, lba, 1, (uint8_t *)buffer, false);
}

static int ata_flush(const ata_device * dev)
{
    port_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xE0 | (dev->slave << 4));
    port_io_wait();
    port_write_u8(dev->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_ready(dev, false, 0);
}

static uint32_t ata_full_sectors(const ata_device * dev, const uint64_t pos, const uint64_t left)
{
    if (pos % ATA_SECTOR_SIZE != 0 || left < ATA_SECTOR_SIZE) return 0;
    const uint64_t max = ata_supports_lba48(dev) ? ATA_MAX_SECTORS_PER_CMD : 256;
    const uint64_t sectors = left / ATA_SECTOR_SIZE;
    return sectors > max ? (uint32_t)max : (uint32_t)sectors;
}

static int ata_read_bytes(ata_device * dev, const uint64_t begin_byte, const uint64_t end_byte, uint8_t * buffer)
{
    for (uint64_t pos = begin_byte; pos < end_byte;) {
        const uint64_t lba = pos / ATA_SECTOR_SIZE;
        uint8_t * out = buffer + (pos - begin_byte);
        const uint32_t sectors = ata_full_sectors(dev, pos, end_byte - pos);

        if (sectors) {
            const int result = ata_transfer(dev, lba, sectors, out, false);
            if (result != 0) return result;
            pos += (uint64_t)sectors * ATA_SECTOR_SIZE;
            continue;
        }

        const uint32_t in = pos % ATA_SECTOR_SIZE;
        const uint64_t n = end_byte - pos < ATA_SECTOR_SIZE - in ? end_byte - pos : ATA_SECTOR_SIZE - in;
        const int result = ata_transfer(dev, lba, 1, dev->bounce, false);
        if (result != 0) return result;
        memcpy(out, dev->bounce + in, n);
        pos += n;
    }
    return 0;
}

static int ata_write_bytes(ata_device * dev, const uint64_t begin_byte, const uint64_t end_byte, const uint8_t * buffer)
{
    for (uint64_t pos = begin_byte; pos < end_byte;) {
        const uint64_t lba = pos / ATA_SECTOR_SIZE;
        const uint8_t * src = buffer + (pos - begin_byte);
        const uint32_t sectors = ata_full_sectors(dev, pos, end_byte - pos);

        if (sectors) {
            const int result = ata_transfer(dev, lba, sectors, (uint8_t *)src, true);
            if (result != 0) return result;
            pos += (uint64_t)sectors * ATA_SECTOR_SIZE;
            continue;
        }

        const uint32_t in = pos % ATA_SECTOR_SIZE;
        const uint64_t n = end_byte - pos < ATA_SECTOR_SIZE - in ? end_byte - pos : ATA_SECTOR_SIZE - in;
        int result = ata_transfer(dev, lba, 1, dev->bounce, false);
        if (result != 0) return result;
        memcpy(dev->bounce + in, src, n);
        result = ata_transfer(dev, lba, 1, dev->bounce, true);
        if (result != 0) return result;
        pos += n;
    }
    return 0;
}

int ata_read_until(ata_device * dev, const uint32_t lba, const uint32_t n_bytes, uint8_t *buffer)
{
    const uint64_t begin = (uint64_t)lba * ATA_SECTOR_SIZE;
    return ata_read_bytes(dev, begin, begin + n_bytes, buffer);
}

int ata_read(const uint32_t unique_id, const uint64_t begin_byte, const uint64_t end_byte, uint8_t* buffer)
{
    ata_device* dev = map_get(ata_disk_registry, (void*)(uintptr_t)unique_id);
    if (dev == nullptr) return -2;
    ata_channel_acquire(ata_channel_of(dev));
    const int result = ata_read_bytes(dev, begin_byte, end_byte, buffer);
    ata_channel_release(ata_channel_of(dev));
    return result;
}

int ata_write(const uint32_t unique_id, const uint64_t begin_byte, const uint64_t end_byte, const uint8_t *buffer)
{
    ata_device* dev = map_get(ata_disk_registry, (void*)(uintptr_t)unique_id);
    if (dev == nullptr) return -2;
    ata_channel_acquire(ata_channel_of(dev));
    const int result = ata_write_bytes(dev, begin_byte, end_byte, buffer);
    ata_channel_release(ata_channel_of(dev));
    return result;
}

int ata_flush_device(const uint32_t unique_id)
{
    const ata_device* dev = map_get(ata_disk_registry, (void*)(uintptr_t)unique_id);
    if (dev == nullptr) return -2;
    ata_channel_acquire(ata_channel_of(dev));
    const int result = ata_flush(dev);
    ata_channel_release(ata_channel_of(dev));
    return result;
}

boot_type detect_boot_type(const ata_device* dev) {
    uint8_t* sector = malloc(512);
    if (!sector) return BOOT_UNKNOWN;

    if (ata_read_sector(dev, 17, (uint16_t*)sector) == 0) {
        if (is_el_torito(sector)) {
            free(sector);
            return BOOT_EL_TORITO;
        }
    }

    if (ata_read_sector(dev, 1, (uint16_t*)sector) == 0) {
        if (is_gpt(sector)) {
            free(sector);
            return BOOT_GPT;
        }
    }

    free(sector);
    return BOOT_UNKNOWN;
}

void ata_get_gpt(ata_device * dev, full_gpt* gpt)
{
    if (gpt == nullptr) return;

    gpt->boot_record = (master_boot_record*)malloc(sizeof(master_boot_record) + 8);
    gpt->header = (gpt_header*)malloc(sizeof(gpt_header) + 8);

    ata_read_until(dev, 0, sizeof(master_boot_record), (uint8_t*)gpt->boot_record);
    ata_read_until(dev, 1, sizeof(gpt_header), (uint8_t*)gpt->header);

    uint16_t* sector_buffer = malloc(ATA_SECTOR_SIZE);
    if (sector_buffer == nullptr) panic("Failed to allocate memory for GPT sector buffer", __FILE__, __LINE__);
    const uint64_t entries_lba = gpt->header->lba_guid_partition_start;
    const uint32_t entry_size = gpt->header->partition_entry_size;
    const uint32_t entries_per_sector = ATA_SECTOR_SIZE / entry_size;

    for (uint32_t i = 0; i < 128; i++) {
        const uint32_t sector_offset = i / entries_per_sector;
        const uint32_t entry_offset = i % entries_per_sector;

        if (entry_offset == 0)
            ata_read_sector(dev, entries_lba + sector_offset, sector_buffer);

        gpt_entry* tmp_entry = malloc(sizeof(gpt_entry));
        if (tmp_entry == nullptr) panic("Failed to allocate memory for GPT entry", __FILE__, __LINE__);
        memcpy(tmp_entry,
               (uint8_t*)sector_buffer + entry_offset * entry_size,
               entry_size);
        tmp_entry->part_type = FS_UNKNOWN;

        if (!is_valid_gpt_entry(tmp_entry))
            continue;
        tmp_entry->part_type = gpt_partition_fs_type(tmp_entry);
        gpt->entries[i] = tmp_entry;

        if (gpt->entries[i]->part_type == FS_UNKNOWN) {
            terminal_printf("Unknown partition type: ");
            for (int j = 0; j < 16; j++) {
                terminal_printf("%02X", gpt->entries[i]->partition_guid[j]);
            }
            terminal_printf("\n");
        }
    }

    free(sector_buffer);
}

void ata_device_initialize(ata_device * dev)
{
    dev->is_atapi = false;
    if (!ata_dma_setup(dev))
        terminal_printf("ata: bus-master DMA unavailable, using PIO\n");

    port_write_u8(dev->io_base + 1, 1);
    port_write_u8(dev->control, 0);

    ata_select_device(dev);
    ata_io_wait(dev);

    full_gpt* gpt = malloc(sizeof(full_gpt));

    const boot_type boot_type = detect_boot_type(dev);

    if (boot_type == BOOT_GPT)
    {
        ata_get_gpt(dev, gpt);

        for (int i = 0; i < 128; i++) {
            if (gpt->entries[i] != nullptr) {
                disk_node *node = malloc(sizeof(disk_node));
                node->unique_id = (uint32_t)(uintptr_t)dev;
                node->length = gpt->entries[i]->lba_end - gpt->entries[i]->lba_start;
                node->offset = gpt->entries[i]->lba_start;
                node->part_type = gpt_partition_fs_type(gpt->entries[i]);
                node->disk_type = DISK_ATA;
                node->name = str_utf16_to_utf8(gpt->entries[i]->partition_name);

                map_put(ata_disk_registry, (void*)(uintptr_t)node->unique_id, dev);

                const uint64_t part_off = node->offset * ATA_SECTOR_SIZE;
                const uint64_t part_len = node->length * ATA_SECTOR_SIZE;
                struct block_device *bdev = fat32_create_bdev(
                    node->unique_id, part_off, part_len, &ata_bdev_ops, dev);

                const char *fs_name = nullptr;
                switch (node->part_type) {
                case FS_FAT32:
                case FS_EFI_SYSTEM:
                    fs_name = "fat32";
                    break;
                default:
                    break;
                }

                if (fs_name && bdev) {
                    struct block_device *cached = bcache_attach(bdev);
                    fs_mount_partition(fs_name, node->name, cached ? cached : bdev);
                } else {
                    free(bdev);
                }
            }
        }
    } else if (boot_type == BOOT_EL_TORITO)
        terminal_printf("El Torito boot detected\n");
    else
        terminal_printf("Unknown boot type\n");

    free(gpt);
}

void atapi_device_initialize(ata_device * dev)
{
}

void ata_device_detect(ata_device * dev)
{
    port_write_u8(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4);
    ata_io_wait(dev);

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
        const uint8_t cl = port_read_u8(dev->io_base + ATA_REG_LBA1);
        const uint8_t ch = port_read_u8(dev->io_base + ATA_REG_LBA2);

        if ((cl == 0x14 && ch == 0xEB) || (cl == 0x69 && ch == 0x96))
            dev->is_atapi = true;
        else return;

        port_write_u8(dev->io_base + ATA_REG_COMMAND, 0xA1);
        ata_io_wait(dev);
    }

    const auto identity = (uint16_t *)&dev->identity;
    for (int i = 0; i < 256; ++i)
        identity[i] = port_read_u16(dev->io_base);

    if (!dev->is_atapi && (identity[0] == 0 || identity[1] == 0)) return;
    if (dev->is_atapi && ((identity[0] & 0x8000) == 0 || (identity[0] & 0xFF00) != 0x8500)) return;

    dev->identity = *(ata_identify *)identity;

    if (!(dev->identity.capabilities[0] & 0x200)) return;

    if (dev->is_atapi) atapi_device_initialize(dev);
    else ata_device_initialize(dev);
}

void ata_initialize() {
    ata_disk_registry = map_create(16, nullptr, nullptr, nullptr, nullptr);

    const scan_result *result = pci_scan(-1);

    for (int i = 0; i < result->count; i++)
    {
        if (result->devices[i] == 0)
            continue;
        const pci_address addr = pci_convert_address(result->devices[i]);
        const uint16_t vendor_id = pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_VENDOR_ID);
        const uint16_t device_id = pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_DEVICE_ID);
        if (vendor_id != 0x8086 || (device_id != 0x7010 && device_id != 0x7111))
            continue;

        const uint32_t bar4 = pci_config_read_u32(addr.bus, addr.device, addr.function, PCI_BAR4);
        if (!(bar4 & 1))
            continue;
        const uint16_t command = pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_COMMAND);
        pci_config_write_u16(addr.bus, addr.device, addr.function, PCI_COMMAND, command | PCI_COMMAND_MASTER);
        ata_bar4 = bar4;
    }

    for (int i = 0; ata_bar4 && i < 2; i++) {
        ata_channels[i].bmide = (ata_bar4 & 0xFFFC) + i * 8;
        callout_init(&ata_channels[i].timer);
        irq_register_handler(14 + i, ata_irq, &ata_channels[i]);
    }

    if (ata_bar4) {
        if (dma_tag_create(nullptr, 4, ATA_PRD_BOUNDARY, DMA_ADDR_MAX_32BIT, ATA_PRD_MAX * sizeof(prdt), 1,
                           ATA_PRD_MAX * sizeof(prdt), 0, &ata_prdt_tag) != 0)
            ata_prdt_tag = nullptr;
        if (dma_tag_create(nullptr, 2, ATA_PRD_BOUNDARY, DMA_ADDR_MAX_32BIT, ATA_DMA_MAX_SIZE, ATA_PRD_MAX,
                           ATA_DMA_MAX_SIZE, 0, &ata_data_tag) != 0)
            ata_data_tag = nullptr;
    }

    ata_device_detect(&ata_primary_master);
    ata_device_detect(&ata_primary_slave);
    ata_device_detect(&ata_secondary_master);
    ata_device_detect(&ata_secondary_slave);
}
