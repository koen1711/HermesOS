#include "fs.h"
#include "fat/fat32.h"
#include "vfs/vfs.h"
#include <utils/str/str.h>


void fs_init(void)
{
    vfs_register_driver("fat32", &fat32_driver);
}


int fs_mount_partition(const char *fs_type_name, const char *dev_name,
                       struct block_device *bdev)
{
    char path[VFS_PATH_MAX] = {0};

    if (vfs_mount("/", fs_type_name, bdev) == 0)
        return 0;

    path[0] = '/';
    strncpy(path + 1, dev_name, VFS_PATH_MAX - 2);
    return vfs_mount(path, fs_type_name, bdev);
}
