#include "pmm.h"
#include <os/stdint.h>
#include <os/stddef.h>
#include <multiboot.h>

#include "drivers/terminal/terminal.h"

extern void* _kernel_virtual_end;

#define MAX_PHYSICAL_PAGES 65536 // Support up to 256MB (65536 * 4K)

static uint64_t physical_pages[MAX_PHYSICAL_PAGES / 64]; // Bitmap
static uintptr_t physical_base = 0;
static size_t total_pages = 0;

#define BITMAP_INDEX(page) ((page) / 64)
#define BITMAP_OFFSET(page) ((page) % 64)

#define SET_BIT(bitmap, page) (bitmap[BITMAP_INDEX(page)] |= (1ULL << BITMAP_OFFSET(page)))
#define CLEAR_BIT(bitmap, page) (bitmap[BITMAP_INDEX(page)] &= ~(1ULL << BITMAP_OFFSET(page)))
#define TEST_BIT(bitmap, page) (bitmap[BITMAP_INDEX(page)] & (1ULL << BITMAP_OFFSET(page)))

void pmm_initialize(const multiboot_info_t mb_info) {
    for (size_t i = 0; i < MAX_PHYSICAL_PAGES / 64; i++) {
        physical_pages[i] = ~0ULL; // Mark all as used
    }

    const uintptr_t kernel_end = (uintptr_t)&_kernel_virtual_end + 2048; // Assuming kernel occupies 2048 bytes after _kernel_virtual_end

    for (size_t i = 0; i < mb_info.region_count && i < MAX_MEMORY_REGIONS; i++) {
        const memory_region_t* mmap = &mb_info.regions[i];
        if (mmap->type != 1) continue; // Skip non-available regions

        const uintptr_t start = mmap->addr;
        uintptr_t end = start + mmap->length;

        if (end > 0x100000000ULL) end = 0x100000000ULL; // Cap at 4GB for now

        for (uintptr_t addr = start; addr + 0x1000 <= end; addr += 0x1000) {
            if (addr < kernel_end) continue; // Skip kernel occupied

            const size_t page = (addr - physical_base) / 0x1000;
            if (page >= MAX_PHYSICAL_PAGES) continue;
            CLEAR_BIT(physical_pages, page);
        }
    }

    total_pages = MAX_PHYSICAL_PAGES;
}

void* alloc_physical_page() {
    for (size_t page = 0; page < total_pages; page++) {
        if (!TEST_BIT(physical_pages, page)) {
            SET_BIT(physical_pages, page);
            return (void*)(physical_base + (page * 0x1000));
        }
    }
    return NULL; // Out of memory
}

void free_physical_page(void* page) {
    const uintptr_t address = (uintptr_t)page;
    if (address < physical_base) return;

    const size_t addr = (address - physical_base) / 0x1000;
    if (addr < total_pages) {
        CLEAR_BIT(physical_pages, addr);
    }
}
