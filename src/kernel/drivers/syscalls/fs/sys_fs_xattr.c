#include <drivers/syscalls/sys.h>

#define XATTR_NAME_MAX 255
#define XATTR_SIZE_MAX 65536
#define XATTR_LIST_MAX 65536

#include <os/errno.h>
#include <drivers/syscalls/uaccess.h>
#include <drivers/fs/vfs/vfs.h>
#include <hardware/memory/alloc.h>

#include "helpers.h"

static long xattr_name_in(const char* uname, char* name)
{
    const long len = copy_string_from_user(name, uname, XATTR_NAME_MAX + 1);
    if (len == -ENAMETOOLONG || len == 0)
        return -ERANGE;
    return len < 0 ? len : 0;
}

static long xattr_open(const char* upath, int* id)
{
    char path[VFS_PATH_MAX];
    const long err = resolve_at(AT_FDCWD, upath, path, false);
    if (err)
        return err;
    *id = vfs_open(path, 0);
    return *id < 0 ? *id : 0;
}

static long xattr_set(const struct file_s* f, const char* uname, const void* uvalue, const size_t size, const int flags)
{
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE))
        return -EINVAL;
    if (size > XATTR_SIZE_MAX)
        return -E2BIG;
    char name[XATTR_NAME_MAX + 1];
    long r = xattr_name_in(uname, name);
    if (r)
        return r;

    void* value = nullptr;
    if (size) {
        value = malloc(size);
        if (value == nullptr)
            return -ENOMEM;
        if (copy_from_user(value, uvalue, size) < 0) {
            free(value);
            return -EFAULT;
        }
    }
    r = vfs_setxattr(f, name, value, size, flags);
    free(value);
    return r;
}

static long xattr_get(const struct file_s* f, const char* uname, void* uvalue, size_t size)
{
    char name[XATTR_NAME_MAX + 1];
    long r = xattr_name_in(uname, name);
    if (r)
        return r;
    if (size == 0)
        return vfs_getxattr(f, name, nullptr, 0);
    if (size > XATTR_SIZE_MAX)
        size = XATTR_SIZE_MAX;

    void* value = malloc(size);
    if (value == nullptr)
        return -ENOMEM;
    r = vfs_getxattr(f, name, value, size);
    if (r > 0 && copy_to_user(uvalue, value, (size_t)r) < 0)
        r = -EFAULT;
    free(value);
    return r;
}

static long xattr_list(const struct file_s* f, char* ulist, size_t size)
{
    if (size == 0)
        return vfs_listxattr(f, nullptr, 0);
    if (size > XATTR_LIST_MAX)
        size = XATTR_LIST_MAX;

    char* list = malloc(size);
    if (list == nullptr)
        return -ENOMEM;
    long r = vfs_listxattr(f, list, size);
    if (r > 0 && copy_to_user(ulist, list, (size_t)r) < 0)
        r = -EFAULT;
    free(list);
    return r;
}

static long xattr_remove(const struct file_s* f, const char* uname)
{
    char name[XATTR_NAME_MAX + 1];
    const long r = xattr_name_in(uname, name);
    return r ? r : vfs_removexattr(f, name);
}

long sys_setxattr(const char* path, const char* name, const void* value, const size_t size, const int flags)
{
    int id;
    const long err = xattr_open(path, &id);
    if (err)
        return err;
    const long r = xattr_set(vfs_get_file(id), name, value, size, flags);
    vfs_close(id);
    return r;
}

long sys_lsetxattr(const char* path, const char* name, const void* value, const size_t size, const int flags)
{
    return sys_setxattr(path, name, value, size, flags);
}

long sys_fsetxattr(const int fd, const char* name, const void* value, const size_t size, const int flags)
{
    const struct file_s* f = fd_get(fd);
    return f ? xattr_set(f, name, value, size, flags) : -EBADF;
}

long sys_getxattr(const char* path, const char* name, void* value, const size_t size)
{
    int id;
    const long err = xattr_open(path, &id);
    if (err)
        return err;
    const long r = xattr_get(vfs_get_file(id), name, value, size);
    vfs_close(id);
    return r;
}

long sys_lgetxattr(const char* path, const char* name, void* value, const size_t size)
{
    return sys_getxattr(path, name, value, size);
}

long sys_fgetxattr(const int fd, const char* name, void* value, const size_t size)
{
    const struct file_s* f = fd_get(fd);
    return f ? xattr_get(f, name, value, size) : -EBADF;
}

long sys_listxattr(const char* path, char* list, const size_t size)
{
    int id;
    const long err = xattr_open(path, &id);
    if (err)
        return err;
    const long r = xattr_list(vfs_get_file(id), list, size);
    vfs_close(id);
    return r;
}

long sys_llistxattr(const char* path, char* list, const size_t size)
{
    return sys_listxattr(path, list, size);
}

long sys_flistxattr(const int fd, char* list, const size_t size)
{
    const struct file_s* f = fd_get(fd);
    return f ? xattr_list(f, list, size) : -EBADF;
}

long sys_removexattr(const char* path, const char* name)
{
    int id;
    const long err = xattr_open(path, &id);
    if (err)
        return err;
    const long r = xattr_remove(vfs_get_file(id), name);
    vfs_close(id);
    return r;
}

long sys_lremovexattr(const char* path, const char* name)
{
    return sys_removexattr(path, name);
}

long sys_fremovexattr(const int fd, const char* name)
{
    const struct file_s* f = fd_get(fd);
    return f ? xattr_remove(f, name) : -EBADF;
}
