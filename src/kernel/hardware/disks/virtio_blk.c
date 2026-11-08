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
#include <hardware/virtio/virtio.h>
#include <utils/str/str.h>
#include <utils/sync/spinlock.h>

#include "partition_table.h"

#define VIRTIO_BLK_LEGACY_ID   0x1001

#define VBLK_F_SEG_MAX         (1u << 2)
#define VBLK_F_RO              (1u << 5)
#define VBLK_F_FLUSH           (1u << 9)

#define VBLK_T_IN              0
#define VBLK_T_OUT             1
#define VBLK_T_FLUSH           4

#define VBLK_SECTOR            512
#define VBLK_MAX_DEVICES       4
#define VBLK_MAX_INFLIGHT      32
#define VBLK_MAX_SEGS          64
#define VBLK_MAX_BYTES         (128 * 1024)
#define VBLK_POKE_MS           100
#define WAIT_CHANNEL_VBLK      0x3D000000

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
    struct virtio_device vdev;
    bool read_only;
    bool can_flush;
    uint64_t sectors;
    uint32_t max_segs;
    uint32_t max_bytes;

    struct virtqueue vq;
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

static bool reap_locked(struct vblk_device *d)
{
    bool any = false;
    uint16_t head;
    while (virtqueue_get(&d->vq, &head, nullptr)) {
        const int slot = d->owner[head];
        if (slot < 0)
            continue;
        d->owner[head] = -1;
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
    if (!(virtio_isr(&d->vdev) & VIRTIO_ISR_QUEUE))
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
    for (int i = 0; i < VBLK_MAX_INFLIGHT; i++) {
        if (d->reqs[i].in_use && d->reqs[i].done)
            process_wakeup(vblk_wait_id(d, i));
    }
    spinlock_release_irqrestore(&d->lock, flags);
}

static int take_slot_locked(struct vblk_device *d, const uint16_t ndesc)
{
    if (d->vq.num_free < ndesc)
        return -1;
    for (int i = 0; i < VBLK_MAX_INFLIGHT; i++) {
        if (!d->reqs[i].in_use)
            return i;
    }
    return -1;
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

    struct virtio_buf bufs[VBLK_MAX_SEGS + 2];
    uint16_t n = 0;
    bufs[n++] = (struct virtio_buf){
        d->dma_phys + __builtin_offsetof(struct vblk_dma, hdr) + slot * sizeof(struct vblk_hdr), sizeof(struct vblk_hdr),
        false};
    for (int i = 0; len && i < map->num_segments; i++)
        bufs[n++] = (struct virtio_buf){map->segments[i].addr, (uint32_t)map->segments[i].len, !write};
    bufs[n++] = (struct virtio_buf){d->dma_phys + __builtin_offsetof(struct vblk_dma, status) + slot, 1, true};

    spinlock_acquire_irqsave(&d->lock, &flags);
    int head;
    while ((head = virtqueue_add(&d->vq, bufs, n)) < 0) {
        if (reap_locked(d))
            continue;
        spinlock_release_irqrestore(&d->lock, flags);
        if (sleep)
            process_sleep_timeout(vblk_free_wait_id(d), ticks_from_ms(VBLK_POKE_MS));
        else
            asm volatile("pause");
        spinlock_acquire_irqsave(&d->lock, &flags);
    }
    req->head = (uint16_t)head;
    d->owner[head] = (int16_t)slot;
    spinlock_release_irqrestore(&d->lock, flags);
    virtqueue_kick(&d->vq);

    for (;;) {
        const uint64_t iflags = irq_save();
        spinlock_acquire_irqsave(&d->lock, &flags);
        reap_locked(d);
        const bool done = req->done;
        spinlock_release_irqrestore(&d->lock, flags);
        if (!done && sleep) {
            callout_reset(&req->poke, ticks_from_ms(VBLK_POKE_MS), vblk_poke, d);
            process_sleep(vblk_wait_id(d, slot));
        }
        irq_restore(iflags);
        if (done)
            break;
        if (sleep)
            callout_stop(&req->poke);
        else
            asm volatile("pause");
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
    if (!virtqueue_setup(&d->vdev, &d->vq, 0))
        return false;
    d->owner = malloc(d->vq.size * sizeof(int16_t));
    if (!d->owner)
        return false;
    for (uint16_t i = 0; i < d->vq.size; i++)
        d->owner[i] = -1;
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
    struct vblk_device *d = malloc_safe(sizeof(struct vblk_device));
    if (!d)
        return;
    if (!virtio_attach(&d->vdev, addr)) {
        free(d);
        return;
    }
    d->index = vblk_count;
    spinlock_init(&d->lock, "virtio-blk");

    const uint32_t guest = virtio_negotiate(&d->vdev, VBLK_F_SEG_MAX | VBLK_F_RO | VBLK_F_FLUSH);
    d->read_only = guest & VBLK_F_RO;
    d->can_flush = guest & VBLK_F_FLUSH;
    d->sectors = virtio_config_u32(&d->vdev, 0) | (uint64_t)virtio_config_u32(&d->vdev, 4) << 32;

    uint32_t segs = VBLK_MAX_SEGS;
    if (guest & VBLK_F_SEG_MAX) {
        const uint32_t seg_max = virtio_config_u32(&d->vdev, 12);
        if (seg_max && seg_max < segs)
            segs = seg_max;
    }

    if (!vblk_setup_queue(d)) {
        virtio_fail(&d->vdev);
        terminal_printf("virtio-blk: queue setup failed\n");
        return;
    }
    if (segs + 2 > d->vq.size)
        segs = d->vq.size > 2 ? d->vq.size - 2 : 1;
    d->max_segs = segs;
    d->max_bytes = segs > 1 ? (segs - 1) * PAGE_SIZE : VBLK_SECTOR * 8;
    if (d->max_bytes > VBLK_MAX_BYTES)
        d->max_bytes = VBLK_MAX_BYTES;

    d->dma = alloc_kernel_pages(PAGE_ALIGN_UP(sizeof(struct vblk_dma)) / PAGE_SIZE);
    if (!d->dma || dma_tag_create(nullptr, 1, 0, DMA_ADDR_MAX, d->max_bytes, (int)segs, d->max_bytes, 0,
                                  &d->tag) != 0) {
        virtio_fail(&d->vdev);
        terminal_printf("virtio-blk: DMA setup failed\n");
        return;
    }
    memset(d->dma, 0, sizeof(struct vblk_dma));
    d->dma_phys = virt_to_phys(d->dma);
    for (int i = 0; i < VBLK_MAX_INFLIGHT; i++) {
        callout_init(&d->reqs[i].poke);
        if (dma_map_create(d->tag, 0, &d->reqs[i].map) != 0) {
            virtio_fail(&d->vdev);
            return;
        }
    }

    if (d->vdev.irq < IRQ_ISA_LINES)
        irq_register_handler(d->vdev.irq, vblk_irq, d);
    virtio_driver_ok(&d->vdev);

    vblk_devices[vblk_count++] = d;
    terminal_printf("virtio-blk%d: %u MiB, queue %u, irq %u\n", d->index, (uint32_t)(d->sectors / 2048),
                    d->vq.size, d->vdev.irq);
    vblk_mount_partitions(d);
}

void virtio_blk_initialize()
{
    virtio_pci_foreach(VIRTIO_BLK_LEGACY_ID, vblk_probe);
}
