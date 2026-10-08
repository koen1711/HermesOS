#ifndef OS_DRIVER_H
#define OS_DRIVER_H

#include "os/stddef.h"
#include "os/types.h"

#define O_RDONLY 0x01
#define O_WRONLY 0x02
#define INODE_FILE 1
#define INODE_DIR 2

#define MAX_FILE_PATH 50

struct inode;
struct file;
struct MountedFileSystem;

typedef struct FileSystemDriver {
    struct inode* (*mount)(void* blob);
    void (*unmount)(struct inode* root);
}FileSystemDriver;

typedef struct Inode_Operations {
    struct inode* (*lookup)(struct inode* dir, const char* name);
}Inode_Operations;

typedef struct File_Operations {
    int (*open)(struct inode* node, struct file* f);
    ssize_t (*read)(struct file* f, void* buf, size_t len);
    ssize_t (*write)(struct file* f, const void* buf, size_t len);
    int (*close)(struct file* f);
}File_Operations;

typedef struct inode {
    size_t id;
    size_t type; //directory or file
    size_t size;
    size_t data_offset; //begin of data in blob
    char path[MAX_FILE_PATH];
    File_Operations* fops;
    Inode_Operations* iops;
    void* private_data;
}inode;

typedef struct file {
    size_t id;
    char filename[64];
    inode* node;
    size_t file_position; //how far are we in a file
    int flags;
    struct MountedFileSystem* mount;
}file;

#endif //OS_DRIVER_H
