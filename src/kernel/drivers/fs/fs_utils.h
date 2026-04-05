#ifndef OS_FS_UTILS_H
#define OS_FS_UTILS_H

#include <stdbool.h>

typedef enum {
    BOOT_UNKNOWN,
    BOOT_GPT,
    BOOT_EL_TORITO,
} boot_type;

typedef enum
{
    FS_UNKNOWN,
    FS_FAT32,
    FS_EXT4,
    FS_EXT3,
    FS_EXT2,
    FS_EFI_SYSTEM,
    FS_BIOS_BOOT
} partition_type;


#endif //OS_FS_UTILS_H