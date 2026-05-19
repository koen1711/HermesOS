#ifndef PMM_MULTIBOOT_INFO_H
#define PMM_MULTIBOOT_INFO_H

#define MAX_MEMORY_REGIONS 64

#define MULTIBOOT_TAG_TYPE_END 0
#define MULTIBOOT_TAG_TYPE_MMAP 6

#include <os/stddef.h>

#include "os/stdint.h"

typedef enum {
    MEMORY_AVAILABLE = 1,
    MEMORY_RESERVED  = 2,
    MEMORY_ACPI_RECLAIMABLE = 3,
    MEMORY_NVS       = 4,
    MEMORY_BADRAM    = 5
} memory_type_t;

typedef struct {
    uint64_t addr;
    uint64_t length;
    memory_type_t type;
} memory_region_t;

typedef struct {
    memory_region_t regions[MAX_MEMORY_REGIONS];
    size_t region_count;
} multiboot_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    // Followed by mmap entries
} __attribute__((packed)) multiboot_tag_mmap_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) multiboot_mmap_entry_t;

typedef struct
{
    uint32_t total_size; // Total size of the multiboot info structure
    uint32_t reserved;   // Reserved, must be zero
    multiboot_tag_t tags[];
} __attribute__((packed)) multiboot_info_t_header;

multiboot_info_t parse_multiboot_info(multiboot_info_t_header* mb_info);

#endif // PMM_MULTIBOOT_INFO_H
