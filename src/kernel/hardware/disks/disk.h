#ifndef DISK_H
#define DISK_H
#include <drivers/fs/fs_utils.h>

typedef struct {
    const char* device_name;
    uint32_t unique_id;
    uint64_t flags;

    const char* name;

    uint64_t length;
    uint64_t offset;

    file_system_type fs_type;
    disk_type disk_type;
} __attribute__((packed)) disk_node;




#endif //DISK_H
