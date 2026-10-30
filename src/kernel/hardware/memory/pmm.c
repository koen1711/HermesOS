#include "pmm.h"
#include "alloc.h"
#include "mmu.h"
#include <os/stdint.h>
#include <os/stddef.h>
#include <multiboot.h>

#include <drivers/fs/pagecache/pagecache.h>
#include <hardware/interrupts/panic.h>
#include <utils/sync/spinlock.h>

extern void* _kernel_virtual_end;

#define BOOT_WORDS (IDENTITY_MAP_SIZE / PAGE_SIZE / 64)
#define LOW_WORDS (0x100000000ULL / PAGE_SIZE / 64)
#define RECLAIM_BATCH 256

static uint64_t* physical_pages; // Bitmap
static uint16_t* shared_refs;
static size_t total_pages = 0;
static size_t total_words = 0;
static size_t low_words = 0;
static size_t boot_words = 0;
static size_t usable_pages = 0;
static spinlock_t pmm_lock = { .name = "pmm" };
static size_t low_hint = 0;
static size_t high_hint = 0;

static memory_region_t usable[MAX_MEMORY_REGIONS];
static size_t usable_count = 0;

#define BITMAP_INDEX(page) ((page) / 64)
#define BITMAP_OFFSET(page) ((page) % 64)

#define SET_BIT(bitmap, page) (bitmap[BITMAP_INDEX(page)] |= (1ULL << BITMAP_OFFSET(page)))
#define CLEAR_BIT(bitmap, page) (bitmap[BITMAP_INDEX(page)] &= ~(1ULL << BITMAP_OFFSET(page)))
#define TEST_BIT(bitmap, page) (bitmap[BITMAP_INDEX(page)] & (1ULL << BITMAP_OFFSET(page)))

static uintptr_t place_metadata(const uintptr_t kernel_end, const size_t size) {
    for (size_t i = 0; i < usable_count; i++) {
        uintptr_t start = PAGE_ALIGN_UP(usable[i].addr);
        uintptr_t end = usable[i].addr + usable[i].length;
        if (start < kernel_end) start = kernel_end;
        if (end > IDENTITY_MAP_SIZE) end = IDENTITY_MAP_SIZE;
        if (start + size <= end) return start;
    }
    return 0;
}

void pmm_initialize(const multiboot_info_t mb_info) {
    const uintptr_t kernel_end = PAGE_ALIGN_UP((uintptr_t)&_kernel_virtual_end + 2048);

    uint64_t limit = 0;
    for (size_t i = 0; i < mb_info.region_count && i < MAX_MEMORY_REGIONS; i++) {
        const memory_region_t* mmap = &mb_info.regions[i];
        if (mmap->type != MEMORY_AVAILABLE) continue;
        usable[usable_count++] = *mmap;
        const uint64_t end = (mmap->addr + mmap->length) & PAGE_MASK;
        if (end > limit) limit = end;
    }

    total_pages = limit / PAGE_SIZE;
    total_words = (total_pages + 63) / 64;
    low_words = total_words < LOW_WORDS ? total_words : LOW_WORDS;
    boot_words = total_words < BOOT_WORDS ? total_words : BOOT_WORDS;
    high_hint = low_words;

    const size_t meta_size = PAGE_ALIGN_UP(total_words * sizeof(uint64_t) + total_pages * sizeof(uint16_t));
    const uintptr_t meta = place_metadata(kernel_end, meta_size);
    if (!meta) panic("No room for the physical page bitmap", __FILE__, __LINE__);
    physical_pages = (uint64_t*)meta;
    shared_refs = (uint16_t*)(meta + total_words * sizeof(uint64_t));
    memset(physical_pages, 0xFF, total_words * sizeof(uint64_t));
    memset(shared_refs, 0, total_pages * sizeof(uint16_t));

    for (size_t i = 0; i < usable_count; i++) {
        const uintptr_t start = PAGE_ALIGN_UP(usable[i].addr);
        const uintptr_t end = usable[i].addr + usable[i].length;

        for (uintptr_t addr = start; addr + PAGE_SIZE <= end; addr += PAGE_SIZE) {
            if (addr < kernel_end || (addr >= meta && addr < meta + meta_size)) continue;

            const size_t page = addr / PAGE_SIZE;
            if (!TEST_BIT(physical_pages, page)) continue;
            CLEAR_BIT(physical_pages, page);
            usable_pages++;
        }
    }
}

static void* take_page_locked(const size_t last_word, size_t* hint) {
    for (size_t word = *hint; word < last_word; word++) {
        if (physical_pages[word] == ~0ULL) continue;
        const size_t page = word * 64 + __builtin_ctzll(~physical_pages[word]);
        SET_BIT(physical_pages, page);
        *hint = word;
        return (void*)(page * PAGE_SIZE);
    }
    *hint = last_word;
    return nullptr;
}

static bool direct_map_ready(void) {
    return direct_map_offset != 0;
}

static void* take_page(void) {
    uint64_t flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);
    void* page = nullptr;
    if (direct_map_ready()) {
        page = take_page_locked(total_words, &high_hint);
        if (!page) page = take_page_locked(low_words, &low_hint);
    } else {
        page = take_page_locked(boot_words, &low_hint);
    }
    spinlock_release_irqrestore(&pmm_lock, flags);
    return page;
}

void* alloc_physical_page() {
    void* page = take_page();
    if (!page && pcache_reclaim(RECLAIM_BATCH)) page = take_page();
    return page;
}

static void free_page_locked(void* page) {
    const size_t addr = (uintptr_t)page / PAGE_SIZE;
    if (addr && addr < total_pages) {
        if (shared_refs[addr]) {
            shared_refs[addr]--;
            return;
        }
        CLEAR_BIT(physical_pages, addr);
        size_t* hint = BITMAP_INDEX(addr) < low_words ? &low_hint : &high_hint;
        if (BITMAP_INDEX(addr) < *hint) *hint = BITMAP_INDEX(addr);
    }
}

void free_physical_page(void* page) {
    free_physical_pages(page, 1);
}

void pmm_share_page(void* page) {
    const size_t addr = (uintptr_t)page / PAGE_SIZE;
    uint64_t flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);
    if (addr < total_pages && shared_refs[addr] < 0xFFFF) {
        shared_refs[addr]++;
    }
    spinlock_release_irqrestore(&pmm_lock, flags);
}

bool pmm_page_shared(void* page) {
    const size_t addr = (uintptr_t)page / PAGE_SIZE;
    uint64_t flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);
    const bool shared = addr < total_pages && shared_refs[addr];
    spinlock_release_irqrestore(&pmm_lock, flags);
    return shared;
}

static void* take_pages(const size_t count) {
    uint64_t flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);
    const size_t last = (direct_map_ready() ? total_words : boot_words) * 64;
    size_t run = 0;
    for (size_t page = low_hint * 64; page < last; page++) {
        run = TEST_BIT(physical_pages, page) ? 0 : run + 1;
        if (run == count) {
            const size_t first = page + 1 - count;
            for (size_t i = first; i <= page; i++) {
                SET_BIT(physical_pages, i);
            }
            spinlock_release_irqrestore(&pmm_lock, flags);
            return (void*)(first * PAGE_SIZE);
        }
    }
    spinlock_release_irqrestore(&pmm_lock, flags);
    return nullptr;
}

void* alloc_physical_pages(const size_t count) {
    if (count == 0) return nullptr;
    void* first = take_pages(count);
    if (!first && pcache_reclaim(RECLAIM_BATCH + count)) first = take_pages(count);
    return first;
}

void free_physical_pages(void* first, const size_t count) {
    uint64_t flags;
    spinlock_acquire_irqsave(&pmm_lock, &flags);
    for (size_t i = 0; i < count; i++) {
        free_page_locked((uint8_t*)first + i * PAGE_SIZE);
    }
    spinlock_release_irqrestore(&pmm_lock, flags);
}

void* alloc_kernel_page(void) {
    void* page = alloc_physical_page();
    return page ? phys_to_virt((uint64_t)page) : nullptr;
}

void* alloc_kernel_pages(const size_t count) {
    void* first = alloc_physical_pages(count);
    return first ? phys_to_virt((uint64_t)first) : nullptr;
}

void free_kernel_page(void* page) {
    free_physical_page((void*)virt_to_phys(page));
}

void free_kernel_pages(void* first, const size_t count) {
    free_physical_pages((void*)virt_to_phys(first), count);
}

size_t pmm_usable_regions(const memory_region_t** regions) {
    *regions = usable;
    return usable_count;
}

size_t pmm_total_pages() {
    return usable_pages;
}

size_t pmm_free_pages() {
    size_t free = 0;
    for (size_t word = 0; word < total_words; word++) {
        for (uint64_t bits = ~physical_pages[word]; bits; bits &= bits - 1) {
            free++;
        }
    }
    return free;
}
