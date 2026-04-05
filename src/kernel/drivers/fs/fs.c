#include "fs.h"
#include "fat/fat32.h"
#include "hardware/terminal/stdio.h"

void register_filesystem(fs_node * node) {
    switch (node->fs_type) {
    case FS_FAT32:
        fat32_initialize(node);
        break;
    case FS_EXT4:
        printf("EXT4 filesystem detected\n");
        break;
    case FS_EXT3:
        printf("EXT3 filesystem detected\n");
        break;
    case FS_EFI_SYSTEM:
        fat32_initialize(node);
        break;
    case FS_BIOS_BOOT:
        break;
    default:
        printf("Unknown filesystem detected\n");
        break;
    }
}
