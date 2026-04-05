#ifndef FS_H
#define FS_H

#include "fs_utils.h"

struct block_device;

/* Register all built-in filesystem types with the VFS */
void fs_init(void);

/* Mount a partition as the root filesystem.
 * fs_type_name: e.g. "fat32"
 * dev_name:     human-readable device/partition name
 * bdev:         block_device* for the partition
 */
int fs_mount_partition(const char *fs_type_name, const char *dev_name,
                       struct block_device *bdev);

#endif /* FS_H */
