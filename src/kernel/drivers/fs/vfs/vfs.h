#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "../fs_utils.h"

typedef int gid_t;
typedef int uid_t;
typedef int dev_t;
typedef int ino_t;
typedef int mode_t;
typedef unsigned short nlink_t;
typedef unsigned long blksize_t;
typedef long off_t;
typedef long time_t;
typedef long clock_t;

typedef struct
{

} __attribute__((packed)) stat_t;

struct fs_node_s;

typedef int ssize_t;

typedef ssize_t (*read_type_t) (struct fs_node_s *,  off_t, size_t, uint8_t *);
typedef ssize_t (*write_type_t) (struct fs_node_s *, off_t, size_t, uint8_t *);
typedef void (*open_type_t) (struct fs_node_s *, unsigned int flags);
typedef void (*close_type_t) (struct fs_node_s *);
typedef struct dirent *(*readdir_type_t) (struct fs_node_s *, unsigned long);
typedef struct fs_node_s *(*finddir_type_t) (struct fs_node_s *, char *name);
typedef int (*create_type_t) (struct fs_node_s *, char *name, mode_t permission);
typedef int (*unlink_type_t) (struct fs_node_s *, char *name);
typedef int (*mkdir_type_t) (struct fs_node_s *, char *name, mode_t permission);
typedef int (*ioctl_type_t) (struct fs_node_s *, unsigned long request, void * argp);
typedef int (*get_size_type_t) (struct fs_node_s *);
typedef int (*chmod_type_t) (struct fs_node_s *, mode_t mode);
typedef int (*symlink_type_t) (struct fs_node_s *, char * name, char * value);
typedef ssize_t (*readlink_type_t) (struct fs_node_s *, char * buf, size_t size);
typedef int (*selectcheck_type_t) (struct fs_node_s *);
typedef int (*selectwait_type_t) (struct fs_node_s *, void * process);
typedef int (*chown_type_t) (struct fs_node_s *, uid_t, gid_t);
typedef int (*truncate_type_t) (struct fs_node_s *);

int vfs_mount(const fs_node* device, const char* mount_point);
int vfs_unmount(const char* mount_point);
int vfs_open(const char* path, int flags);
int vfs_close(int fd);
int vfs_read(int fd, void* buffer, size_t size);
int vfs_write(int fd, const void* buffer, size_t size);
int vfs_stat(const char* path, stat_t* buffer);


#endif //VFS_H
