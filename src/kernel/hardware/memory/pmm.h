#ifndef PMM_H
#define PMM_H

#include <os/stdint.h>

#include <multiboot.h>

void pmm_initialize(multiboot_info_t mb_info);
void pmm_add_region(uint64_t base_addr, uint64_t length);

void* alloc_physical_page(void);
void free_physical_page(void* page);

void* alloc_physical_pages(size_t count);
void free_physical_pages(void* first, size_t count);

size_t pmm_total_pages(void);
size_t pmm_free_pages(void);

#endif