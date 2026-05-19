#include "multiboot.h"


multiboot_info_t parse_multiboot_info(multiboot_info_t_header* mb_info) {
    multiboot_info_t result = {0};

    uint8_t* ptr = (uint8_t*)mb_info + 8; // Skip total_size and reserved

    while (1) {
        multiboot_tag_t* tag = (multiboot_tag_t*)ptr;

        if (tag->type == MULTIBOOT_TAG_TYPE_END)
            break;

        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            multiboot_tag_mmap_t* mmap_tag = (multiboot_tag_mmap_t*)tag;
            multiboot_mmap_entry_t* mmap_entry =
                (multiboot_mmap_entry_t*)((uint8_t*)mmap_tag + sizeof(multiboot_tag_mmap_t));

            const size_t entries = (mmap_tag->size - sizeof(multiboot_tag_mmap_t)) / mmap_tag->entry_size;

            for (size_t i = 0; i < entries && result.region_count < MAX_MEMORY_REGIONS; i++) {
                result.regions[result.region_count].addr = mmap_entry->addr;
                result.regions[result.region_count].length = mmap_entry->len;
                result.regions[result.region_count].type = (memory_type_t)mmap_entry->type;
                result.region_count++;

                mmap_entry = (multiboot_mmap_entry_t*)((uint8_t*)mmap_entry + mmap_tag->entry_size);
            }
        }

        ptr += (tag->size + 7) & ~7; // 8-byte alignment
    }

    return result;
}
