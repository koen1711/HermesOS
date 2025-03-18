#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

struct fat32_boot_sector {
    unsigned char jump_instruction[3];
    char oem_name[8];
    unsigned short bytes_per_sector;
    unsigned char sectors_per_cluster;
    unsigned short reserved_sectors;
    unsigned char number_of_fats;
    unsigned short root_entries;
    unsigned short total_sectors;
    unsigned char media_descriptor;
    unsigned short sectors_per_fat;
    unsigned short sectors_per_track;
    unsigned short number_of_heads;
    unsigned int hidden_sectors;
    unsigned int total_sectors_large;
    unsigned char drive_number;
    unsigned char current_head;
    unsigned char boot_signature;
    unsigned int volume_id;
    char volume_label[11];
    char fs_type[8];
    char boot_code[448];
    unsigned short boot_sector_signature;
} __attribute__((packed));

struct fat32_directory_entry {
    char filename[8];
    char extension[3];
    unsigned char attributes;
    unsigned char reserved;
    unsigned char creation_time_tenths;
    unsigned short creation_time;
    unsigned short creation_date;
    unsigned short access_date;
    unsigned short cluster_high;
    unsigned short modification_time;
    unsigned short modification_date;
    unsigned short cluster_low;
    unsigned int file_size;
} __attribute__((packed));

struct fat32_long_filename_entry {
    unsigned char sequence_number;
    unsigned short name1[5];
    unsigned char attributes;
    unsigned char type;
    unsigned char checksum;
    unsigned short name2[6];
    unsigned short reserved;
    unsigned short name3[2];
} __attribute__((packed));

void fat32_initialize(uint16_t drive);

#endif //FAT32_H
