#include "vfs_type.h"
#include "vfs.h"

#include <hardware/memory/alloc.h>
#include <hardware/terminal/stdio.h>
#include <utils/str/str.h>

/* ── filesystem type registry ─────────────────────────────────── */

#define MAX_FS_TYPES 16

static const struct file_system_type *fs_types[MAX_FS_TYPES];
static int num_fs_types = 0;

path vfs_root = {0};

int vfs_register_filesystem(const struct file_system_type *fs)
{
    if (!fs || !fs->name || num_fs_types >= MAX_FS_TYPES)
        return -1;
    fs_types[num_fs_types++] = fs;
    return 0;
}

static const struct file_system_type *find_fs_type(const char *name)
{
    for (int i = 0; i < num_fs_types; i++) {
        if (strcmp(fs_types[i]->name, name) == 0)
            return fs_types[i];
    }
    return NULL;
}

/* ── root mount accessor ──────────────────────────────────────── */

struct vfsmount *vfs_get_root_mount(void)
{
    return vfs_root.mnt;
}

/* ── mount root filesystem ────────────────────────────────────── */

int vfs_mount_root(const char *fs_type_name, const char *root_device)
{
    return vfs_mount_root_dev(fs_type_name, root_device, NULL);
}

int vfs_mount_root_dev(const char *fs_type_name, const char *dev_name, void *data)
{
    const struct file_system_type *fst = find_fs_type(fs_type_name);
    if (!fst) {
        printf("vfs: unknown filesystem type '%s'\n", fs_type_name);
        return -1;
    }

    struct super_block *sb = fst->mount(
        (struct file_system_type *)fst, 0, dev_name, data);
    if (!sb) {
        printf("vfs: mount failed for '%s'\n", fs_type_name);
        return -1;
    }

    struct vfsmount *mnt = malloc(sizeof(struct vfsmount));
    if (!mnt) return -1;
    mnt->sb = sb;

    vfs_root.mnt    = mnt;
    vfs_root.dentry = sb->s_root;

    return 0;
}

/* ── init ─────────────────────────────────────────────────────── */

int vfs_init(void)
{
    memset(&vfs_root, 0, sizeof(vfs_root));
    num_fs_types = 0;
    return 0;
}
