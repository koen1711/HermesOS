#include "fat32.h"

void fat32_initialize(uint16_t drive)
{
    // Read the boot sector
    struct fat32_boot_sector boot_sector;
    //fs_read(drive, 0, 1, (uint16_t*)&boot_sector);
}

