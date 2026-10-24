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

static uint64_t* split_two_mb_page(uint64_t* pd, const uint16_t pd_idx) {
    const uint64_t entry = pd[pd_idx];
    uint64_t* pt = allocate_page_table();
    if (!pt) return nullptr;

    const uint64_t base = entry & 0x000FFFFFFFE00000ULL;
    const uint64_t flags = (entry & 0xFFFULL & ~MMU_PDE_TWO_MB) | (entry & (1ULL << 63));
    for (int i = 0; i < 512; i++) {
        pt[i] = (base + i * PAGE_SIZE) | flags;
    }
    pd[pd_idx] = (uint64_t)pt | MMU_PRESENT | MMU_WRITABLE | (entry & MMU_USER);
    return pt;
}

static uint64_t* get_or_create_pt(uint64_t* pd, const uint16_t pd_idx, const uint64_t flags) {
    const uint64_t entry = pd[pd_idx];
    if ((entry & MMU_PRESENT) && (entry & MMU_PDE_TWO_MB)) {
        return split_two_mb_page(pd, pd_idx);
    }
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

int map_page(const uint64_t virtual_addr, const uint64_t physical_addr, const uint64_t flags) {
    return map_page_in((uint64_t)kernel_mmu.pml4, virtual_addr, physical_addr, flags);
}

void unmap_page(uint64_t virtual_addr) {
    uint64_t* pte = mmu_find_pte((uint64_t)kernel_mmu.pml4, virtual_addr);
    if (!pte) return;

    *pte = 0;

    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

void setup_kernel_mapping(const mmu_context_t* mmu) {
    constexpr uint64_t size = 0x40000000ULL;

    uint64_t* high_pdpt = allocate_page_table();
    mmu->pml4[511] = (uint64_t)high_pdpt | MMU_PRESENT | MMU_WRITABLE;

    uint64_t* kernel_pd = allocate_page_table();
    high_pdpt[510] = (uint64_t)kernel_pd | MMU_PRESENT | MMU_WRITABLE;

    for (uint64_t i = 0; i < (size / TWO_MEGABYTES); i++) {
        constexpr uint64_t phys_start = 0x400000ULL;
        const uint64_t phys_addr = phys_start + (i * TWO_MEGABYTES);
        kernel_pd[i] = phys_addr | MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB;
    }
}

void setup_identity_mapping(const mmu_context_t* mmu) {
    constexpr uint64_t size = IDENTITY_MAP_SIZE;

    uint64_t* pdpt = allocate_page_table();
    mmu->pml4[0] = (uint64_t)pdpt | MMU_PRESENT | MMU_WRITABLE;

    uint64_t* pd = allocate_page_table();
    pdpt[0] = (uint64_t)pd | MMU_PRESENT | MMU_WRITABLE;

    for (uint64_t i = 0; i < (size / TWO_MEGABYTES); i++) {
        constexpr uint64_t phys_start = 0x00000000ULL;
        const uint64_t phys_addr = phys_start + (i * TWO_MEGABYTES);
        pd[i] = phys_addr | MMU_PRESENT | MMU_WRITABLE | MMU_PDE_TWO_MB;
    }
}

void setup_device_mappings(mmu_context_t* mmu) {
    constexpr uint64_t virtual_addr = 0xFFFFFFFFFEC00000ULL;
    constexpr uint64_t phys = 0xFEC00000ULL;
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
    if ((uintptr_t)addr + size <= IDENTITY_MAP_SIZE) return addr;

    for (size_t i = 0; i < size; i += 0x1000) {
        if (map_page((uintptr_t)addr + i, (uintptr_t)addr + i, MMU_PRESENT | MMU_WRITABLE) < 0) {
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

    const uintptr_t addr = (uintptr_t)ptr;
    if (addr >= IDENTITY_MAP_SIZE) {
        unmap_page(addr);
    }
    free_physical_page(ptr);
}

void free_vm(const uint64_t pml4_addr) {
    const auto pml4 = (uint64_t*)pml4_addr;
    if (!pml4) return;

    const auto kernel_low = (const uint64_t*)(kernel_mmu.pml4[0] & ~0xFFFULL);
    for (int i = 0; i < 512; i++) {
        if (!(pml4[i] & MMU_PRESENT) || pml4[i] == kernel_mmu.pml4[i]) continue;
        const auto pdpt = (uint64_t*)(pml4[i] & ~0xFFFULL);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & MMU_PRESENT) || (i == 0 && pdpt[j] == kernel_low[j])) continue;
            const auto pd = (uint64_t*)(pdpt[j] & ~0xFFFULL);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & MMU_PRESENT) || (pd[k] & MMU_PDE_TWO_MB)) continue;
                const auto pt = (uint64_t*)(pd[k] & ~0xFFFULL);

                for (int l = 0; l < 512; l++) {
                    if ((pt[l] & MMU_USER) && (pt[l] & 0x000FFFFFFFFFF000ULL)) {
                        free_physical_page((void*)(pt[l] & 0x000FFFFFFFFFF000ULL));
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

uint64_t* mmu_find_pte(const uint64_t pml4_addr, const uint64_t virtual_addr) {
    auto table = (uint64_t*)pml4_addr;
    for (int shift = 39; shift > 12; shift -= 9) {
        const uint64_t entry = table[(virtual_addr >> shift) & 0x1FF];
        if (!(entry & MMU_PRESENT) || (entry & MMU_PDE_TWO_MB)) return nullptr;
        table = (uint64_t*)(entry & ~0xFFFULL);
    }
    return &table[(virtual_addr >> 12) & 0x1FF];
}


#define MMIO_VIRTUAL_BASE  0xFFFFFFFFC0000000ULL
#define MMIO_VIRTUAL_LIMIT 0xFFFFFFFFFEC00000ULL

uint64_t vmm_alloc_mmio_region(size_t size) {
    static uint64_t next = MMIO_VIRTUAL_BASE;
    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0 || next + size > MMIO_VIRTUAL_LIMIT) return 0;
    const uint64_t region = next;
    next += size;
    return region;
}

uint64_t mmu_kernel_page_map(void) {
    return (uint64_t)kernel_mmu.pml4;
}

uint64_t setup_kvm(void) {
    uint64_t* pml4 = allocate_page_table();
    if (!pml4) return 0;

    for (int i = 0; i < 512; i++) {
        pml4[i] = kernel_mmu.pml4[i];
    }

    const auto kernel_low = (const uint64_t*)(kernel_mmu.pml4[0] & ~0xFFFULL);
    uint64_t* low = allocate_page_table();
    if (!low) {
        free_physical_page(pml4);
        return 0;
    }
    for (int i = 0; i < 512; i++) {
        low[i] = kernel_low[i];
    }
    pml4[0] = (uint64_t)low | MMU_PRESENT | MMU_WRITABLE | MMU_USER;
    return (uint64_t)pml4;
}

void switch_vm(uint64_t pml4_addr) {
    asm volatile("mov %0, %%cr3" : : "r"(pml4_addr) : "memory");
}

uint64_t read_cr3(void) {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value & ~0xFFFULL;
}
