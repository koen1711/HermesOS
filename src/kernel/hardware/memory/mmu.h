#ifndef MMU_H
#define MMU_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>

#define MMU_PRESENT       0x001
#define MMU_WRITABLE      0x002
#define MMU_USER          0x004
#define MMU_PDE_TWO_MB    0x080
#define MMU_CACHE_DISABLE 0x010

#define TWO_MEGABYTES     0x200000
#define IDENTITY_MAP_SIZE 0x40000000ULL

#define PAGE_SIZE         0x1000
#define PAGE_MASK         (~(uint64_t)(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x)  (((uint64_t)(x) + PAGE_SIZE - 1) & PAGE_MASK)
#define DIRECT_MAP_BASE   0xFFFF800000000000ULL
#define USER_VIRTUAL_BASE 0x8000000000ULL
#define USER_MEMORY_SIZE  (16 * PAGE_SIZE)

void mmu_initialize(void);
int  map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
void unmap_page(uint64_t virtual_addr);
void* vmalloc(size_t size);
void  vfree(void* ptr);
void  free_vm(uint64_t pml4_addr);


int      map_page_in(uint64_t pml4_addr, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
uint64_t setup_kvm(void);
uint64_t mmu_kernel_page_map(void);
uint64_t vmm_alloc_mmio_region(size_t size);
void     vmm_free_mmio_region(uint64_t virt, size_t size);
uint64_t* mmu_find_pte(uint64_t pml4_addr, uint64_t virtual_addr);
void     switch_vm(uint64_t pml4_addr);
uint64_t read_cr3(void);

extern uint64_t direct_map_offset;

static inline void* phys_to_virt(const uint64_t phys) {
    return (void*)(phys + direct_map_offset);
}

static inline uint64_t virt_to_phys(const void* virt) {
    const uint64_t addr = (uint64_t)virt;
    return addr >= DIRECT_MAP_BASE ? addr - DIRECT_MAP_BASE : addr;
}

#endif // MMU_H
