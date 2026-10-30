#include "dma.h"

#include "alloc.h"
#include "mmu.h"
#include "pmm.h"
#include <os/errno.h>
#include <utils/sync/spinlock.h>

#define PTE_FRAME 0x000FFFFFFFFFF000ULL

static spinlock_t dma_lock = { .name = "dma" };

static bool power_of_two(const uint64_t x)
{
    return x && !(x & (x - 1));
}

static size_t page_count(const dma_size_t len)
{
    return (len + PAGE_SIZE - 1) / PAGE_SIZE;
}

static bool crosses_boundary(const dma_addr_t addr, const dma_size_t len, const dma_addr_t boundary)
{
    return boundary && ((addr ^ (addr + len - 1)) & ~(boundary - 1));
}

static bool translate(const uint64_t vaddr, dma_addr_t* out)
{
    auto table = (const uint64_t*)phys_to_virt(read_cr3() & PTE_FRAME);
    for (int shift = 39; shift >= 12; shift -= 9) {
        const uint64_t entry = table[(vaddr >> shift) & 0x1FF];
        if (!(entry & MMU_PRESENT))
            return false;
        if (shift == 12 || (shift != 39 && (entry & MMU_PDE_TWO_MB))) {
            const uint64_t offset = (1ULL << shift) - 1;
            *out = (entry & PTE_FRAME & ~offset) | (vaddr & offset);
            return true;
        }
        table = (const uint64_t*)phys_to_virt(entry & PTE_FRAME);
    }
    return false;
}

static void* alloc_pages(const size_t count, dma_size_t alignment, const dma_addr_t max_addr)
{
    if (alignment < PAGE_SIZE)
        alignment = PAGE_SIZE;

    const size_t total = count + alignment / PAGE_SIZE - 1;
    uint8_t* raw = alloc_physical_pages(total);
    if (!raw)
        return nullptr;

    const auto base = (uint8_t*)(((uintptr_t)raw + alignment - 1) & ~(uintptr_t)(alignment - 1));
    const size_t head = (base - raw) / PAGE_SIZE;
    const size_t tail = total - head - count;
    if (head)
        free_physical_pages(raw, head);
    if (tail)
        free_physical_pages(base + count * PAGE_SIZE, tail);

    if ((uint64_t)base + count * PAGE_SIZE - 1 > max_addr) {
        free_physical_pages(base, count);
        return nullptr;
    }
    return phys_to_virt((uint64_t)base);
}

static int bounce_reserve(struct dma_map* map, const dma_size_t len)
{
    if (map->bounce && map->bounce_len >= len)
        return 0;

    if (map->bounce)
        free_kernel_pages(map->bounce, page_count(map->bounce_len));
    map->bounce = nullptr;
    map->bounce_len = 0;

    const size_t pages = page_count(len);
    void* mem = alloc_pages(pages, map->tag->alignment, map->tag->max_addr);
    if (!mem)
        return -ENOMEM;

    map->bounce = mem;
    map->bounce_phys = virt_to_phys(mem);
    map->bounce_len = pages * PAGE_SIZE;
    return 0;
}

static int add_segment(const struct dma_map* map, int count, dma_addr_t addr, dma_size_t len)
{
    const struct dma_tag* tag = map->tag;
    const dma_addr_t window = tag->boundary ? ~(tag->boundary - 1) : 0;

    while (len > 0) {
        dma_size_t chunk = len;
        if (tag->boundary) {
            const dma_addr_t limit = (addr & window) + tag->boundary;
            if (chunk > limit - addr)
                chunk = limit - addr;
        }

        struct dma_segment* last = count ? &map->segments[count - 1] : nullptr;
        if (last && last->addr + last->len == addr && last->len + chunk <= tag->max_segment_size &&
            (last->addr & window) == (addr & window)) {
            last->len += chunk;
        } else {
            if (chunk > tag->max_segment_size)
                chunk = tag->max_segment_size;
            if (count == tag->max_segments)
                return -EFBIG;
            map->segments[count++] = (struct dma_segment){ .addr = addr, .len = chunk };
        }

        addr += chunk;
        len -= chunk;
    }
    return count;
}

static int load_buffer(const struct dma_map* map, uint64_t vaddr, dma_size_t len, bool* bounce)
{
    const struct dma_tag* tag = map->tag;
    int count = 0;

    while (len > 0) {
        dma_addr_t phys;
        if (!translate(vaddr, &phys))
            return -EFAULT;

        dma_size_t chunk = PAGE_SIZE - (vaddr & (PAGE_SIZE - 1));
        if (chunk > len)
            chunk = len;

        if (phys + chunk - 1 > tag->max_addr || (count == 0 && (phys & (tag->alignment - 1)))) {
            *bounce = true;
            return 0;
        }

        count = add_segment(map, count, phys, chunk);
        if (count < 0)
            return count;

        vaddr += chunk;
        len -= chunk;
    }
    return count;
}

void dma_initialize()
{
    spinlock_init(&dma_lock, "dma");
}

int dma_tag_create(struct dma_tag* parent, dma_size_t alignment, dma_addr_t boundary, dma_addr_t max_addr,
                   const dma_size_t max_size, const int max_segments, dma_size_t max_segment_size,
                   const uint32_t flags, struct dma_tag** out)
{
    if (!out || max_size == 0 || max_segments <= 0 || max_segment_size == 0)
        return -EINVAL;
    if (alignment == 0)
        alignment = 1;
    if (!power_of_two(alignment) || (boundary && !power_of_two(boundary)))
        return -EINVAL;

    if (parent) {
        if (parent->alignment > alignment)
            alignment = parent->alignment;
        if (parent->boundary && (!boundary || parent->boundary < boundary))
            boundary = parent->boundary;
        if (parent->max_addr < max_addr)
            max_addr = parent->max_addr;
    }

    if (boundary && max_segment_size > boundary)
        max_segment_size = boundary;

    struct dma_tag* tag = calloc(1, sizeof(struct dma_tag));
    if (!tag)
        return -ENOMEM;

    tag->parent = parent;
    tag->alignment = alignment;
    tag->boundary = boundary;
    tag->max_addr = max_addr;
    tag->max_size = max_size;
    tag->max_segments = max_segments;
    tag->max_segment_size = max_segment_size;
    tag->flags = flags;
    tag->ref_count = 1;

    if (parent) {
        uint64_t irq;
        spinlock_acquire_irqsave(&dma_lock, &irq);
        parent->ref_count++;
        spinlock_release_irqrestore(&dma_lock, irq);
    }

    *out = tag;
    return 0;
}

int dma_tag_destroy(struct dma_tag* tag)
{
    if (!tag)
        return -EINVAL;

    uint64_t irq;
    spinlock_acquire_irqsave(&dma_lock, &irq);
    if (tag->map_count) {
        spinlock_release_irqrestore(&dma_lock, irq);
        return -EBUSY;
    }

    while (tag && --tag->ref_count == 0) {
        struct dma_tag* parent = tag->parent;
        free(tag);
        tag = parent;
    }
    spinlock_release_irqrestore(&dma_lock, irq);
    return 0;
}

int dma_map_create(struct dma_tag* tag, const uint32_t flags, struct dma_map** out)
{
    if (!tag || !out)
        return -EINVAL;

    struct dma_map* map = calloc(1, sizeof(struct dma_map));
    if (!map)
        return -ENOMEM;

    map->tag = tag;
    map->segments = calloc(tag->max_segments, sizeof(struct dma_segment));
    if (!map->segments) {
        free(map);
        return -ENOMEM;
    }

    if ((flags & DMA_ALLOCNOW) && bounce_reserve(map, tag->max_size) < 0) {
        free(map->segments);
        free(map);
        return -ENOMEM;
    }

    uint64_t irq;
    spinlock_acquire_irqsave(&dma_lock, &irq);
    tag->map_count++;
    spinlock_release_irqrestore(&dma_lock, irq);

    *out = map;
    return 0;
}

int dma_map_destroy(struct dma_map* map)
{
    if (!map)
        return -EINVAL;
    if (map->loaded)
        return -EBUSY;

    if (map->bounce)
        free_kernel_pages(map->bounce, page_count(map->bounce_len));

    uint64_t irq;
    spinlock_acquire_irqsave(&dma_lock, &irq);
    map->tag->map_count--;
    spinlock_release_irqrestore(&dma_lock, irq);

    free(map->segments);
    free(map);
    return 0;
}

int dma_mem_alloc(struct dma_tag* tag, const uint32_t flags, void** vaddr, struct dma_map** out)
{
    if (!tag || !vaddr || !out)
        return -EINVAL;

    struct dma_map* map;
    const int err = dma_map_create(tag, 0, &map);
    if (err)
        return err;

    const size_t pages = page_count(tag->max_size);
    void* mem = alloc_pages(pages, tag->alignment, tag->max_addr);
    if (mem && tag->max_size <= tag->boundary && crosses_boundary(virt_to_phys(mem), tag->max_size, tag->boundary)) {
        free_kernel_pages(mem, pages);
        mem = alloc_pages(pages, tag->boundary, tag->max_addr);
    }
    if (!mem) {
        dma_map_destroy(map);
        return -ENOMEM;
    }

    if (flags & DMA_ZERO)
        memset(mem, 0, pages * PAGE_SIZE);

    *vaddr = mem;
    *out = map;
    return 0;
}

void dma_mem_free(void* vaddr, struct dma_map* map)
{
    if (!vaddr || !map)
        return;

    dma_map_unload(map);
    free_kernel_pages(vaddr, page_count(map->tag->max_size));
    dma_map_destroy(map);
}

int dma_map_load(struct dma_map* map, void* buffer, const dma_size_t len, const uint32_t flags)
{
    (void)flags;
    if (!map || !buffer || len == 0 || len > map->tag->max_size)
        return -EINVAL;
    if (map->loaded)
        return -EBUSY;

    bool bounce = false;
    int count = load_buffer(map, (uint64_t)buffer, len, &bounce);
    if (count < 0)
        return count;

    if (bounce) {
        const int err = bounce_reserve(map, len);
        if (err)
            return err;
        count = add_segment(map, 0, map->bounce_phys, len);
        if (count < 0)
            return count;
    }

    map->num_segments = count;
    map->buffer = buffer;
    map->buffer_len = len;
    map->bounced = bounce;
    map->loaded = true;
    return 0;
}

void dma_map_unload(struct dma_map* map)
{
    if (!map)
        return;

    map->num_segments = 0;
    map->buffer = nullptr;
    map->buffer_len = 0;
    map->bounced = false;
    map->loaded = false;
}

void dma_map_sync(struct dma_map* map, const uint32_t op)
{
    if (!map || !map->loaded)
        return;

    if (map->bounced && (op & DMA_SYNC_PREWRITE))
        memcpy(map->bounce, map->buffer, map->buffer_len);

    asm volatile("mfence" : : : "memory");

    if (map->bounced && (op & DMA_SYNC_POSTREAD))
        memcpy(map->buffer, map->bounce, map->buffer_len);
}
