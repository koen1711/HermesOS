#ifndef FAT32_H
#define FAT32_H

#include <drivers/fs/fs_utils.h>
#include <hardware/disks/disk.h>

typedef struct {
    uint8_t jump_instruction[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t number_of_fats;
    uint16_t root_entries;
    uint16_t total_sectors;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t number_of_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_large;
    uint32_t sectors_per_fat32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char file_system_type[8];
    uint8_t boot_code[420];
    uint16_t boot_signature_2;

} __attribute__((packed)) fat32_boot_sector;

typedef struct
{
    uint32_t lead_signature;
    uint8_t reserved1[480];
    uint32_t structure_signature;
    uint32_t free_cluster_count;
    uint32_t next_free_cluster;
    uint8_t reserved2[12];
    uint32_t trail_signature;
} __attribute__((packed)) fat32_fsinfo;

typedef struct
{
    fat32_boot_sector* boot_sector;
    fat32_fsinfo* fs_info;
}  __attribute__((packed)) fat32_node;

typedef struct {
    char filename[8];
    char extension[3];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t modification_time;
    uint16_t modification_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat32_directory_entry;

typedef struct {
    uint8_t sequence_number;
    uint16_t name1[5];
    uint8_t attributes;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t reserved;
    uint16_t name3[2];
} __attribute__((packed)) fat32_long_filename_entry;

typedef struct
{
    char filename[255]; // Max 255 characters
    uint32_t cluster_number;
    uint32_t size;
    uint32_t start_sector;
} fat32_file;

typedef struct fat32_dir_s
{
    char filename[255];
    uint32_t n_files;
    uint32_t n_subdirs;

    struct fat32_dir_s* subdirs;
    fat32_file* files;
} fat32_dir;

void fat32_initialize(const fs_node* device);

#endif //FAT32_H
