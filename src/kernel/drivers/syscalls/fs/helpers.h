#ifndef OS_SYS_VFS_HELPERS_H
#define OS_SYS_VFS_HELPERS_H

#define AT_FDCWD (-100)

#include <os/errno.h>

#include <drivers/fs/vfs/vfs.h>
#include <drivers/syscalls/uaccess.h>
#include <drivers/scheduler/process.h>
#include <drivers/syscalls/fd.h>

static struct proc_files* files()
{
    return scheduler_get()->current->files;
}

static long resolve_at(const int dirfd, const char* upath, char* out, const bool allow_empty)
{
    char path[VFS_PATH_MAX];
    const long len = copy_string_from_user(path, upath, sizeof(path));
    if (len < 0)
        return len;
    if (len == 0) {
        if (!allow_empty)
            return -ENOENT;
        out[0] = '\0';
        return 0;
    }

    const char* base = files()->cwd;
    if (path[0] != '/' && dirfd != AT_FDCWD) {
        const struct file_s* dir = fd_get(dirfd);
        if (dir == nullptr)
            return -EBADF;
        if (dir->node->type != INODE_DIR)
            return -ENOTDIR;
        base = dir->filename;
    }
    return vfs_resolve_root(files()->root, base, path, out);
}

#endif //OS_SYS_VFS_HELPERS_H