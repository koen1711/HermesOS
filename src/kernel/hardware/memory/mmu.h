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

#define PAGE_SIZE         0x1000
#define USER_VIRTUAL_BASE 0x8000000000ULL
#define USER_MEMORY_SIZE  (16 * PAGE_SIZE)

typedef struct {
    uint64_t* pml4;
    uint64_t* pdpt_tables;
    uint64_t* pd_tables;
    uint64_t* pt_tables;
    size_t num_pdpt;
    size_t num_pd;
    size_t num_pt;
} mmu_context_t;

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
uint64_t* mmu_find_pte(uint64_t pml4_addr, uint64_t virtual_addr);
void     switch_vm(uint64_t pml4_addr);
uint64_t read_cr3(void);

void setup_kernel_mapping(const mmu_context_t* mmu);
void setup_identity_mapping(const mmu_context_t* mmu);
void setup_device_mappings(mmu_context_t* mmu);

extern void* alloc_physical_page(void);

#endif // MMU_H
