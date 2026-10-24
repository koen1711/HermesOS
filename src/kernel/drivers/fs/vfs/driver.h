#ifndef OS_DRIVER_H
#define OS_DRIVER_H

#include "os/stddef.h"
#include "os/types.h"

#define O_RDONLY 0x01
#define O_WRONLY 0x02
#define INODE_FILE 1
#define INODE_DIR 2
#define INODE_SOCKET 3
#define INODE_PIPE 4
#define INODE_CHAR 5

#define MAX_FILE_PATH 256
#define MAX_PATH_LENGTH 256

struct inode_s;
struct file_s;
struct mounted_file_system_s;

struct vfs_dirent {
    uint64_t ino;
    size_t type;
    char name[MAX_FILE_PATH];
};

enum vfs_poll_events {
    VFS_POLLIN = 0x001,
    VFS_POLLPRI = 0x002,
    VFS_POLLOUT = 0x004,
    VFS_POLLERR = 0x008,
    VFS_POLLHUP = 0x010,
    VFS_POLLNVAL = 0x020
};

typedef struct file_system_driver_ts {
    struct inode_s* (*mount)(void* blob);
    void (*unmount)(struct inode_s* root);
} file_system_driver_t;

typedef struct inode_operations_s {
    struct inode_s* (*lookup)(struct inode_s* dir, const char* name);
    void (*release)(struct inode_s* node);
} inode_operations_t;

typedef struct file_operations_s {
    int (*open)(struct inode_s* node, struct file_s* f);
    ssize_t (*read)(struct file_s* f, void* buf, size_t len);
    ssize_t (*write)(struct file_s* f, const void* buf, size_t len);
    int (*close)(struct file_s* f);
    int (*readdir)(struct file_s* f, struct vfs_dirent* out);
    int (*poll)(struct file_s* f);
    int (*ioctl)(struct file_s* f, unsigned long request, void* arg);
} file_operations_t;

typedef struct inode_s {
    size_t id;
    size_t type;
    size_t size;
    size_t data_offset;
    char path[MAX_FILE_PATH];
    file_operations_t* fops;
    inode_operations_t* iops;
    void* private_data;
} inode_t;

typedef struct file_s {
    size_t id;
    char filename[MAX_PATH_LENGTH];
    inode_t* node;
    size_t file_position;
    int flags;
    int status_flags;
    struct mounted_file_system_s* mount;
    int ref_count;
} file_t;

#endif //OS_DRIVER_H
