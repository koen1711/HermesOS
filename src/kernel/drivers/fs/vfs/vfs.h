#ifndef OS_VFS_V2_H
#define OS_VFS_V2_H

#include <drivers/fs/vfs/driver.h>

#define MAX_OPEN_FILES 256
#define MAX_MOUNTED_FILESYSTEMS 10
#define MAX_DRIVERS 8
#define VFS_NAME_MAX 16
#define VFS_PATH_MAX 64


typedef struct RegisteredDriver {
    char name[VFS_NAME_MAX];
    int magic_bytes;
    FileSystemDriver *fsd;
} RegisteredDriver;

typedef struct MountedFileSystem {
    char mount_path[VFS_PATH_MAX];
    size_t mount_path_length;
    inode* root;
    RegisteredDriver* driver;
    int flags;
    int open_count;
} MountedFileSystem;

int  vfs_register_driver(const char *name, FileSystemDriver* fsd);
int vfs_mount(const char *mount_path, const char* fs_name, void *blob);
int vfs_unmount(const char *mount_path);
int vfs_open(const char *path, int flags);
ssize_t vfs_read(int fd, void *buf, size_t length);
ssize_t vfs_write(int fd, const void *buf, size_t length);
int vfs_close(int fd);

#endif //OS_VFS_V2_H
