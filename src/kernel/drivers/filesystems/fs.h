#ifndef FS_H
#define FS_H
#include <stdint.h>

enum FS_TYPE {
    FS_TYPE_FAT32
};

enum DISK_TYPE {
    ATAPI,
    ATA,
    NVME,
    USB,
};

struct fs_node {
    char name[64];

    uint64_t length;
    uint64_t offset;

    enum FS_TYPE fs_type;
    enum DISK_TYPE disk_type;

    void *data;
};

void register_disks();

#endif //FS_H
