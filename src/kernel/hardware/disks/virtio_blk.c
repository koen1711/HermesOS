#include "virtio_blk.h"

#include <os/errno.h>
#include <os/stdint.h>
#include <os/stdbool.h>

#include <drivers/fs/fs.h>
#include <drivers/fs/fat/fat32.h>
#include <drivers/fs/bcache/bcache.h>
#include <drivers/scheduler/process.h>
#include <drivers/terminal/terminal.h>
#include <drivers/timer/callout.h>
#include <hardware/interrupts/irq.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/dma.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <hardware/port/pci.h>
#include <hardware/port/ports.h>
#include <utils/str/str.h>
#include <utils/sync/spinlock.h>

#include "partition_table.h"

#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_BLK_LEGACY_ID   0x1001

#define VIO_HOST_FEATURES      0x00
#define VIO_GUEST_FEATURES     0x04
#define VIO_QUEUE_PFN          0x08
#define VIO_QUEUE_NUM          0x0C
#define VIO_QUEUE_SEL          0x0E
#define VIO_QUEUE_NOTIFY       0x10
#define VIO_STATUS             0x12
#define VIO_ISR                0x13
#define VIO_CONFIG             0x14

#define VIO_STATUS_ACK         0x01
#define VIO_STATUS_DRIVER      0x02
#define VIO_STATUS_DRIVER_OK   0x04
#define VIO_STATUS_FAILED      0x80

#define VBLK_F_SEG_MAX         (1u << 2)
#define VBLK_F_RO              (1u << 5)
#define VBLK_F_FLUSH           (1u << 9)

#define VBLK_T_IN              0
#define VBLK_T_OUT             1
#define VBLK_T_FLUSH           4

#define VRING_DESC_F_NEXT      1
#define VRING_DESC_F_WRITE     2

#define VBLK_SECTOR            512
#define VBLK_MAX_DEVICES       4
#define VBLK_MAX_INFLIGHT      32
#define VBLK_MAX_SEGS          64
#define VBLK_MAX_BYTES         (128 * 1024)
#define VBLK_POKE_MS           100
#define WAIT_CHANNEL_VBLK      0x3D000000

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vblk_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct vblk_dma {
    struct vblk_hdr hdr[VBLK_MAX_INFLIGHT];
    uint8_t status[VBLK_MAX_INFLIGHT];
};

struct vblk_req {
    bool in_use;
    bool done;
    uint16_t head;
    struct dma_map *map;
    struct callout poke;
};

struct vblk_device {
    int index;
    uint16_t io;
    uint8_t irq;
    bool read_only;
    bool can_flush;
    uint64_t sectors;
    uint32_t max_segs;
    uint32_t max_bytes;

    uint16_t qsize;
    struct vring_desc *desc;
    volatile uint16_t *avail;
    volatile uint8_t *used;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used;
    int16_t *owner;

    struct vblk_dma *dma;
    uint64_t dma_phys;
    struct vblk_req reqs[VBLK_MAX_INFLIGHT];
    struct dma_tag *tag;
    spinlock_t lock;
};

static struct vblk_device *vblk_devices[VBLK_MAX_DEVICES];
static int vblk_count;

static bool vblk_can_sleep()
{
    const process_t *current = scheduler_get()->current;
    return current != nullptr && current->pid != IDLE_PROC_PID;
}

static int vblk_wait_id(const struct vblk_device *d, const int slot)
{
    return WAIT_CHANNEL_VBLK | (d->index << 8) | slot;
}

static int vblk_free_wait_id(const struct vblk_device *d)
{
    return WAIT_CHANNEL_VBLK | (d->index << 8) | 0xFF;
}

static volatile uint16_t *used_idx(const struct vblk_device *d)
{
    return (volatile uint16_t *)(d->used + 2);
}

static volatile struct vring_used_elem *used_ring(const struct vblk_device *d)
{
    return (volatile struct vring_used_elem *)(d->used + 4);
}

static void free_chain_locked(struct vblk_device *d, uint16_t head)
{
    for (;;) {
        const uint16_t flags = d->desc[head].flags;
        const uint16_t next = d->desc[head].next;
        d->desc[head].next = d->free_head;
        d->free_head = head;
        d->num_free++;
        if (!(flags & VRING_DESC_F_NEXT))
            break;
        head = next;
    }
}

static bool reap_locked(struct vblk_device *d)
{
    bool any = false;
    while (d->last_used != *used_idx(d)) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint16_t head = (uint16_t)used_ring(d)[d->last_used % d->qsize].id;
        d->last_used++;
        const int slot = head < d->qsize ? d->owner[head] : -1;
        if (slot < 0)
            continue;
        d->owner[head] = -1;
        free_chain_locked(d, head);
        d->reqs[slot].done = true;
        process_wakeup(vblk_wait_id(d, slot));
        any = true;
    }
    if (any)
        process_wakeup(vblk_free_wait_id(d));
    return any;
}

static void vblk_irq(uint8_t irq, void *ctx)
{
    (void)irq;
    struct vblk_device *d = ctx;
    if (!(port_read_u8(d->io + VIO_ISR) & 1))
        return;
    uint64_t flags;
    spinlock_acquire_irqsave(&d->lock, &flags);
    reap_locked(d);
    spinlock_release_irqrestore(&d->lock, flags);
}

static void vblk_poke(void *arg)
{
    struct vblk_device *d = arg;
    uint64_t flags;
    spinlock_acquire_irqsave(&d->lock, &flags);
    reap_locked(d);
    spinlock_release_irqrestore(&d->lock, flags);
}

static int take_slot_locked(struct vblk_device *d, const uint16_t ndesc)
{
    if (d->num_free < ndesc)
        return -1;
    for (int i = 0; i < VBLK_MAX_INFLIGHT; i++) {
        if (!d->reqs[i].in_use)
            return i;
    }
    return -1;
}

static uint16_t take_desc_locked(struct vblk_device *d)
{
    const uint16_t i = d->free_head;
    d->free_head = d->desc[i].next;
    d->num_free--;
    return i;
}

static void dma_pin(const struct dma_map *map, const bool pin)
{
    if (map->bounced)
        return;
    for (int i = 0; i < map->num_segments; i++) {
        const uint64_t end = map->segments[i].addr + map->segments[i].len;
        for (uint64_t page = map->segments[i].addr & ~0xFFFULL; page < end; page += 0x1000) {
            if (pin)
                pmm_share_page((void *)page);
            else
                free_physical_page((void *)page);
        }
    }
}

static int vblk_request(struct vblk_device *d, const uint32_t type, const uint64_t sector, void *buf,
                        const uint32_t len)
{
    const bool write = type == VBLK_T_OUT;
    const bool sleep = vblk_can_sleep();

    uint64_t flags;
    spinlock_acquire_irqsave(&d->lock, &flags);
    int slot;
    while ((slot = take_slot_locked(d, (uint16_t)(d->max_segs + 2))) < 0) {
        if (!reap_locked(d) && sleep) {
            spinlock_release_irqrestore(&d->lock, flags);
            process_sleep_timeout(vblk_free_wait_id(d), ticks_from_ms(VBLK_POKE_MS));
            spinlock_acquire_irqsave(&d->lock, &flags);
        }
    }
    struct vblk_req *req = &d->reqs[slot];
    req->in_use = true;
    req->done = false;
    spinlock_release_irqrestore(&d->lock, flags);

    struct dma_map *map = req->map;
    if (len) {
        const int err = dma_map_load(map, buf, len, 0);
        if (err) {
            spinlock_acquire_irqsave(&d->lock, &flags);
            req->in_use = false;
            spinlock_release_irqrestore(&d->lock, flags);
            process_wakeup(vblk_free_wait_id(d));
            return err;
        }
        dma_map_sync(map, write ? DMA_SYNC_PREWRITE : DMA_SYNC_PREREAD);
        if (sleep)
            dma_pin(map, true);
    }

    d->dma->hdr[slot].type = type;
    d->dma->hdr[slot].reserved = 0;
    d->dma->hdr[slot].sector = sector;
    d->dma->status[slot] = 0xFF;

    spinlock_acquire_irqsave(&d->lock, &flags);
    const uint16_t head = take_desc_locked(d);
    d->desc[head].addr = d->dma_phys + __builtin_offsetof(struct vblk_dma, hdr) + slot * sizeof(struct vblk_hdr);
    d->desc[head].len = sizeof(struct vblk_hdr);
    d->desc[head].flags = VRING_DESC_F_NEXT;
    uint16_t prev = head;
    for (int i = 0; len && i < map->num_segments; i++) {
        const uint16_t n = take_desc_locked(d);
        d->desc[n].addr = map->segments[i].addr;
        d->desc[n].len = (uint32_t)map->segments[i].len;
        d->desc[n].flags = VRING_DESC_F_NEXT | (write ? 0 : VRING_DESC_F_WRITE);
        d->desc[prev].next = n;
        prev = n;
    }
    const uint16_t tail = take_desc_locked(d);
    d->desc[tail].addr = d->dma_phys + __builtin_offsetof(struct vblk_dma, status) + slot;
    d->desc[tail].len = 1;
    d->desc[tail].flags = VRING_DESC_F_WRITE;
    d->desc[prev].next = tail;

    req->head = head;
    d->owner[head] = (int16_t)slot;
    const uint16_t avail_idx = d->avail[1];
    d->avail[2 + avail_idx % d->qsize] = head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->avail[1] = (uint16_t)(avail_idx + 1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    spinlock_release_irqrestore(&d->lock, flags);
    port_write_u16(d->io + VIO_QUEUE_NOTIFY, 0);

    for (;;) {
        spinlock_acquire_irqsave(&d->lock, &flags);
        reap_locked(d);
        const bool done = req->done;
        spinlock_release_irqrestore(&d->lock, flags);
        if (done)
            break;
        if (sleep) {
            callout_reset(&req->poke, ticks_from_ms(VBLK_POKE_MS), vblk_poke, d);
            process_sleep(vblk_wait_id(d, slot));
            callout_stop(&req->poke);
        } else {
            asm volatile("pause");
        }
    }

    const uint8_t status = d->dma->status[slot];
    if (len) {
        dma_map_sync(map, write ? DMA_SYNC_POSTWRITE : DMA_SYNC_POSTREAD);
        if (sleep)
            dma_pin(map, false);
        dma_map_unload(map);
    }

    spinlock_acquire_irqsave(&d->lock, &flags);
    req->in_use = false;
    spinlock_release_irqrestore(&d->lock, flags);
    process_wakeup(vblk_free_wait_id(d));
    return status == 0 ? 0 : -EIO;
}

static int vblk_rw_aligned(struct vblk_device *d, uint64_t off, uint32_t len, uint8_t *buf, const bool write)
{
    while (len) {
        const uint32_t n = len < d->max_bytes ? len : d->max_bytes;
        const int err = vblk_request(d, write ? VBLK_T_OUT : VBLK_T_IN, off / VBLK_SECTOR, buf, n);
        if (err)
            return err;
        off += n;
        buf += n;
        len -= n;
    }
    return 0;
}

static int vblk_rw(struct vblk_device *d, const uint64_t off, const uint32_t len, void *buf, const bool write)
{
    if (len == 0)
        return 0;
    if ((off + len + VBLK_SECTOR - 1) / VBLK_SECTOR > d->sectors)
        return -EIO;
    if (write && d->read_only)
        return -EROFS;
    if (off % VBLK_SECTOR == 0 && len % VBLK_SECTOR == 0)
        return vblk_rw_aligned(d, off, len, buf, write);

    const uint64_t start = off & ~(uint64_t)(VBLK_SECTOR - 1);
    const uint64_t end = (off + len + VBLK_SECTOR - 1) & ~(uint64_t)(VBLK_SECTOR - 1);
    uint8_t *tmp = malloc(end - start);
    if (!tmp)
        return -ENOMEM;
    int err = vblk_rw_aligned(d, start, (uint32_t)(end - start), tmp, false);
    if (!err && !write)
        memcpy(buf, tmp + (off - start), len);
    if (!err && write) {
        memcpy(tmp + (off - start), buf, len);
        err = vblk_rw_aligned(d, start, (uint32_t)(end - start), tmp, true);
    }
    free(tmp);
    return err;
}

static int vblk_bdev_read(struct block_device *bdev, const uint64_t off, const uint32_t len, void *buf)
{
    return vblk_rw(bdev->private, bdev->part_offset + off, len, buf, false);
}

static int vblk_bdev_write(struct block_device *bdev, const uint64_t off, const uint32_t len, const void *buf)
{
    return vblk_rw(bdev->private, bdev->part_offset + off, len, (void *)buf, true);
}

static int vblk_bdev_flush(struct block_device *bdev)
{
    struct vblk_device *d = bdev->private;
    return d->can_flush ? vblk_request(d, VBLK_T_FLUSH, 0, nullptr, 0) : 0;
}

static const struct block_device_ops vblk_bdev_ops = {
    .read_bytes  = vblk_bdev_read,
    .write_bytes = vblk_bdev_write,
    .flush       = vblk_bdev_flush,
};

static bool vblk_setup_queue(struct vblk_device *d)
{
    port_write_u16(d->io + VIO_QUEUE_SEL, 0);
    d->qsize = port_read_u16(d->io + VIO_QUEUE_NUM);
    if (d->qsize == 0)
        return false;

    const uint64_t avail_end = (uint64_t)d->qsize * sizeof(struct vring_desc) + 6 + 2ULL * d->qsize;
    const uint64_t used_off = PAGE_ALIGN_UP(avail_end);
    const uint64_t total = used_off + PAGE_ALIGN_UP(6 + 8ULL * d->qsize);
    uint8_t *ring = alloc_kernel_pages(total / PAGE_SIZE);
    if (!ring)
        return false;
    memset(ring, 0, total);

    d->desc = (struct vring_desc *)ring;
    d->avail = (volatile uint16_t *)(ring + (uint64_t)d->qsize * sizeof(struct vring_desc));
    d->used = ring + used_off;
    d->owner = malloc(d->qsize * sizeof(int16_t));
    if (!d->owner)
        return false;
    for (uint16_t i = 0; i < d->qsize; i++) {
        d->desc[i].next = (uint16_t)(i + 1);
        d->owner[i] = -1;
    }
    d->free_head = 0;
    d->num_free = d->qsize;

    port_write_u32(d->io + VIO_QUEUE_PFN, (uint32_t)(virt_to_phys(ring) >> 12));
    return true;
}

static int read_disk(struct vblk_device *d, const uint64_t off, const uint32_t len, void *buf)
{
    return vblk_rw(d, off, len, buf, false);
}

static void vblk_mount_partitions(struct vblk_device *d)
{
    uint8_t *sector = malloc(VBLK_SECTOR);
    gpt_header *header = malloc(sizeof(gpt_header) + 8);
    if (!sector || !header || read_disk(d, VBLK_SECTOR, VBLK_SECTOR, sector) != 0
        || memcmp(sector, (void *)"EFI PART", 8) != 0) {
        terminal_printf("virtio-blk%d: no GPT partition table\n", d->index);
        free(sector);
        free(header);
        return;
    }
    memcpy(header, sector, sizeof(gpt_header));

    const uint32_t entry_size = header->partition_entry_size;
    const uint32_t entries = header->partition_amount < 128 ? header->partition_amount : 128;
    if (entry_size < sizeof(gpt_entry) - sizeof(partition_type) || entry_size > VBLK_SECTOR) {
        free(sector);
        free(header);
        return;
    }
    const uint32_t per_sector = VBLK_SECTOR / entry_size;

    for (uint32_t i = 0; i < entries; i++) {
        if (i % per_sector == 0
            && read_disk(d, (header->lba_guid_partition_start + i / per_sector) * VBLK_SECTOR, VBLK_SECTOR,
                         sector) != 0)
            break;
        gpt_entry entry;
        memset(&entry, 0, sizeof(entry));
        memcpy(&entry, sector + (i % per_sector) * entry_size, sizeof(gpt_entry) - sizeof(partition_type));
        if (entry.partition_type_guid[0] == 0 && entry.partition_type_guid[1] == 0)
            continue;

        const partition_type type = gpt_partition_fs_type(&entry);
        if (type != FS_FAT32 && type != FS_EFI_SYSTEM)
            continue;

        const uint64_t part_off = entry.lba_start * VBLK_SECTOR;
        const uint64_t part_len = (entry.lba_end - entry.lba_start) * VBLK_SECTOR;
        struct block_device *bdev =
            fat32_create_bdev((uint32_t)(uintptr_t)d + i, part_off, part_len, &vblk_bdev_ops, d);
        if (!bdev)
            continue;
        char *name = str_utf16_to_utf8(entry.partition_name);
        struct block_device *cached = bcache_attach(bdev);
        fs_mount_partition("fat32", name, cached ? cached : bdev);
    }
    free(sector);
    free(header);
}

static void vblk_probe(const pci_address addr)
{
    if (vblk_count >= VBLK_MAX_DEVICES)
        return;
    const uint32_t bar0 = pci_config_read_u32(addr.bus, addr.device, addr.function, PCI_BAR0);
    if (!(bar0 & 1))
        return;

    struct vblk_device *d = malloc(sizeof(struct vblk_device));
    if (!d)
        return;
    d->index = vblk_count;
    d->io = (uint16_t)(bar0 & ~3u);
    d->irq = pci_config_read_u8(addr.bus, addr.device, addr.function, PCI_INTERRUPT_LINE);
    spinlock_init(&d->lock, "virtio-blk");

    const uint16_t command = pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_COMMAND);
    pci_config_write_u16(addr.bus, addr.device, addr.function, PCI_COMMAND, command | PCI_COMMAND_MASTER | 1);

    port_write_u8(d->io + VIO_STATUS, 0);
    port_write_u8(d->io + VIO_STATUS, VIO_STATUS_ACK);
    port_write_u8(d->io + VIO_STATUS, VIO_STATUS_ACK | VIO_STATUS_DRIVER);

    const uint32_t host = port_read_u32(d->io + VIO_HOST_FEATURES);
    const uint32_t guest = host & (VBLK_F_SEG_MAX | VBLK_F_RO | VBLK_F_FLUSH);
    port_write_u32(d->io + VIO_GUEST_FEATURES, guest);
    d->read_only = guest & VBLK_F_RO;
    d->can_flush = guest & VBLK_F_FLUSH;
    d->sectors = port_read_u32(d->io + VIO_CONFIG) | (uint64_t)port_read_u32(d->io + VIO_CONFIG + 4) << 32;

    uint32_t segs = VBLK_MAX_SEGS;
    if (guest & VBLK_F_SEG_MAX) {
        const uint32_t seg_max = port_read_u32(d->io + VIO_CONFIG + 12);
        if (seg_max && seg_max < segs)
            segs = seg_max;
    }

    if (!vblk_setup_queue(d)) {
        port_write_u8(d->io + VIO_STATUS, VIO_STATUS_FAILED);
        terminal_printf("virtio-blk: queue setup failed\n");
        return;
    }
    if (segs + 2 > d->qsize)
        segs = d->qsize > 2 ? d->qsize - 2 : 1;
    d->max_segs = segs;
    d->max_bytes = segs > 1 ? (segs - 1) * PAGE_SIZE : VBLK_SECTOR * 8;
    if (d->max_bytes > VBLK_MAX_BYTES)
        d->max_bytes = VBLK_MAX_BYTES;

    d->dma = alloc_kernel_pages(PAGE_ALIGN_UP(sizeof(struct vblk_dma)) / PAGE_SIZE);
    if (!d->dma || dma_tag_create(nullptr, 1, 0, DMA_ADDR_MAX, d->max_bytes, (int)segs, d->max_bytes, 0,
                                  &d->tag) != 0) {
        port_write_u8(d->io + VIO_STATUS, VIO_STATUS_FAILED);
        terminal_printf("virtio-blk: DMA setup failed\n");
        return;
    }
    memset(d->dma, 0, sizeof(struct vblk_dma));
    d->dma_phys = virt_to_phys(d->dma);
    for (int i = 0; i < VBLK_MAX_INFLIGHT; i++) {
        callout_init(&d->reqs[i].poke);
        if (dma_map_create(d->tag, 0, &d->reqs[i].map) != 0) {
            port_write_u8(d->io + VIO_STATUS, VIO_STATUS_FAILED);
            return;
        }
    }

    if (d->irq < IRQ_MAX_LINES)
        irq_register_handler(d->irq, vblk_irq, d);
    port_write_u8(d->io + VIO_STATUS, VIO_STATUS_ACK | VIO_STATUS_DRIVER | VIO_STATUS_DRIVER_OK);

    vblk_devices[vblk_count++] = d;
    terminal_printf("virtio-blk%d: %u MiB, queue %u, irq %u\n", d->index, (uint32_t)(d->sectors / 2048),
                    d->qsize, d->irq);
    vblk_mount_partitions(d);
}

void virtio_blk_initialize()
{
    const scan_result *result = pci_scan(-1);
    for (uint32_t i = 0; i < result->count; i++) {
        if (result->devices[i] == 0)
            continue;
        const pci_address addr = pci_convert_address(result->devices[i]);
        if (pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_VENDOR_ID) == VIRTIO_VENDOR
            && pci_config_read_u16(addr.bus, addr.device, addr.function, PCI_DEVICE_ID) == VIRTIO_BLK_LEGACY_ID)
            vblk_probe(addr);
    }
}
