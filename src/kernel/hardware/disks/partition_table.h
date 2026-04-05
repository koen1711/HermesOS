#ifndef GPT_H
#define GPT_H

#include <stddef.h>
#include <stdint.h>

#include <hardware/memory/alloc.h>

#include "drivers/fs/fs_utils.h"

typedef struct
{
    uint8_t boot_indicator;
    uint8_t chs_start[3];
    uint8_t os_type;
    uint8_t chs_end[3];
    uint32_t lba_start;
    uint32_t lba_end;
} __attribute__((packed)) master_boot_record;

typedef struct
{
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32_header;
    uint32_t reserved;
    uint64_t lba_header;
    uint64_t lba_second_header;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint16_t disk_guid[8]; // UTF-16
    uint64_t lba_guid_partition_start;
    uint32_t partition_amount;
    uint32_t partition_entry_size;
    uint32_t crc32_partition;
} __attribute__((packed)) gpt_header;

typedef struct {
    uint8_t partition_type_guid[16]; // UTF-16
    uint16_t partition_guid[8]; // UTF-16
    uint64_t lba_start;
    uint64_t lba_end;
    uint64_t attributes;
    uint16_t partition_name[36]; // UTF-16
    partition_type part_type;
} __attribute__((packed)) gpt_entry;

typedef struct
{
    master_boot_record* boot_record;
    gpt_header* header;
    gpt_entry* entries[128];
    uint32_t num_entries;
} __attribute__((packed)) full_gpt;

static const uint8_t GUIDS[][16] = {
    { 0xEB, 0xD0, 0xA0, 0xA2, 0xB9, 0xE5, 0x44, 0x33, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC8 }, // FAT32
    { 0x0F, 0xC6, 0x3D, 0xAF, 0x84, 0x83, 0x47, 0x72, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }, // EXT4
    { 0x0F, 0xC6, 0x3D, 0xAF, 0x84, 0x83, 0x47, 0x72, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE3 }, // EXT3
    { 0xC1, 0x2A, 0x73, 0x28, 0xF8, 0x1F, 0x11, 0xD2, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }, // EFI System
    { 0x21, 0x68, 0x61, 0x48, 0x64, 0x49, 0x6E, 0x6F, 0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 }  // BIOS Boot
};

static const partition_type TYPES[] = {
    FS_FAT32, FS_EXT4, FS_EXT3, FS_EFI_SYSTEM, FS_BIOS_BOOT
};

// Converts a GUID from little-endian to big-endian format
static void swap_guid_endian(const uint8_t* guid, uint8_t* out) {
    // Swap first 3 fields (little-endian -> big-endian)
    out[0] = guid[3]; out[1] = guid[2]; out[2] = guid[1]; out[3] = guid[0];  // uint32_t
    out[4] = guid[5]; out[5] = guid[4];  // uint16_t
    out[6] = guid[7]; out[7] = guid[6];  // uint16_t
    // Last 8 bytes stay the same
    memcpy(&out[8], (void*)&guid[8], 8);
}

// Function to map GPT partition type to filesystem type
static partition_type gpt_partition_fs_type(const gpt_entry* entry) {
    uint8_t guid_fixed[16];
    swap_guid_endian(entry->partition_type_guid, guid_fixed);  // Fix endianness

    // Check which FULL GUID matches
    for (size_t i = 0; i < sizeof(GUIDS) / sizeof(GUIDS[0]); i++)
        if (memcmp(guid_fixed, GUIDS[i], sizeof(GUIDS[0])) == 0)
            return TYPES[i];


    return FS_UNKNOWN;
}

#endif //GPT_H
