#include "mmu.h"

#include <os/stddef.h>

#include "pmm.h"
#include "alloc.h"
#include <hardware/interrupts/panic.h>
#include <hardware/smp/smp.h>
#include <utils/sync/spinlock.h>

#define PTE_ADDR 0x000FFFFFFFFFF000ULL

uint64_t direct_map_offset = 0;
static uint64_t kernel_pml4;

static uint64_t* table_at(const uint64_t entry) {
    return phys_to_virt(entry & PTE_ADDR);
}

static uint64_t allocate_page_table(void) {
    void* table = alloc_physical_page();
    if (!table) return 0;
    memset(phys_to_virt((uint64_t)table), 0, PAGE_SIZE);
    return (uint64_t)table;
}

static uint64_t* get_or_create_table(uint64_t* parent, const uint16_t idx, const uint64_t flags) {
    if (parent[idx] & MMU_PRESENT) {
        return table_at(parent[idx]);
    }
    const uint64_t table = allocate_page_table();
    if (!table) return nullptr;
    parent[idx] = table | MMU_PRESENT | MMU_WRITABLE | (flags & MMU_USER);
    return table_at(table);
}

static uint64_t* split_two_mb_page(uint64_t* pd, const uint16_t pd_idx) {
    const uint64_t entry = pd[pd_idx];
    const uint64_t table = allocate_page_table();
    if (!table) return nullptr;

    uint64_t* pt = table_at(table);
    const uint64_t base = entry & 0x000FFFFFFFE00000ULL;
    const uint64_t flags = (entry & 0xFFFULL & ~MMU_PDE_TWO_MB) | (entry & (1ULL << 63));
    for (int i = 0; i < 512; i++) {
        pt[i] = (base + i * PAGE_SIZE) | flags;
    }
    pd[pd_idx] = table | MMU_PRESENT | MMU_WRITABLE | (entry & MMU_USER);
    return pt;
}

static uint64_t* get_or_create_pt(uint64_t* pd, const uint16_t pd_idx, const uint64_t flags) {
    const uint64_t entry = pd[pd_idx];
    if ((entry & MMU_PRESENT) && (entry & MMU_PDE_TWO_MB)) {
        return split_two_mb_page(pd, pd_idx);
    }
    return get_or_create_table(pd, pd_idx, flags);
}

int map_page_in(uint64_t pml4_addr, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint16_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint16_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint16_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint16_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    uint64_t* pdpt = get_or_create_table(phys_to_virt(pml4_addr), pml4_idx, flags);
    if (!pdpt) return -1;

    uint64_t* pd = get_or_create_table(pdpt, pdpt_idx, flags);
    if (!pd) return -1;

    uint64_t* pt = get_or_create_pt(pd, pd_idx, flags);
    if (!pt) return -1;

    pt[pt_idx] = physical_addr | flags;

    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 0;
}

int map_page(const uint64_t virtual_addr, const uint64_t physical_addr, const uint64_t flags) {
    return map_page_in(kernel_pml4, virtual_addr, physical_addr, flags);
}

void unmap_page(uint64_t virtual_addr) {
    uint64_t* pte = mmu_find_pte(kernel_pml4, virtual_addr);
    if (!pte) return;

    *pte = 0;

    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

static void setup_kernel_mapping(uint64_t* pml4) {
    constexpr uint64_t size = 0x40000000ULL;

    const uint64_t high_pdpt = allocate_page_table();
    pml4[511] = high_pdpt | MMU_PRESENT | MMU_WRITABLE;

    const uint64_t kernel_pd = allocate_page_table();
    table_at(high_pdpt)[510] = kernel_pd | MMU_PRESENT | MMU_WRITABLE;

    uint64_t* pd = table_at(kernel_pd);
    for (uint64_t i = 0; i < (size / TWO_MEGABYTES); i++) {
        constexpr uint64_t phys_start = 0x400000ULL;
        const uint64_t phys_addr = phys_start + (i * TWO_MEGABYTES);
        pd[i] = phys_addr | MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB;
    }
}

static void setup_identity_mapping(uint64_t* pml4) {
    constexpr uint64_t size = IDENTITY_MAP_SIZE;

    const uint64_t pdpt = allocate_page_table();
    pml4[0] = pdpt | MMU_PRESENT | MMU_WRITABLE;

    const uint64_t identity_pd = allocate_page_table();
    table_at(pdpt)[0] = identity_pd | MMU_PRESENT | MMU_WRITABLE;

    uint64_t* pd = table_at(identity_pd);
    for (uint64_t i = 0; i < (size / TWO_MEGABYTES); i++) {
        constexpr uint64_t phys_start = 0x00000000ULL;
        const uint64_t phys_addr = phys_start + (i * TWO_MEGABYTES);
        pd[i] = phys_addr | MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB;
    }
}

static void setup_device_mappings(void) {
    constexpr uint64_t virtual_addr = 0xFFFFFFFFFEC00000ULL;
    constexpr uint64_t phys = 0xFEC00000ULL;
    map_page(virtual_addr, phys, MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE);
}

static void setup_direct_mapping(uint64_t* pml4) {
    const memory_region_t* regions;
    const size_t count = pmm_usable_regions(&regions);

    for (size_t i = 0; i < count; i++) {
        const uint64_t end = regions[i].addr + regions[i].length;
        for (uint64_t phys = regions[i].addr & ~(TWO_MEGABYTES - 1); phys < end; phys += TWO_MEGABYTES) {
            const uint64_t virt = DIRECT_MAP_BASE + phys;
            uint64_t* pdpt = get_or_create_table(pml4, (virt >> 39) & 0x1FF, 0);
            uint64_t* pd = pdpt ? get_or_create_table(pdpt, (virt >> 30) & 0x1FF, 0) : nullptr;
            if (!pd) panic("Failed to build the direct map", __FILE__, __LINE__);
            pd[(virt >> 21) & 0x1FF] = phys | MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB;
        }
    }
}

void mmu_initialize(void) {
    kernel_pml4 = allocate_page_table();
    uint64_t* pml4 = table_at(kernel_pml4);

    setup_kernel_mapping(pml4);
    setup_identity_mapping(pml4);
    setup_direct_mapping(pml4);
    setup_device_mappings();

    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4) : "memory");
    direct_map_offset = DIRECT_MAP_BASE;
}

void* vmalloc(size_t size) {
    if (size == 0) return nullptr;
    size = (size + 0xFFF) & ~0xFFF;
    return alloc_kernel_pages(size / 0x1000);
}

void vfree(void* ptr, const size_t size) {
    if (!ptr || size == 0) return;
    free_kernel_pages(ptr, PAGE_ALIGN_UP(size) / PAGE_SIZE);
}

void free_vm(const uint64_t pml4_addr) {
    if (!pml4_addr) return;
    const uint64_t* pml4 = phys_to_virt(pml4_addr);
    const uint64_t* kernel = phys_to_virt(kernel_pml4);

    const uint64_t* kernel_low = table_at(kernel[0]);
    for (int i = 0; i < 512; i++) {
        if (!(pml4[i] & MMU_PRESENT) || pml4[i] == kernel[i]) continue;
        const uint64_t* pdpt = table_at(pml4[i]);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & MMU_PRESENT) || (i == 0 && pdpt[j] == kernel_low[j])) continue;
            const uint64_t* pd = table_at(pdpt[j]);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & MMU_PRESENT) || (pd[k] & MMU_PDE_TWO_MB)) continue;
                const uint64_t* pt = table_at(pd[k]);

                for (int l = 0; l < 512; l++) {
                    if ((pt[l] & MMU_USER) && (pt[l] & PTE_ADDR)) {
                        free_physical_page((void*)(pt[l] & PTE_ADDR));
                    }
                }
                free_physical_page((void*)(pd[k] & PTE_ADDR));
            }
            free_physical_page((void*)(pdpt[j] & PTE_ADDR));
        }
        free_physical_page((void*)(pml4[i] & PTE_ADDR));
    }
    free_physical_page((void*)pml4_addr);
}

uint64_t* mmu_find_pte(const uint64_t pml4_addr, const uint64_t virtual_addr) {
    uint64_t* table = phys_to_virt(pml4_addr);
    for (int shift = 39; shift > 12; shift -= 9) {
        const uint64_t entry = table[(virtual_addr >> shift) & 0x1FF];
        if (!(entry & MMU_PRESENT) || (entry & MMU_PDE_TWO_MB)) return nullptr;
        table = table_at(entry);
    }
    return &table[(virtual_addr >> 12) & 0x1FF];
}


#define MMIO_VIRTUAL_BASE  0xFFFFFFFFC0000000ULL
#define MMIO_VIRTUAL_LIMIT 0xFFFFFFFFFEC00000ULL
#define MMIO_PAGES         ((MMIO_VIRTUAL_LIMIT - MMIO_VIRTUAL_BASE) / PAGE_SIZE)

static uint64_t mmio_bitmap[MMIO_PAGES / 64];
static spinlock_t mmio_lock = { .name = "mmio" };

static void mmio_mark(const size_t first, const size_t count, const bool used) {
    for (size_t page = first; page < first + count; page++) {
        if (used) mmio_bitmap[page / 64] |= 1ULL << (page % 64);
        else mmio_bitmap[page / 64] &= ~(1ULL << (page % 64));
    }
}

uint64_t vmm_alloc_mmio_region(size_t size) {
    const size_t count = ((size + 0xFFF) & ~0xFFFULL) / PAGE_SIZE;
    if (count == 0 || count > MMIO_PAGES) return 0;

    uint64_t flags;
    spinlock_acquire_irqsave(&mmio_lock, &flags);
    size_t run = 0;
    for (size_t page = 0; page < MMIO_PAGES; page++) {
        run = mmio_bitmap[page / 64] & (1ULL << (page % 64)) ? 0 : run + 1;
        if (run == count) {
            const size_t first = page + 1 - count;
            mmio_mark(first, count, true);
            spinlock_release_irqrestore(&mmio_lock, flags);
            return MMIO_VIRTUAL_BASE + first * PAGE_SIZE;
        }
    }
    spinlock_release_irqrestore(&mmio_lock, flags);
    return 0;
}

void vmm_free_mmio_region(const uint64_t virt, size_t size) {
    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0 || virt < MMIO_VIRTUAL_BASE || virt + size > MMIO_VIRTUAL_LIMIT || (virt & 0xFFF)) return;

    for (uint64_t off = 0; off < size; off += PAGE_SIZE)
        unmap_page(virt + off);
    smp_tlb_shootdown();

    uint64_t flags;
    spinlock_acquire_irqsave(&mmio_lock, &flags);
    mmio_mark((virt - MMIO_VIRTUAL_BASE) / PAGE_SIZE, size / PAGE_SIZE, false);
    spinlock_release_irqrestore(&mmio_lock, flags);
}

uint64_t mmu_kernel_page_map(void) {
    return kernel_pml4;
}

uint64_t setup_kvm(void) {
    const uint64_t pml4_addr = allocate_page_table();
    if (!pml4_addr) return 0;

    uint64_t* pml4 = table_at(pml4_addr);
    const uint64_t* kernel = table_at(kernel_pml4);
    for (int i = 0; i < 512; i++) {
        pml4[i] = kernel[i];
    }

    const uint64_t* kernel_low = table_at(kernel[0]);
    const uint64_t low_addr = allocate_page_table();
    if (!low_addr) {
        free_physical_page((void*)pml4_addr);
        return 0;
    }
    uint64_t* low = table_at(low_addr);
    for (int i = 0; i < 512; i++) {
        low[i] = kernel_low[i];
    }
    pml4[0] = low_addr | MMU_PRESENT | MMU_WRITABLE | MMU_USER;
    return pml4_addr;
}

void switch_vm(uint64_t pml4_addr) {
    asm volatile("mov %0, %%cr3" : : "r"(pml4_addr) : "memory");
}

uint64_t read_cr3(void) {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value & ~0xFFFULL;
}
