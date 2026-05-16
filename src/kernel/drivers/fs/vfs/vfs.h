#ifndef VFS_H
#define VFS_H

#include <os/types.h>
#include <os/stddef.h>
#include <os/stdint.h>

struct file;
struct inode;
struct dentry;
struct super_block;
struct file_system_type;
struct vfsmount;

#define AT_FDCWD (-100)

int vfs_init(void);

int vfs_register_filesystem(const struct file_system_type *fs);

int vfs_mount_root(const char *fs_type_name, const char *root_device);
int vfs_mount_root_dev(const char *fs_type_name, const char *dev_name, void *data);

struct vfsmount *vfs_get_root_mount(void);

int     vfs_openat(int dirfd, const char *path, int flags, uint32_t mode);
int     vfs_close(int fd);

ssize_t vfs_read(int fd, void *buffer, size_t size);
ssize_t vfs_write(int fd, const void *buffer, size_t size);

int     vfs_fsync(int fd);
int     vfs_ftruncate(int fd, uint64_t length);

ssize_t vfs_getdents64(int fd, void *user_buf, size_t size);

int vfs_mkdirat(int dirfd, const char *path, uint32_t mode);
int vfs_unlinkat(int dirfd, const char *path, int flags);
int vfs_renameat2(int olddirfd, const char *oldpath,
                  int newdirfd, const char *newpath,
                  unsigned int flags);

int vfs_linkat(int olddirfd, const char *oldpath,
               int newdirfd, const char *newpath,
               unsigned int flags);

int vfs_symlinkat(const char *target, int newdirfd, const char *linkpath);
ssize_t vfs_readlinkat(int dirfd, const char *path, char *buf, size_t bufsz);

int vfs_open(const char *path, int flags, uint32_t mode);

#endif /* VFS_H */