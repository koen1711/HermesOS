#ifndef OS_VFS_V2_H
#define OS_VFS_V2_H

#include <drivers/fs/vfs/driver.h>

#define MAX_OPEN_FILES 1024
#define MAX_MOUNTED_FILESYSTEMS 10
#define MAX_DRIVERS 8
#define VFS_NAME_MAX 16
#define VFS_PATH_MAX MAX_PATH_LENGTH

enum seek_origin {
    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2
};

typedef struct vfs_stat_s {
    size_t size;
    size_t type;
    uint64_t ino;
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
int vfs_sync();
int vfs_create(const char *path, bool directory);
int vfs_unlink(const char *path, bool directory);
int vfs_rename(const char *old_path, const char *new_path, bool no_replace);
int vfs_truncate(int fd, size_t size);
int64_t vfs_seek(int fd, int64_t offset, enum seek_origin origin);
int vfs_readdir(int fd, struct vfs_dirent *out);
int vfs_stat(int fd, vfs_stat_t *st);
int vfs_normalize_path(const char *base, const char *path, char *out);
struct file_s* vfs_get_file(int fd);
int vfs_install(struct file_s* f);

#endif //OS_VFS_V2_H
