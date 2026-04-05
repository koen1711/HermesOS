#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include <drivers/fs/fs_utils.h>

typedef enum {
    DISK_UNKNOWN = 0,
    DISK_ATA,
    DISK_ATAPI,
} disk_type_t;

typedef struct {
    const char* device_name;
    uint32_t unique_id;
    uint64_t flags;

    const char* name;

    uint64_t length;
    uint64_t offset;

    disk_type_t disk_type;
    partition_type part_type;
} disk_node;

#endif /* DISK_H */
