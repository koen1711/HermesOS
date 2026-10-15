#ifndef OS_DRIVER_H
#define OS_DRIVER_H

#include "os/stddef.h"
#include "os/types.h"

#define O_RDONLY 0x01
#define O_WRONLY 0x02
#define INODE_FILE 1
#define INODE_DIR 2

#define MAX_FILE_PATH 50

struct inode_s;
struct file_s;
struct mounted_file_system_s;

typedef struct file_system_driver_ts {
    struct inode_s* (*mount)(void* blob);
    void (*unmount)(struct inode_s* root);
} file_system_driver_t;

typedef struct inode_operations_s {
    struct inode_s* (*lookup)(struct inode_s* dir, const char* name);
} inode_operations_t;

typedef struct file_operations_s {
    int (*open)(struct inode_s* node, struct file_s* f);
    ssize_t (*read)(struct file_s* f, void* buf, size_t len);
    ssize_t (*write)(struct file_s* f, const void* buf, size_t len);
    int (*close)(struct file_s* f);
} file_operations_t;

typedef struct inode_s {
    size_t id;
    size_t type; //directory or file
    size_t size;
    size_t data_offset; //begin of data in blob
    char path[MAX_FILE_PATH];
    file_operations_t* fops;
    inode_operations_t* iops;
    void* private_data;
} inode_t;

typedef struct file_s {
    size_t id;
    char filename[64];
    inode_t* node;
    size_t file_position; //how far are we in a file
    int flags;
    struct mounted_file_system_s* mount;
} file_t;

#endif //OS_DRIVER_H
