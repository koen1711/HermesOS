#include "mmu.h"

#include <os/stddef.h>

#include "pmm.h"

static mmu_context_t kernel_mmu;

// Allocate new page tables (you'll need a physical memory allocator)
static uint64_t* allocate_page_table(void) {
    uint64_t* table = alloc_physical_page();
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    return table;
}

// Helpers to walk or create page table levels
static uint64_t* get_or_create_pdpt(const uint16_t pml4_idx) {
    const uint64_t entry = kernel_mmu.pml4[pml4_idx];
    if (entry & MMU_PRESENT) {
        return (uint64_t*)(entry & ~0xFFFULL);
    }
    uint64_t* pdpt = allocate_page_table();
    kernel_mmu.pml4[pml4_idx] = (uint64_t)pdpt | MMU_PRESENT | MMU_WRITABLE;
    return pdpt;
}

static uint64_t* get_or_create_pd(uint64_t* pdpt, const uint16_t pdpt_idx) {
    const uint64_t entry = pdpt[pdpt_idx];
    if (entry & MMU_PRESENT) {
        return (uint64_t*)(entry & ~0xFFFULL);
    }
    uint64_t* pd = allocate_page_table();
    pdpt[pdpt_idx] = (uint64_t)pd | MMU_PRESENT | MMU_WRITABLE;
    return pd;
}

static uint64_t* get_or_create_pt(uint64_t* pd, const uint16_t pd_idx) {
    const uint64_t entry = pd[pd_idx];
    if (entry & MMU_PRESENT) {
        return (uint64_t*)(entry & ~0xFFFULL);
    }
    uint64_t* pt = allocate_page_table();
    pd[pd_idx] = (uint64_t)pt | MMU_PRESENT | MMU_WRITABLE;
    return pt;
}

int map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint16_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint16_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint16_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint16_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    uint64_t* pdpt = get_or_create_pdpt(pml4_idx);
    if (!pdpt) return -1;

    uint64_t* pd = get_or_create_pd(pdpt, pdpt_idx);
    if (!pd) return -1;

    uint64_t* pt = get_or_create_pt(pd, pd_idx);
    if (!pt) return -1;

    pt[pt_idx] = physical_addr | flags;

    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 0;
}

void unmap_page(uint64_t virtual_addr) {
    uint16_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint16_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint16_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint16_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    uint64_t* pdpt = get_or_create_pdpt(pml4_idx);
    if (!pdpt) return;

    uint64_t* pd = get_or_create_pd(pdpt, pdpt_idx);
    if (!pd) return;

    uint64_t* pt = get_or_create_pt(pd, pd_idx);
    if (!pt) return;

    pt[pt_idx] = 0;

    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

void setup_kernel_mapping(const mmu_context_t* mmu) {
    const uint64_t size = 0x40000000ULL;

    uint64_t* high_pdpt = allocate_page_table();
    mmu->pml4[511] = (uint64_t)high_pdpt | MMU_PRESENT | MMU_WRITABLE;

    uint64_t* kernel_pd = allocate_page_table();
    high_pdpt[510] = (uint64_t)kernel_pd | MMU_PRESENT | MMU_WRITABLE;

    for (uint64_t i = 0; i < (size / TWO_MEGABYTES); i++) {
        const uint64_t phys_start = 0x400000ULL;
        const uint64_t phys_addr = phys_start + (i * TWO_MEGABYTES);
        kernel_pd[i] = phys_addr | MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB;
    }
}

void setup_identity_mapping(const mmu_context_t* mmu) {
    const uint64_t size = 0x40000000ULL;

    uint64_t* pdpt = allocate_page_table();
    mmu->pml4[0] = (uint64_t)pdpt | MMU_PRESENT | MMU_WRITABLE;

    uint64_t* pd = allocate_page_table();
    pdpt[0] = (uint64_t)pd | MMU_PRESENT | MMU_WRITABLE;

    for (uint64_t i = 0; i < (size / TWO_MEGABYTES); i++) {
        const uint64_t phys_start = 0x00000000ULL;
        const uint64_t phys_addr = phys_start + (i * TWO_MEGABYTES);
        pd[i] = phys_addr | MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB;
    }
}

void setup_device_mappings(mmu_context_t* mmu) {
    const uint64_t virtual_addr = 0xFFFFFFFFFEC00000ULL;
    const uint64_t phys = 0xFEC00000ULL;
    map_page(virtual_addr, phys, MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE);
}

void mmu_initialize(void) {
    kernel_mmu.pml4 = allocate_page_table();

    setup_kernel_mapping(&kernel_mmu);
    setup_identity_mapping(&kernel_mmu);
    setup_device_mappings(&kernel_mmu);

    asm volatile("mov %0, %%cr3" : : "r"(kernel_mmu.pml4) : "memory");
}

void* vmalloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 0xFFF) & ~0xFFF; // Align to page size
    void* addr = alloc_physical_page();

    if (!addr) return NULL;

    for (size_t i = 0; i < size; i += 0x1000) {
        if (map_page((uintptr_t)addr + i, (uintptr_t)addr + i, MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE) < 0) {
            vfree(addr);
            return NULL;
        }
    }
    return addr;
}

void vfree(void* ptr) {
    if (!ptr) return;

    uintptr_t addr = (uintptr_t)ptr;
    for (size_t i = 0; i < 0x1000; i += 0x1000) {
        unmap_page(addr + i);
    }
    free_physical_page(ptr);
}
