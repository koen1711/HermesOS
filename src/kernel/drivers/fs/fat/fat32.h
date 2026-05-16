#ifndef FAT32_H
#define FAT32_H

#include <drivers/fs/vfs/vfs_type.h>
#include <os/stdint.h>

/* ── block device abstraction ─────────────────────────────────── */

struct block_device;

struct block_device_ops {
    int (*read_bytes)(struct block_device *bdev, uint64_t off, uint32_t len, void *buf);
    int (*write_bytes)(struct block_device *bdev, uint64_t off, uint32_t len, const void *buf);
};

struct block_device {
    uint32_t    unique_id;
    uint64_t    part_offset;   /* partition start in bytes   */
    uint64_t    part_length;   /* partition length in bytes   */
    const struct block_device_ops *ops;
    void       *private;       /* e.g. pointer to ata_device */
};

/* ── on-disk structures ───────────────────────────────────────── */

typedef struct {
    uint8_t  jump_instruction[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  number_of_fats;
    uint16_t root_entries;
    uint16_t total_sectors;
    uint8_t  media_descriptor;
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
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     file_system_type[8];
    uint8_t  boot_code[420];
    uint16_t boot_signature_2;
} __attribute__((packed)) fat32_boot_sector;

typedef struct {
    uint32_t lead_signature;
    uint8_t  reserved1[480];
    uint32_t structure_signature;
    uint32_t free_cluster_count;
    uint32_t next_free_cluster;
    uint8_t  reserved2[12];
    uint32_t trail_signature;
} __attribute__((packed)) fat32_fsinfo;

typedef struct {
    char     filename[8];
    char     extension[3];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  creation_time_tenths;
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
    uint8_t  sequence_number;
    uint16_t name1[5];
    uint8_t  attributes;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t reserved;
    uint16_t name3[2];
} __attribute__((packed)) fat32_long_filename_entry;

/* ── in-memory VFS info ───────────────────────────────────────── */

typedef struct fat32_sb_info {
    struct block_device *bdev;
    fat32_boot_sector    bs;
    fat32_fsinfo         fsinfo;

    uint32_t fat_begin_lba;
    uint32_t data_begin_lba;
    uint32_t sectors_per_fat;
    uint32_t sectors_per_cluster;
    uint32_t root_cluster;
    uint32_t bytes_per_sector;
} fat32_sb_info;

typedef struct fat32_inode_info {
    uint32_t first_cluster;
    uint32_t size;
    uint8_t  attr;

    uint32_t parent_dir_cluster;
    uint32_t dirent_offset;
} fat32_inode_info;

/* ── VFS interface ────────────────────────────────────────────── */

extern struct file_system_type fat32_fs_type;

struct super_block *fat32_mount(struct file_system_type *fs_type,
                                uint32_t mount_flags,
                                const char *dev_name,
                                void *data);
void fat32_kill_sb(struct super_block *sb);

/* Helper: create a block_device suitable for fat32_mount(data=bdev) */
struct block_device *fat32_create_bdev(uint32_t unique_id,
                                       uint64_t part_offset_bytes,
                                       uint64_t part_length_bytes,
                                       const struct block_device_ops *ops,
                                       void *private);

#endif /* FAT32_H */
