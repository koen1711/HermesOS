#include "fs.h"
#include "fat/fat32.h"
#include "vfs/vfs.h"


void fs_init(void)
{
    vfs_init();
    vfs_register_filesystem(&fat32_fs_type);
}

int fs_mount_partition(const char *fs_type_name, const char *dev_name,
                       struct block_device *bdev)
{
    return vfs_mount_root_dev(fs_type_name, dev_name, bdev);
}
