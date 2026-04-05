#include "fat32.h"

#include <drivers/fs/fs_utils.h>

#include <hardware/memory/alloc.h>
#include <hardware/terminal/stdio.h>
#include <utils/str/str.h>

#include "../vfs/vfs.h"
#include "hardware/disks/ata.h"
#include "hardware/interrupts/panic.h"
#include "utils/list/list.h"

full_map* fat32_registry = NULL;

int fat32_read(const fs_node* node, const uint32_t begin_byte, const uint32_t size, uint8_t* buffer)
{
    return node->read(node->unique_id, begin_byte, size + begin_byte, buffer);
}

#define LFN_LAST_ENTRY_MASK 0x40
#define LFN_FIRST_ENTRY_MASK 0x3F

uint32_t fat32_extract_lfn(const fat32_directory_entry* entry, char* long_filename, const uint32_t buffer_size) {
    uint32_t offset = 0;
    uint32_t index = 0;
    fat32_long_filename_entry* lfn_entries = (fat32_long_filename_entry*)entry;

    uint8_t entry_count = 0;
    fat32_long_filename_entry* current = lfn_entries;
    while (1) {
        if (current->attributes != 0x0F) break;
        entry_count++;
        current = (fat32_long_filename_entry*)((uint8_t*)current + sizeof(fat32_long_filename_entry));
    }

    // Now process entries in the original order (from first to last)
    for (uint8_t i = entry_count; i > 0; i--) {
        const fat32_long_filename_entry* current_lfn = (fat32_long_filename_entry*)((uint8_t*)lfn_entries + (i-1) * sizeof(fat32_long_filename_entry));

        // Copy name parts, checking for buffer overflow
        for (int k = 0; k < 5; k++) {
            if (index >= buffer_size - 1) break; // Leave room for null terminator
            const uint16_t unicode_char = current_lfn->name1[k];
            if (unicode_char == 0 || unicode_char == 0xFFFF) break;
            // Handle Unicode characters (simplified - assumes ASCII)
            long_filename[index++] = (char)unicode_char;
        }

        for (int k = 0; k < 6; k++) {
            if (index >= buffer_size - 1) break;
            uint16_t unicode_char = current_lfn->name2[k];
            if (unicode_char == 0 || unicode_char == 0xFFFF) break;
            long_filename[index++] = (char)unicode_char;
        }

        for (int k = 0; k < 2; k++) {
            if (index >= buffer_size - 1) break;
            uint16_t unicode_char = current_lfn->name3[k];
            if (unicode_char == 0 || unicode_char == 0xFFFF) break;
            long_filename[index++] = (char)unicode_char;
        }
    }

    // Ensure null termination
    if (index < buffer_size) {
        long_filename[index] = '\0';
    } else {
        long_filename[buffer_size - 1] = '\0';
    }

    return entry_count * sizeof(fat32_long_filename_entry);
}

void fat32_traverse_directory(const fs_node* node, const fat32_boot_sector* boot_sector,
                              const uint32_t first_cluster, const uint32_t first_data_sector, fat32_dir* dir) {
    if (dir == NULL) return;

    const uint32_t cluster_size = boot_sector->sectors_per_cluster * boot_sector->bytes_per_sector;
    const uint32_t sector = ((first_cluster - 2) * boot_sector->sectors_per_cluster) + first_data_sector;
    uint8_t* buffer = malloc(cluster_size);
    if (!buffer) return;

    const int result = fat32_read(node, sector * boot_sector->bytes_per_sector, cluster_size, buffer);
    if (result != 0) {
        printf("Failed to read directory\n");
        free(buffer);
        return;
    }

    uint32_t entry_offset = 0;

    while (entry_offset < cluster_size) {
        char long_filename[255] = {0};
        bool has_long_filename = false;
        const fat32_directory_entry* entry = (fat32_directory_entry*)(buffer + entry_offset);

        if (entry->filename[0] == 0x00) break;
        if (entry->filename[0] == 0xE5 || (entry->attributes == 0x08)) {
            entry_offset += sizeof(fat32_directory_entry);
            continue;
        }
        if (entry->attributes & 0x0F) {
            has_long_filename = true;
            entry_offset += fat32_extract_lfn(entry, long_filename, 255);
            entry = (fat32_directory_entry*)(buffer + entry_offset);
        }


        char filename[255] = {0};

        if (has_long_filename)
        {
            strncpy(filename, long_filename, 255);
        } else {
            if (entry->attributes & 0x10) {
                // Directory
                strncpy(filename, entry->filename, 8);
            } else {
                strncpy(filename, entry->filename, 8);
                // Trim spaces
                for (int i = 8; i > 0; i--) {
                    if (filename[i - 1] == ' ') {
                        filename[i - 1] = '\0';
                    } else {
                        break;
                    }
                }
                strncat(filename, ".", 1);
                strncat(filename, entry->extension, 3);
            }
        }

        if (entry->attributes & 0x10) {
            // Check if dir is . or ..
            entry_offset += sizeof(fat32_directory_entry);
            if (strncmp(filename, ".  ", 3) == 0 || strncmp(filename, "..  ", 4) == 0)
                continue;

            fat32_dir* subdir = malloc(sizeof(fat32_dir));

            subdir->n_files = 0;
            subdir->n_subdirs = 0;
            subdir->subdirs = NULL;
            subdir->files = NULL;
            strncpy(subdir->filename, filename, 255);

            dir->subdirs = realloc(dir->subdirs, sizeof(fat32_dir) * (dir->n_subdirs + 1));
            dir->subdirs[dir->n_subdirs] = *subdir;
            dir->n_subdirs++;

            free(subdir);
            // Recursively traverse the subdirectory
            // fat32_traverse_directory(node, boot_sector,
            //     (entry->cluster_high << 16) | entry->cluster_low,
            //     first_data_sector, subdir);
        } else {
            fat32_file* new_file = malloc(sizeof(fat32_file));

            new_file->cluster_number = (entry->cluster_high << 16) | entry->cluster_low;
            new_file->size = entry->file_size;
            new_file->start_sector = ((new_file->cluster_number - 2) * boot_sector->sectors_per_cluster) + first_data_sector;
            strncpy(new_file->filename, filename, 255);

            dir->files = realloc(dir->files, sizeof(fat32_file) * (dir->n_files + 1));
            dir->files[dir->n_files] = *new_file;
            dir->n_files++;

            entry_offset += sizeof(fat32_file);
            free(new_file);
        }

    }
    free(buffer);
}

void fat32_mount(const uint32_t unique_id, vfs_node* root)
{
    fat32_node* fat_node = (fat32_node*)map_get(fat32_registry, (void*)unique_id);
    if (fat_node == NULL) {
        printf("FAT32 node not found for unique ID: %u\n", unique_id);
        return;
    }

    fat32_boot_sector* boot_sector = fat_node->boot_sector;

    const uint32_t first_data_sector = boot_sector->reserved_sectors +
                                      (boot_sector->number_of_fats * boot_sector->sectors_per_fat32);
    const uint32_t root_cluster = boot_sector->root_cluster;

    fat32_dir* root_dir = malloc(sizeof(fat32_dir));
    fat32_traverse_directory(device, boot_sector, root_cluster, first_data_sector, root_dir);

    free(root_dir);
}

void fat32_initialize(const fs_node* device)
{
    // Read the boot sector
    fat32_boot_sector* boot_sector = malloc(sizeof(fat32_boot_sector));
    if (!boot_sector) {
        printf("Failed to allocate memory for boot sector\n");
        free(boot_sector);
        return;
    }

    const int result = fat32_read(device, 0, sizeof(fat32_boot_sector), (uint8_t*)boot_sector);
    if (result != 0) {
        printf("Failed to read boot sector\nError code: %d\n", result);
        free(boot_sector);
        return;
    }

    if (boot_sector->bytes_per_sector == 0 ||
        (boot_sector->bytes_per_sector & (boot_sector->bytes_per_sector - 1)) != 0) { // Check if power of 2
        printf("Invalid bytes per sector\n");
        free(boot_sector);
        return;
    }

    fat32_fsinfo* fs_info = malloc(sizeof(fat32_fsinfo));
    if (!fs_info) {
        printf("Failed to allocate memory for FSINFO\n");
        free(boot_sector);
        return;
    }

    const int fsinfo_result = fat32_read(device, boot_sector->bytes_per_sector, sizeof(fat32_fsinfo), (uint8_t*)fs_info);
    if (fsinfo_result != 0) {
        printf("Failed to read FSINFO sector\nError code: %d\n", fsinfo_result);
        free(boot_sector);
        free(fs_info);
        return;
    }

    fs_node* root_node = malloc(sizeof(fs_node));
    if (!root_node) {
        printf("Failed to allocate memory for root node\n");
        free(fs_info);
        free(boot_sector);
        return;
    }

    root_node->flags = 0;
    root_node->device_name = device->device_name;
    root_node->unique_id = device->unique_id;
    root_node->fs_type = FS_FAT32;

    fat32_node* fat32_node = malloc(sizeof(fat32_node));
    if (!fat32_node)
    {
        printf("Failed to allocate memory for FAT32 node\n");
        free(root_node);
        free(fs_info);
        free(boot_sector);
        return;
    }

    fat32_node->boot_sector = boot_sector;
    fat32_node->fs_info = fs_info;

    if (fat32_registry == NULL)
    {
        fat32_registry = map_create(64, NULL, NULL, NULL, NULL);
    }
    map_put(fat32_registry, (void*)device->unique_id, fat32_node);

    vfs_mount(root_node, "/");
}

