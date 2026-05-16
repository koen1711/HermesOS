#ifndef MMU_H
#define MMU_H

#include <os/stdint.h>
#include <os/stddef.h>

// Flags for page table entries
#define MMU_PRESENT       0x001
#define MMU_WRITABLE      0x002
#define MMU_USER          0x004
#define MMU_PDE_TWO_MB    0x080
#define MMU_CACHE_DISABLE 0x010

#define TWO_MEGABYTES     0x200000

typedef struct {
    uint64_t* pml4;
    uint64_t* pdpt_tables;
    uint64_t* pd_tables;
    uint64_t* pt_tables;
    size_t num_pdpt;
    size_t num_pd;
    size_t num_pt;
} mmu_context_t;

// External interface
void mmu_initialize(void);
int  map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
void unmap_page(uint64_t virtual_addr);
void* vmalloc(size_t size);
void  vfree(void* ptr);

// Internal helpers (can be static in mmu.c or exposed if needed)
void setup_kernel_mapping(const mmu_context_t* mmu);
void setup_identity_mapping(const mmu_context_t* mmu);
void setup_device_mappings(mmu_context_t* mmu);

extern void* alloc_physical_page(void);

#endif // MMU_H
