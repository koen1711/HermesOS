#ifndef OS_VFS_V2_H
#define OS_VFS_V2_H

#include <drivers/fs/vfs/driver.h>

#define MAX_OPEN_FILES 256
#define MAX_MOUNTED_FILESYSTEMS 10
#define MAX_DRIVERS 8
#define VFS_NAME_MAX 16
#define VFS_PATH_MAX 64

enum seek_origin {
    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2
};

typedef struct vfs_stat_s {
    size_t size; //file size in bytes
    size_t type; //INODE_FILE or INODE_DIR
} vfs_stat_t;

typedef struct registered_driver_s {
    char name[VFS_NAME_MAX];
    int magic_bytes;
    file_system_driver_t *fsd;
} registered_driver_t;

typedef struct mounted_file_system_s {
    char mount_path[VFS_PATH_MAX];
    size_t mount_path_length;
    struct inode_s* root;
    registered_driver_t* driver;
    int flags;
    int open_count;
} mounted_file_system_t;

int  vfs_register_driver(const char *name, file_system_driver_t* fsd);
int vfs_mount(const char *mount_path, const char* fs_name, void *blob);
int vfs_unmount(const char *mount_path);
int vfs_open(const char *path, int flags);
ssize_t vfs_read(int fd, void *buf, size_t length);
ssize_t vfs_write(int fd, const void *buf, size_t length);
int vfs_close(int fd);
int vfs_stat(int fd, vfs_stat_t *st);

#endif //OS_VFS_V2_H
