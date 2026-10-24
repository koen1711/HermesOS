#include "mmu.h"

#include <os/stddef.h>

#include "pmm.h"
#include "alloc.h"

static mmu_context_t kernel_mmu;

static uint64_t* allocate_page_table(void) {
    uint64_t* table = alloc_physical_page();
    if (!table) return nullptr;
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    return table;
}

static uint64_t* get_or_create_pdpt(uint64_t* pml4, const uint16_t pml4_idx, const uint64_t flags) {
    const uint64_t entry = pml4[pml4_idx];
    if (entry & MMU_PRESENT) {
        return (uint64_t*)(entry & ~0xFFFULL);
    }
    uint64_t* pdpt = allocate_page_table();
    if (!pdpt) return nullptr;
    pml4[pml4_idx] = (uint64_t)pdpt | MMU_PRESENT | MMU_WRITABLE | (flags & MMU_USER);
    return pdpt;
}

static uint64_t* get_or_create_pd(uint64_t* pdpt, const uint16_t pdpt_idx, const uint64_t flags) {
    const uint64_t entry = pdpt[pdpt_idx];
    if (entry & MMU_PRESENT) {
        return (uint64_t*)(entry & ~0xFFFULL);
    }
    uint64_t* pd = allocate_page_table();
    if (!pd) return nullptr;
    pdpt[pdpt_idx] = (uint64_t)pd | MMU_PRESENT | MMU_WRITABLE | (flags & MMU_USER);
    return pd;
}

static uint64_t* get_or_create_pt(uint64_t* pd, const uint16_t pd_idx, const uint64_t flags) {
    const uint64_t entry = pd[pd_idx];
    if (entry & MMU_PRESENT) {
        return (uint64_t*)(entry & ~0xFFFULL);
    }
    uint64_t* pt = allocate_page_table();
    if (!pt) return nullptr;
    pd[pd_idx] = (uint64_t)pt | MMU_PRESENT | MMU_WRITABLE | (flags & MMU_USER);
    return pt;
}

int map_page_in(uint64_t pml4_addr, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint16_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint16_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint16_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint16_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    uint64_t* pdpt = get_or_create_pdpt((uint64_t*)pml4_addr, pml4_idx, flags);
    if (!pdpt) return -1;

    uint64_t* pd = get_or_create_pd(pdpt, pdpt_idx, flags);
    if (!pd) return -1;

    uint64_t* pt = get_or_create_pt(pd, pd_idx, flags);
    if (!pt) return -1;

    pt[pt_idx] = physical_addr | flags;

    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 0;
}

int map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    return map_page_in((uint64_t)kernel_mmu.pml4, virtual_addr, physical_addr, flags);
}

void unmap_page(uint64_t virtual_addr) {
    uint16_t pml4_idx = (virtual_addr >> 39) & 0x1FF;
    uint16_t pdpt_idx = (virtual_addr >> 30) & 0x1FF;
    uint16_t pd_idx   = (virtual_addr >> 21) & 0x1FF;
    uint16_t pt_idx   = (virtual_addr >> 12) & 0x1FF;

    uint64_t* pdpt = get_or_create_pdpt(kernel_mmu.pml4, pml4_idx, 0);
    if (!pdpt) return;

    uint64_t* pd = get_or_create_pd(pdpt, pdpt_idx, 0);
    if (!pd) return;

    uint64_t* pt = get_or_create_pt(pd, pd_idx, 0);
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
    size = (size + 0xFFF) & ~0xFFF;
    void* addr = alloc_physical_pages(size / 0x1000);

    if (!addr) return NULL;

    for (size_t i = 0; i < size; i += 0x1000) {
        if (map_page((uintptr_t)addr + i, (uintptr_t)addr + i, MMU_PRESENT | MMU_WRITABLE | MMU_CACHE_DISABLE) < 0) {
            for (size_t j = 0; j < i; j += 0x1000) {
                unmap_page((uintptr_t)addr + j);
            }
            free_physical_pages(addr, size / 0x1000);
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

void free_vm(uint64_t pml4_addr) {
    uint64_t* pml4 = (uint64_t*)pml4_addr;
    if (!pml4) return;

    for (int i = 0; i < 512; i++) {
        if (!(pml4[i] & MMU_PRESENT) || pml4[i] == kernel_mmu.pml4[i]) continue;
        uint64_t* pdpt = (uint64_t*)(pml4[i] & ~0xFFFULL);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & MMU_PRESENT)) continue;
            uint64_t* pd = (uint64_t*)(pdpt[j] & ~0xFFFULL);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & MMU_PRESENT) || (pd[k] & MMU_PDE_TWO_MB)) continue;
                uint64_t* pt = (uint64_t*)(pd[k] & ~0xFFFULL);

                for (int l = 0; l < 512; l++) {
                    if ((pt[l] & MMU_PRESENT) && (pt[l] & MMU_USER)) {
                        free_physical_page((void*)(pt[l] & ~0xFFFULL));
                    }
                }
                free_physical_page(pt);
            }
            free_physical_page(pd);
        }
        free_physical_page(pdpt);
    }
    free_physical_page(pml4);
}

static uint64_t* find_pte(uint64_t pml4_addr, uint64_t virtual_addr) {
    uint64_t* table = (uint64_t*)pml4_addr;
    for (int shift = 39; shift > 12; shift -= 9) {
        const uint64_t entry = table[(virtual_addr >> shift) & 0x1FF];
        if (!(entry & MMU_PRESENT) || (entry & MMU_PDE_TWO_MB)) return nullptr;
        table = (uint64_t*)(entry & ~0xFFFULL);
    }
    return &table[(virtual_addr >> 12) & 0x1FF];
}


uint64_t setup_kvm(void) {
    uint64_t* pml4 = allocate_page_table();
    if (!pml4) return 0;

    for (int i = 0; i < 512; i++) {
        pml4[i] = kernel_mmu.pml4[i];
    }
    return (uint64_t)pml4;
}

bool setup_uvm(uint64_t pml4_addr, uint64_t start_location, size_t size) {
    if (size > USER_MEMORY_SIZE) return false;

    for (uint64_t offset = 0; offset < USER_MEMORY_SIZE; offset += PAGE_SIZE) {
        uint8_t* page = alloc_physical_page();
        if (!page) return false;

        memset(page, 0, PAGE_SIZE);
        if (offset < size) {
            const size_t n = (size - offset < PAGE_SIZE) ? size - offset : PAGE_SIZE;
            memcpy(page, (const void*)(start_location + offset), n);
        }

        if (map_page_in(pml4_addr, USER_VIRTUAL_BASE + offset, (uint64_t)page,
                        MMU_PRESENT | MMU_WRITABLE | MMU_USER) < 0) {
            free_physical_page(page);
            return false;
        }
    }
    return true;
}

bool copy_uvm(uint64_t new_pml4, uint64_t current_pml4) {
    for (uint64_t va = USER_VIRTUAL_BASE; va < USER_VIRTUAL_BASE + USER_MEMORY_SIZE; va += PAGE_SIZE) {
        const uint64_t* pte = find_pte(current_pml4, va);
        if (!pte || !(*pte & MMU_PRESENT)) continue;

        void* page = alloc_physical_page();
        if (!page) return false;

        memcpy(page, (const void*)(*pte & ~0xFFFULL), PAGE_SIZE);

        if (map_page_in(new_pml4, va, (uint64_t)page, *pte & 0xFFF) < 0) {
            free_physical_page(page);
            return false;
        }
    }
    return true;
}


void switch_vm(uint64_t pml4_addr) {
    asm volatile("mov %0, %%cr3" : : "r"(pml4_addr) : "memory");
}

uint64_t read_cr3(void) {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value & ~0xFFFULL;
}
