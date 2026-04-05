#ifndef FS_HEADER_H
#define FS_HEADER_H

#include <stdint.h>

#include "stdbool.h"

// typedef enum {
//     BOOT_UNKNOWN,
//     BOOT_GPT,
//     BOOT_EL_TORITO,
// } boot_type;
//
// typedef enum
// {
//     FS_UNKNOWN,
//     FS_FAT32,
//     FS_EXT4,
//     FS_EXT3,
//     FS_EXT2,
//     FS_EFI_SYSTEM,
//     FS_BIOS_BOOT
// } file_system_type;
//
// typedef enum {
//     ATAPI,
//     ATA,
//     NVME,
//     USB,
// } disk_type;
//
// typedef struct fs_node_s {
//     int fd; // File descriptor
//     char* path; // Path to the file
//     int flags; // Flags for opening the file
//     bool is_directory; // Is this a directory?
//     struct fs_node_s* children; // Children nodes (for directories)
// } __attribute__((packed)) vfs_node;
//
// // Function pointer types for filesystem operations
// typedef int (*fs_mount_func)(uint32_t unique_id, vfs_node* root);
// typedef int (*fs_read_func)(uint32_t unique_id, uint64_t begin_byte, uint64_t end_byte, uint8_t* buffer);
// typedef int (*fs_write_func)(uint32_t unique_id, uint64_t begin_byte, uint64_t end_byte, const uint8_t* buffer);

struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void (*destroy_inode)(struct inode *);
    void (*free_inode)(struct inode *);

    void (*dirty_inode) (struct inode *, int flags);
    int (*write_inode) (struct inode *, struct writeback_control *wbc);
    int (*drop_inode) (struct inode *);
    void (*evict_inode) (struct inode *);
    void (*put_super) (struct super_block *);
    int (*sync_fs)(struct super_block *sb, int wait);
    int (*freeze_super) (struct super_block *sb,
                            enum freeze_holder who);
    int (*freeze_fs) (struct super_block *);
    int (*thaw_super) (struct super_block *sb,
                            enum freeze_wholder who);
    int (*unfreeze_fs) (struct super_block *);
    int (*statfs) (struct dentry *, struct kstatfs *);
    int (*remount_fs) (struct super_block *, int *, char *);
    void (*umount_begin) (struct super_block *);

    int (*show_options)(struct seq_file *, struct dentry *);
    int (*show_devname)(struct seq_file *, struct dentry *);
    int (*show_path)(struct seq_file *, struct dentry *);
    int (*show_stats)(struct seq_file *, struct dentry *);

    ssize_t (*quota_read)(struct super_block *, int, char *, size_t, loff_t);
    ssize_t (*quota_write)(struct super_block *, int, const char *, size_t, loff_t);
    struct dquot **(*get_dquots)(struct inode *);

    long (*nr_cached_objects)(struct super_block *,
                            struct shrink_control *);
    long (*free_cached_objects)(struct super_block *,
                            struct shrink_control *);
};

struct file_system_type {
    const char *name;
    int fs_flags;
    int (*init_fs_context)(struct fs_context *);
    const struct fs_parameter_spec *parameters;
    struct dentry *(*mount) (struct file_system_type *, int,
            const char *, void *);
    void (*kill_sb) (struct super_block *);
    struct module *owner;
    struct file_system_type * next;
    struct hlist_head fs_supers;

    struct lock_class_key s_lock_key;
    struct lock_class_key s_umount_key;
    struct lock_class_key s_vfs_rename_key;
    struct lock_class_key s_writers_key[SB_FREEZE_LEVELS];

    struct lock_class_key i_lock_key;
    struct lock_class_key i_mutex_key;
    struct lock_class_key invalidate_lock_key;
    struct lock_class_key i_mutex_dir_key;
};


#endif //FS_HEADER_H
