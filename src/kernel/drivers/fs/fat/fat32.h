#ifndef FAT32_H
#define FAT32_H

#include <drivers/fs/vfs/driver.h>
#include <os/stdint.h>
#include <os/stdbool.h>

/* ── block device abstraction ─────────────────────────────────── */

struct block_device;

struct block_device_ops {
    int (*read_bytes)(struct block_device *bdev, uint64_t off, uint32_t len, void *buf);
    int (*write_bytes)(struct block_device *bdev, uint64_t off, uint32_t len, const void *buf);
    int (*flush)(struct block_device *bdev);
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

/* ── in-memory driver info ────────────────────────────────────── */

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
    uint32_t total_clusters;
    bool     fsinfo_dirty;

    uint32_t fat_cache_sector;     /* one FAT sector kept in memory */
    bool     fat_cache_valid;
    uint8_t  fat_cache[512];

    struct fat32_inode_info *open_nodes;
} fat32_sb_info;

/* inode_t.private_data of every FAT32 inode; one per file, shared by all its opens */
typedef struct fat32_inode_info {
    fat32_sb_info *sbi;
    inode_t *inode;
    struct fat32_inode_info *next;
    int      refs;
    uint32_t first_cluster;
    uint32_t size;
    uint8_t  attr;
    bool     is_root;
    bool     orphan;        /* unlinked while open, the chain is freed by the last release */

    uint32_t dir_cluster;   /* first cluster of the parent directory */
    uint32_t dirent_pos;    /* byte offset of the 8.3 entry in the parent */
    uint32_t lfn_pos;       /* first long name entry, dirent_pos without one */

    uint32_t cache_index;   /* last cluster looked up in the chain */
    uint32_t cache_cluster;
} fat32_inode_info;

/* ── VFS interface ────────────────────────────────────────────── */

/* Register with vfs_register_driver("fat32", &fat32_driver).
 * vfs_mount passes a struct block_device* as the blob. */
extern file_system_driver_t fat32_driver;

/* Helper: create a block_device suitable for vfs_mount(..., "fat32", bdev) */
struct block_device *fat32_create_bdev(uint32_t unique_id,
                                       uint64_t part_offset_bytes,
                                       uint64_t part_length_bytes,
                                       const struct block_device_ops *ops,
                                       void *private);

#endif /* FAT32_H */
