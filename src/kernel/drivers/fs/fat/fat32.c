#include "fat32.h"

#include <hardware/memory/alloc.h>
#include <hardware/terminal/stdio.h>
#include <utils/str/str.h>

#include <drivers/fs/vfs/vfs.h>
#include <drivers/fs/vfs/vfs_type.h>

/* ── mode bits (Linux-compatible) ─────────────────────────────── */

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000

/* FAT attribute bits */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

/* ── forward declarations ─────────────────────────────────────── */

static const struct inode_operations fat32_dir_inode_ops;
static const struct inode_operations fat32_file_inode_ops;
static const struct file_operations  fat32_dir_fops;
static const struct file_operations  fat32_file_fops;
static const struct super_operations fat32_sops;

/* ── block device I/O helper ──────────────────────────────────── */

static int bdev_read(fat32_sb_info *sbi, uint64_t off, uint32_t len, void *buf)
{
    return sbi->bdev->ops->read_bytes(sbi->bdev, off, len, buf);
}

/* ── cluster / FAT arithmetic ─────────────────────────────────── */

static uint64_t cluster_to_byte(fat32_sb_info *sbi, uint32_t cluster)
{
    return (uint64_t)(sbi->data_begin_lba +
           (cluster - 2) * sbi->sectors_per_cluster) * sbi->bytes_per_sector;
}

static uint32_t cluster_byte_size(fat32_sb_info *sbi)
{
    return sbi->sectors_per_cluster * sbi->bytes_per_sector;
}

static uint32_t fat32_next_cluster(fat32_sb_info *sbi, uint32_t cluster)
{
    uint64_t fat_off = (uint64_t)sbi->fat_begin_lba * sbi->bytes_per_sector
                     + cluster * 4;
    uint32_t next = 0;
    if (bdev_read(sbi, fat_off, 4, &next) != 0)
        return 0x0FFFFFFF;
    return next & 0x0FFFFFFF;
}

static inline int is_eof_cluster(uint32_t c) { return c >= 0x0FFFFFF8; }

/* ── LFN extraction (kept from original) ─────────────────────── */

static uint32_t fat32_extract_lfn(const fat32_directory_entry *entry,
                                  char *long_filename, uint32_t buffer_size)
{
    const fat32_long_filename_entry *lfn_entries =
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
        const fat32_long_filename_entry *e =
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

/* Build a short filename from an 8.3 directory entry */
static void fat32_short_name(const fat32_directory_entry *de,
                             char *out, uint32_t out_size)
{
    int len = 0;

    /* Copy base name, trimming trailing spaces */
    for (int i = 0; i < 8 && len < (int)out_size - 1; i++) {
        if (de->filename[i] == ' ') break;
        out[len++] = de->filename[i];
    }

    /* Directories don't get an extension dot */
    if (!(de->attributes & FAT_ATTR_DIRECTORY)) {
        int ext_len = 0;
        for (int i = 0; i < 3; i++) {
            if (de->extension[i] != ' ') ext_len = i + 1;
        }
        if (ext_len > 0 && len < (int)out_size - 1) {
            out[len++] = '.';
            for (int i = 0; i < ext_len && len < (int)out_size - 1; i++)
                out[len++] = de->extension[i];
        }
    }

    out[len] = '\0';
}

/* ── inode creation ───────────────────────────────────────────── */

static ino_t fat32_ino_from_cluster(uint32_t cluster)
{
    return (ino_t)cluster;
}

static struct inode *fat32_make_inode(struct super_block *sb,
                                     uint32_t cluster,
                                     uint32_t size,
                                     uint8_t attr,
                                     uint32_t parent_cluster,
                                     uint32_t dirent_off)
{
    struct inode *inode = malloc(sizeof(struct inode));
    if (!inode) return NULL;
    memset(inode, 0, sizeof(*inode));

    fat32_inode_info *fi = malloc(sizeof(fat32_inode_info));
    if (!fi) { free(inode); return NULL; }

    fi->first_cluster     = cluster;
    fi->size              = size;
    fi->attr              = attr;
    fi->parent_dir_cluster = parent_cluster;
    fi->dirent_offset     = dirent_off;

    inode->i_ino     = fat32_ino_from_cluster(cluster);
    inode->i_sb      = sb;
    inode->i_size    = size;
    inode->i_private = fi;

    if (attr & FAT_ATTR_DIRECTORY) {
        inode->i_mode = S_IFDIR | 0755;
        inode->i_op   = &fat32_dir_inode_ops;
        inode->i_fop  = &fat32_dir_fops;
    } else {
        inode->i_mode = S_IFREG | 0644;
        inode->i_op   = &fat32_file_inode_ops;
        inode->i_fop  = &fat32_file_fops;
    }

    return inode;
}

/* ── directory iteration (VFS callback) ───────────────────────── */

static int fat32_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct inode *dir_inode = file->f_path.dentry->inode;
    fat32_sb_info  *sbi = dir_inode->i_sb->s_fs_info;
    fat32_inode_info *fi = dir_inode->i_private;

    uint32_t clus = fi->first_cluster;
    uint32_t clus_size = cluster_byte_size(sbi);
    uint8_t *buffer = malloc(clus_size);
    if (!buffer) return -1;

    u64 pos = 0;  /* linear position across all clusters */

    while (!is_eof_cluster(clus)) {
        if (bdev_read(sbi, cluster_to_byte(sbi, clus), clus_size, buffer) != 0) {
            free(buffer);
            return -1;
        }

        uint32_t off = 0;
        while (off < clus_size) {
            const fat32_directory_entry *de =
                (const fat32_directory_entry *)(buffer + off);

            if (de->filename[0] == 0x00) goto out;       /* end of dir */
            if ((uint8_t)de->filename[0] == 0xE5) {      /* deleted */
                off += sizeof(fat32_directory_entry);
                pos++;
                continue;
            }
            if (de->attributes == FAT_ATTR_VOLUME_ID) {  /* volume label */
                off += sizeof(fat32_directory_entry);
                pos++;
                continue;
            }

            char name[256] = {0};
            bool has_lfn = false;

            if ((de->attributes & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                has_lfn = true;
                uint32_t lfn_bytes = fat32_extract_lfn(de, name, sizeof(name));
                off += lfn_bytes;
                de = (const fat32_directory_entry *)(buffer + off);
            }

            if (!has_lfn)
                fat32_short_name(de, name, sizeof(name));

            /* Skip . and .. */
            if (name[0] == '.' && (name[1] == '\0' ||
                (name[1] == '.' && name[2] == '\0'))) {
                off += sizeof(fat32_directory_entry);
                pos++;
                continue;
            }

            uint32_t entry_cluster = ((uint32_t)de->cluster_high << 16)
                                   | de->cluster_low;

            /* Only emit entries at or past ctx->pos */
            if (pos >= ctx->pos) {
                unsigned dtype = (de->attributes & FAT_ATTR_DIRECTORY)
                               ? VFS_DT_DIR : VFS_DT_REG;
                if (!ctx->actor(ctx, name, strlen(name),
                                fat32_ino_from_cluster(entry_cluster), dtype)) {
                    free(buffer);
                    return 0;  /* caller's buffer full */
                }
                ctx->pos = pos + 1;
            }

            off += sizeof(fat32_directory_entry);
            pos++;
        }

        clus = fat32_next_cluster(sbi, clus);
    }

out:
    free(buffer);
    return 0;
}

/* ── lookup (find a name inside a directory) ──────────────────── */

static struct dentry *fat32_lookup(struct inode *dir, struct dentry *dentry)
{
    fat32_sb_info  *sbi = dir->i_sb->s_fs_info;
    fat32_inode_info *fi = dir->i_private;

    uint32_t clus = fi->first_cluster;
    uint32_t clus_size = cluster_byte_size(sbi);
    uint8_t *buffer = malloc(clus_size);
    if (!buffer) return NULL;

    const char *target = dentry->name.name;
    uint32_t    target_len = dentry->name.len;

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
                uint32_t lfn_bytes = fat32_extract_lfn(de, name, sizeof(name));
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

            /* Found it */
            uint32_t entry_cluster = ((uint32_t)de->cluster_high << 16)
                                   | de->cluster_low;
            struct inode *inode = fat32_make_inode(
                dir->i_sb, entry_cluster, de->file_size, de->attributes,
                fi->first_cluster, dirent_off);

            dentry->inode = inode;
            free(buffer);
            return dentry;
        }

        clus = fat32_next_cluster(sbi, clus);
    }

not_found:
    free(buffer);
    dentry->inode = NULL;
    return NULL;
}

/* ── file read (follows FAT chain) ────────────────────────────── */

static ssize_t fat32_file_read(struct file *file, void *user_buf,
                               size_t len, loff_t *pos)
{
    struct inode *inode = file->f_path.dentry->inode;
    fat32_sb_info  *sbi = inode->i_sb->s_fs_info;
    fat32_inode_info *fi = inode->i_private;

    if (*pos >= (loff_t)fi->size)
        return 0;
    if (*pos + (loff_t)len > (loff_t)fi->size)
        len = fi->size - *pos;
    if (len == 0)
        return 0;

    uint32_t clus_size = cluster_byte_size(sbi);
    uint8_t *clus_buf = malloc(clus_size);
    if (!clus_buf) return -1;

    /* Walk the FAT chain to the cluster containing *pos */
    uint32_t clus = fi->first_cluster;
    uint64_t skip = (uint64_t)*pos;

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
    *pos += copied;
    return (ssize_t)copied;
}

/* ── file open / release (no-op for FAT32) ────────────────────── */

static int fat32_file_open(struct inode *inode, struct file *file)
{
    file->private_data = NULL;
    return 0;
}

static int fat32_file_release(struct inode *inode, struct file *file)
{
    return 0;
}

/* ── operations tables ────────────────────────────────────────── */

static const struct file_operations fat32_dir_fops = {
    .iterate_shared = fat32_iterate_shared,
    .open           = fat32_file_open,
    .release        = fat32_file_release,
};

static const struct file_operations fat32_file_fops = {
    .read    = fat32_file_read,
    .open    = fat32_file_open,
    .release = fat32_file_release,
};

static const struct inode_operations fat32_dir_inode_ops = {
    .lookup = fat32_lookup,
};

static const struct inode_operations fat32_file_inode_ops = {
    /* no special ops for regular files yet */
};

/* ── super_block operations ───────────────────────────────────── */

static void fat32_put_super(struct super_block *sb)
{
    fat32_sb_info *sbi = sb->s_fs_info;
    if (sbi) {
        if (sbi->bdev)
            free(sbi->bdev);
        free(sbi);
    }
    sb->s_fs_info = NULL;
}

static struct inode *fat32_sops_alloc_inode(struct super_block *sb)
{
    struct inode *inode = malloc(sizeof(struct inode));
    if (inode) memset(inode, 0, sizeof(*inode));
    return inode;
}

static void fat32_sops_destroy_inode(struct inode *inode)
{
    if (inode->i_private)
        free(inode->i_private);
    free(inode);
}

static const struct super_operations fat32_sops = {
    .put_super     = fat32_put_super,
    .alloc_inode   = fat32_sops_alloc_inode,
    .destroy_inode = fat32_sops_destroy_inode,
};

/* ── mount ────────────────────────────────────────────────────── */

struct super_block *fat32_mount(struct file_system_type *fs_type,
                                uint32_t mount_flags,
                                const char *dev_name,
                                void *data)
{
    /*
     * Caller passes a struct block_device* via `data`.
     * In the future this could be looked up by dev_name instead.
     */
    struct block_device *bdev = (struct block_device *)data;
    if (!bdev) {
        printf("fat32: no block device\n");
        return NULL;
    }

    /* Read boot sector */
    fat32_boot_sector bs;
    if (bdev->ops->read_bytes(bdev, 0, sizeof(bs), &bs) != 0) {
        printf("fat32: failed to read boot sector\n");
        return NULL;
    }

    if (bs.bytes_per_sector == 0 ||
        (bs.bytes_per_sector & (bs.bytes_per_sector - 1)) != 0) {
        printf("fat32: invalid bytes_per_sector\n");
        return NULL;
    }

    /* Read FSINFO */
    fat32_fsinfo fsi;
    memset(&fsi, 0, sizeof(fsi));
    bdev->ops->read_bytes(bdev, bs.bytes_per_sector, sizeof(fsi), &fsi);

    /* Allocate sb_info */
    fat32_sb_info *sbi = malloc(sizeof(fat32_sb_info));
    if (!sbi) return NULL;
    memset(sbi, 0, sizeof(*sbi));

    sbi->bdev               = bdev;
    sbi->bs                 = bs;
    sbi->fsinfo             = fsi;
    sbi->bytes_per_sector   = bs.bytes_per_sector;
    sbi->sectors_per_cluster = bs.sectors_per_cluster;
    sbi->sectors_per_fat    = bs.sectors_per_fat32;
    sbi->root_cluster       = bs.root_cluster;
    sbi->fat_begin_lba      = bs.reserved_sectors;
    sbi->data_begin_lba     = bs.reserved_sectors +
                              bs.number_of_fats * bs.sectors_per_fat32;

    /* Allocate super_block */
    struct super_block *sb = malloc(sizeof(struct super_block));
    if (!sb) { free(sbi); return NULL; }
    memset(sb, 0, sizeof(*sb));

    sb->s_magic     = 0x46415433; /* "FAT3" */
    sb->s_blocksize = cluster_byte_size(sbi);
    sb->s_op        = &fat32_sops;
    sb->s_fs_info   = sbi;

    /* Root inode */
    struct inode *root_inode = fat32_make_inode(
        sb, sbi->root_cluster, 0, FAT_ATTR_DIRECTORY, 0, 0);
    if (!root_inode) {
        free(sb);
        free(sbi);
        return NULL;
    }

    /* Root dentry */
    struct dentry *root_dentry = malloc(sizeof(struct dentry));
    if (!root_dentry) {
        free(root_inode->i_private);
        free(root_inode);
        free(sb);
        free(sbi);
        return NULL;
    }
    memset(root_dentry, 0, sizeof(*root_dentry));
    root_dentry->name.name = "/";
    root_dentry->name.len  = 1;
    root_dentry->inode     = root_inode;
    root_dentry->parent    = root_dentry;
    root_dentry->refcnt    = 1;

    sb->s_root = root_dentry;

    return sb;
}

/* ── kill_sb ──────────────────────────────────────────────────── */

void fat32_kill_sb(struct super_block *sb)
{
    if (!sb) return;
    if (sb->s_op && sb->s_op->put_super)
        sb->s_op->put_super(sb);
    /* TODO: walk and free dentry/inode tree */
    free(sb);
}

/* ── file_system_type registration ────────────────────────────── */

struct file_system_type fat32_fs_type = {
    .name    = "fat32",
    .mount   = fat32_mount,
    .kill_sb = fat32_kill_sb,
};

/* ── block_device helper ──────────────────────────────────────── */

struct block_device *fat32_create_bdev(uint32_t unique_id,
                                       uint64_t part_offset_bytes,
                                       uint64_t part_length_bytes,
                                       const struct block_device_ops *ops,
                                       void *private)
{
    struct block_device *bdev = malloc(sizeof(struct block_device));
    if (!bdev) return NULL;
    bdev->unique_id   = unique_id;
    bdev->part_offset = part_offset_bytes;
    bdev->part_length = part_length_bytes;
    bdev->ops         = ops;
    bdev->private     = private;
    return bdev;
}
