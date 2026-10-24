#include "fat32.h"

#include <hardware/memory/alloc.h>
#include "drivers/terminal/terminal.h"
#include <utils/str/str.h>

#include <drivers/fs/vfs/vfs.h>
#include <os/errno.h>

#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

static inode_operations_t fat32_dir_iops;
static inode_operations_t fat32_file_iops;
static file_operations_t  fat32_dir_fops;
static file_operations_t  fat32_file_fops;

static int bdev_read(const fat32_sb_info *sbi, const uint64_t off, const uint32_t len, void *buf)
{
    return sbi->bdev->ops->read_bytes(sbi->bdev, off, len, buf);
}

static uint64_t cluster_to_byte(const fat32_sb_info *sbi, const uint32_t cluster)
{
    return (uint64_t)(sbi->data_begin_lba +
           (cluster - 2) * sbi->sectors_per_cluster) * sbi->bytes_per_sector;
}

static uint32_t cluster_byte_size(const fat32_sb_info *sbi)
{
    return sbi->sectors_per_cluster * sbi->bytes_per_sector;
}

static uint32_t fat32_next_cluster(const fat32_sb_info *sbi, const uint32_t cluster)
{
    const uint64_t fat_off = (uint64_t)sbi->fat_begin_lba * sbi->bytes_per_sector
                     + cluster * 4;
    uint32_t next = 0;
    if (bdev_read(sbi, fat_off, 4, &next) != 0)
        return 0x0FFFFFFF;
    return next & 0x0FFFFFFF;
}

static inline int is_eof_cluster(const uint32_t c) { return c >= 0x0FFFFFF8; }

static uint32_t fat32_extract_lfn(const fat32_directory_entry *entry,
                                  char *long_filename, uint32_t buffer_size)
{
    const auto lfn_entries =
        (const fat32_long_filename_entry *)entry;

    uint8_t entry_count = 0;
    const fat32_long_filename_entry *cur = lfn_entries;
    while (cur->attributes == FAT_ATTR_LFN) {
        entry_count++;
        cur = (const fat32_long_filename_entry *)
              ((const uint8_t *)cur + sizeof(fat32_long_filename_entry));
    }

    uint32_t index = 0;
    for (uint8_t i = entry_count; i > 0; i--) {
        const auto e =
            (const fat32_long_filename_entry *)
            ((const uint8_t *)lfn_entries +
             (i - 1) * sizeof(fat32_long_filename_entry));

        for (int k = 0; k < 5 && index < buffer_size - 1; k++) {
            if (e->name1[k] == 0 || e->name1[k] == 0xFFFF) goto done;
            long_filename[index++] = (char)e->name1[k];
        }
        for (int k = 0; k < 6 && index < buffer_size - 1; k++) {
            if (e->name2[k] == 0 || e->name2[k] == 0xFFFF) goto done;
            long_filename[index++] = (char)e->name2[k];
        }
        for (int k = 0; k < 2 && index < buffer_size - 1; k++) {
            if (e->name3[k] == 0 || e->name3[k] == 0xFFFF) goto done;
            long_filename[index++] = (char)e->name3[k];
        }
    }

done:
    long_filename[index < buffer_size ? index : buffer_size - 1] = '\0';
    return entry_count * sizeof(fat32_long_filename_entry);
}

#define FAT_NT_LOWER_BASE 0x08
#define FAT_NT_LOWER_EXT  0x10

static char fat32_lower(const char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* the NT reserved byte marks an all-lowercase base name or extension */
static void fat32_short_name(const fat32_directory_entry *de,
                             char *out, uint32_t out_size)
{
    int len = 0;

    for (int i = 0; i < 8 && len < (int)out_size - 1; i++) {
        if (de->filename[i] == ' ') break;
        out[len++] = de->reserved & FAT_NT_LOWER_BASE ? fat32_lower(de->filename[i]) : de->filename[i];
    }

    if (!(de->attributes & FAT_ATTR_DIRECTORY)) {
        int ext_len = 0;
        for (int i = 0; i < 3; i++) {
            if (de->extension[i] != ' ') ext_len = i + 1;
        }
        if (ext_len > 0 && len < (int)out_size - 1) {
            out[len++] = '.';
            for (int i = 0; i < ext_len && len < (int)out_size - 1; i++)
                out[len++] = de->reserved & FAT_NT_LOWER_EXT ? fat32_lower(de->extension[i]) : de->extension[i];
        }
    }

    out[len] = '\0';
}

static inode_t *fat32_make_inode(fat32_sb_info *sbi,
                                 const char *name,
                                 const uint32_t cluster,
                                 const uint32_t size,
                                 const uint8_t attr,
                                 const uint32_t parent_cluster,
                                 const uint32_t dirent_off)
{
    inode_t *inode = malloc(sizeof(inode_t));
    if (!inode)
        return nullptr;
    memset(inode, 0, sizeof(*inode));

    fat32_inode_info *fi = malloc(sizeof(fat32_inode_info));
    if (!fi)
    {
        free(inode);
        return nullptr;
    }
    memset(fi, 0, sizeof(*fi));

    fi->sbi                = sbi;
    fi->first_cluster      = cluster;
    fi->size               = size;
    fi->attr               = attr;
    fi->parent_dir_cluster = parent_cluster;
    fi->dirent_offset      = dirent_off;

    inode->id           = cluster;
    inode->size         = size;
    inode->private_data = fi;
    strncpy(inode->path, name, MAX_FILE_PATH - 1);

    if (attr & FAT_ATTR_DIRECTORY) {
        inode->type = INODE_DIR;
        inode->iops = &fat32_dir_iops;
        inode->fops = &fat32_dir_fops;
    } else {
        inode->type = INODE_FILE;
        inode->iops = &fat32_file_iops;
        inode->fops = &fat32_file_fops;
    }

    return inode;
}

static void fat32_free_inode(inode_t *inode)
{
    if (!inode) return;
    free(inode->private_data);
    free(inode);
}

static inode_t *fat32_lookup(const inode_t *dir, const char *target)
{
    const fat32_inode_info *fi = dir->private_data;
    fat32_sb_info  *sbi = fi->sbi;

    uint32_t clus = fi->first_cluster;
    const uint32_t clus_size = cluster_byte_size(sbi);
    uint8_t *buffer = malloc(clus_size);
    if (!buffer)
        return nullptr;

    const uint32_t target_len = strlen(target);

    while (!is_eof_cluster(clus)) {
        if (bdev_read(sbi, cluster_to_byte(sbi, clus), clus_size, buffer) != 0)
            break;

        uint32_t off = 0;
        while (off < clus_size) {
            const fat32_directory_entry *de =
                (const fat32_directory_entry *)(buffer + off);

            if (de->filename[0] == 0x00) goto not_found;
            if ((uint8_t)de->filename[0] == 0xE5 ||
                de->attributes == FAT_ATTR_VOLUME_ID) {
                off += sizeof(fat32_directory_entry);
                continue;
            }

            char name[256] = {0};
            uint32_t dirent_off = off;

            if ((de->attributes & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                const uint32_t lfn_bytes = fat32_extract_lfn(de, name, sizeof(name));
                off += lfn_bytes;
                de = (const fat32_directory_entry *)(buffer + off);
            } else {
                fat32_short_name(de, name, sizeof(name));
            }

            off += sizeof(fat32_directory_entry);

            if (strlen(name) != target_len)
                continue;
            if (strncmp(name, target, target_len) != 0)
                continue;

            uint32_t entry_cluster = ((uint32_t)de->cluster_high << 16)
                                   | de->cluster_low;
            inode_t *inode = fat32_make_inode(
                sbi, name, entry_cluster, de->file_size, de->attributes,
                fi->first_cluster, dirent_off);

            free(buffer);
            return inode;
        }

        clus = fat32_next_cluster(sbi, clus);
    }

not_found:
    free(buffer);
    return NULL;
}

static ssize_t fat32_file_read(file_t *file, void *user_buf, size_t len)
{
    fat32_inode_info *fi = file->node->private_data;
    fat32_sb_info  *sbi = fi->sbi;
    size_t pos = file->file_position;

    if (pos >= fi->size)
        return 0;
    if (pos + len > fi->size)
        len = fi->size - pos;
    if (len == 0)
        return 0;

    uint32_t clus_size = cluster_byte_size(sbi);
    uint8_t *clus_buf = malloc(clus_size);
    if (!clus_buf) return -1;

    uint32_t clus = fi->first_cluster;
    uint64_t skip = (uint64_t)pos;

    while (skip >= clus_size) {
        clus = fat32_next_cluster(sbi, clus);
        if (is_eof_cluster(clus)) { free(clus_buf); return 0; }
        skip -= clus_size;
    }

    size_t copied = 0;
    uint8_t *dst = (uint8_t *)user_buf;

    while (copied < len && !is_eof_cluster(clus)) {
        if (bdev_read(sbi, cluster_to_byte(sbi, clus), clus_size, clus_buf) != 0)
            break;

        uint32_t offset_in_clus = (copied == 0) ? (uint32_t)skip : 0;
        uint32_t avail = clus_size - offset_in_clus;
        uint32_t to_copy = (len - copied < avail) ? (len - copied) : avail;

        memcpy(dst + copied, clus_buf + offset_in_clus, to_copy);
        copied += to_copy;

        clus = fat32_next_cluster(sbi, clus);
    }

    free(clus_buf);
    file->file_position += copied;
    return (ssize_t)copied;
}

static int fat32_readdir(file_t *file, struct vfs_dirent *out)
{
    fat32_inode_info *fi = file->node->private_data;
    fat32_sb_info  *sbi = fi->sbi;
    uint32_t clus_size = cluster_byte_size(sbi);
    uint8_t *buffer = malloc(clus_size);
    if (!buffer) return -ENOMEM;

    uint32_t clus = fi->first_cluster;
    for (size_t skip = file->file_position / clus_size; skip > 0 && !is_eof_cluster(clus); skip--)
        clus = fat32_next_cluster(sbi, clus);

    int result = 0;
    while (result == 0 && !is_eof_cluster(clus)) {
        if (bdev_read(sbi, cluster_to_byte(sbi, clus), clus_size, buffer) != 0) {
            result = -EIO;
            break;
        }

        size_t base = file->file_position - file->file_position % clus_size;
        uint32_t off = file->file_position % clus_size;
        while (off < clus_size) {
            const fat32_directory_entry *de =
                (const fat32_directory_entry *)(buffer + off);

            if (de->filename[0] == 0x00) {
                file->file_position = base + off;
                free(buffer);
                return 0;
            }
            if ((uint8_t)de->filename[0] == 0xE5 ||
                de->attributes == FAT_ATTR_VOLUME_ID) {
                off += sizeof(fat32_directory_entry);
                continue;
            }

            if ((de->attributes & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                uint32_t lfn_end = off;
                while (lfn_end < clus_size &&
                       ((const fat32_directory_entry *)(buffer + lfn_end))->attributes == FAT_ATTR_LFN)
                    lfn_end += sizeof(fat32_directory_entry);

                if (lfn_end >= clus_size) {
                    off = lfn_end;
                    continue;
                }
                off += fat32_extract_lfn(de, out->name, sizeof(out->name));
                de = (const fat32_directory_entry *)(buffer + off);
            } else {
                fat32_short_name(de, out->name, sizeof(out->name));
            }
            off += sizeof(fat32_directory_entry);

            out->ino  = ((uint32_t)de->cluster_high << 16) | de->cluster_low;
            out->type = (de->attributes & FAT_ATTR_DIRECTORY) ? INODE_DIR : INODE_FILE;
            result = 1;
            break;
        }
        file->file_position = base + off;
        if (result == 0)
            clus = fat32_next_cluster(sbi, clus);
    }

    free(buffer);
    return result;
}

static int fat32_file_open(inode_t *inode, file_t *file)
{
    return 0;
}

static int fat32_file_close(file_t *file)
{
    return 0;
}

static void fat32_release(inode_t *inode)
{
    fat32_inode_info *fi = inode->private_data;
    if (!fi->is_root)
        fat32_free_inode(inode);
}

static file_operations_t fat32_dir_fops = {
    .open    = fat32_file_open,
    .readdir = fat32_readdir,
    .close = fat32_file_close,
};

static file_operations_t fat32_file_fops = {
    .read  = fat32_file_read,
    .open  = fat32_file_open,
    .close = fat32_file_close,
};

static inode_operations_t fat32_dir_iops = {
    .lookup  = fat32_lookup,
    .release = fat32_release,
};

static inode_operations_t fat32_file_iops = {
    .release = fat32_release,
};

static inode_t *fat32_mount(void *blob)
{
    const auto bdev = (struct block_device *)blob;
    if (!bdev) {
        terminal_printf("fat32: no block device\n");
        return NULL;
    }

    fat32_boot_sector bs;
    if (bdev->ops->read_bytes(bdev, 0, sizeof(bs), &bs) != 0) {
        terminal_printf("fat32: failed to read boot sector\n");
        return NULL;
    }

    if (bs.bytes_per_sector == 0 ||
        (bs.bytes_per_sector & (bs.bytes_per_sector - 1)) != 0) {
        terminal_printf("fat32: invalid bytes_per_sector\n");
        return NULL;
    }

    fat32_fsinfo fsi = {0};
    bdev->ops->read_bytes(bdev, bs.bytes_per_sector, sizeof(fsi), &fsi);

    fat32_sb_info *sbi = malloc(sizeof(fat32_sb_info));
    if (!sbi) return NULL;
    memset(sbi, 0, sizeof(*sbi));

    sbi->bdev                = bdev;
    sbi->bs                  = bs;
    sbi->fsinfo              = fsi;
    sbi->bytes_per_sector    = bs.bytes_per_sector;
    sbi->sectors_per_cluster = bs.sectors_per_cluster;
    sbi->sectors_per_fat     = bs.sectors_per_fat32;
    sbi->root_cluster        = bs.root_cluster;
    sbi->fat_begin_lba       = bs.reserved_sectors;
    sbi->data_begin_lba      = bs.reserved_sectors +
                               bs.number_of_fats * bs.sectors_per_fat32;

    inode_t *root = fat32_make_inode(
        sbi, "/", sbi->root_cluster, 0, FAT_ATTR_DIRECTORY, 0, 0);
    if (!root) {
        free(sbi);
        return NULL;
    }
    ((fat32_inode_info *)root->private_data)->is_root = true;

    return root;
}

static void fat32_unmount(inode_t *root)
{
    if (!root) return;
    fat32_inode_info *fi = root->private_data;
    fat32_sb_info *sbi = fi->sbi;

    if (sbi) {
        free(sbi->bdev);
        free(sbi);
    }
    fat32_free_inode(root);
}

file_system_driver_t fat32_driver = {
    .mount   = fat32_mount,
    .unmount = fat32_unmount,
};

struct block_device *fat32_create_bdev(const uint32_t unique_id,
                                       const uint64_t part_offset_bytes,
                                       const uint64_t part_length_bytes,
                                       const struct block_device_ops *ops,
                                       void *private)
{
    struct block_device *bdev = malloc(sizeof(struct block_device));
    if (!bdev)
        return nullptr;
    bdev->unique_id   = unique_id;
    bdev->part_offset = part_offset_bytes;
    bdev->part_length = part_length_bytes;
    bdev->ops         = ops;
    bdev->private     = private;
    return bdev;
}
