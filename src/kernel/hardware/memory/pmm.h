#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#include <multiboot.h>

void pmm_initialize(multiboot_info_t mb_info);
void pmm_add_region(uint64_t base_addr, uint64_t length);

void* alloc_physical_page(void);
void free_physical_page(void* page);

#endif