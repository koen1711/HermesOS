#ifndef VFS_TYPE_H
#define VFS_TYPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int64_t   loff_t;
typedef uint64_t  u64;
typedef uint32_t  u32;
typedef uint16_t  u16;
typedef uint8_t   u8;
typedef int64_t   s64;
typedef int32_t   s32;

typedef long      ssize_t;
typedef u32       umode_t;

typedef u32 kuid_t;
typedef u32 kgid_t;

typedef u64 dev_t;

typedef u64 ino_t;

typedef s64 time64_t;

struct timespec64 {
    time64_t tv_sec;
    long     tv_nsec;
};

struct super_block;
struct inode;
struct dentry;
struct file;
struct path;
struct dir_context;

struct qstr {
    const char *name;
    u32 len;
    u32 hash;
};

struct vfsmount {
    struct super_block *sb;
};

typedef struct {
    struct vfsmount *mnt;
    struct dentry   *dentry;
} path;

typedef bool (*dir_emit_t)(struct dir_context *ctx,
                           const char *name, int namelen,
                           u64 ino, unsigned type);

struct dir_context {
    dir_emit_t actor;
    u64 pos;
};

enum vfs_dtype {
    VFS_DT_UNKNOWN = 0,
    VFS_DT_FIFO    = 1,
    VFS_DT_CHR     = 2,
    VFS_DT_DIR     = 4,
    VFS_DT_BLK     = 6,
    VFS_DT_REG     = 8,
    VFS_DT_LNK     = 10,
    VFS_DT_SOCK    = 12,
    VFS_DT_WHT     = 14,
};

struct file_operations {
    loff_t  (*llseek)(struct file *file, loff_t off, int whence);

    ssize_t (*read)(struct file *file, void *user_buf, size_t len, loff_t *pos);
    ssize_t (*write)(struct file *file, const void *user_buf, size_t len, loff_t *pos);

    int     (*iterate_shared)(struct file *file, struct dir_context *ctx);

    int     (*open)(struct inode *inode, struct file *file);
    int     (*release)(struct inode *inode, struct file *file);

    long    (*unlocked_ioctl)(struct file *file, unsigned int cmd, unsigned long arg);
};

struct inode_operations {
    struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry);

    /* Creation / mutation (implement as needed) */
    int (*create)(struct inode *dir, struct dentry *dentry, umode_t mode);
    int (*mkdir)(struct inode *dir, struct dentry *dentry, umode_t mode);
    int (*unlink)(struct inode *dir, struct dentry *dentry);
    int (*rmdir)(struct inode *dir, struct dentry *dentry);
    int (*rename)(struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry,
                  unsigned int flags);

    int (*link)(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry);
    int (*symlink)(struct inode *dir, struct dentry *dentry, const char *target);

    ssize_t (*readlink)(struct dentry *dentry, char *user_buf, size_t bufsz);
    const char *(*get_link)(struct dentry *dentry);
};

struct super_operations {
    void (*put_super)(struct super_block *sb);
    struct inode *(*alloc_inode)(struct super_block *sb);
    void (*destroy_inode)(struct inode *inode);
    int (*sync_fs)(struct super_block *sb, int wait);
};

struct inode {
    ino_t   i_ino;
    umode_t i_mode;
    kuid_t  i_uid;
    kgid_t  i_gid;
    u64     i_nlink;
    u64     i_size;
    u64     i_blocks;
    dev_t   i_rdev;

    struct timespec64 i_atime;
    struct timespec64 i_mtime;
    struct timespec64 i_ctime;

    struct super_block *i_sb;

    const struct inode_operations *i_op;
    const struct file_operations  *i_fop; /* default fops */

    void *i_private;
};

struct dentry {
    struct qstr name;
    struct dentry *parent;
    struct inode  *inode;

    u32 refcnt;

    void *d_private;
};

struct file {
    path f_path;

    const struct file_operations *f_op;

    loff_t f_pos;
    u32    f_flags;

    void  *private_data;
};

struct super_block {
    u64 s_magic;
    u32 s_blocksize;

    const struct super_operations *s_op;

    struct dentry *s_root;
    void *s_fs_info;
};

struct file_system_type {
    const char *name;

    struct super_block *(*mount)(struct file_system_type *fs_type,
                                 u32 mount_flags,
                                 const char *dev_name,
                                 void *data);

    void (*kill_sb)(struct super_block *sb);
};

static inline struct inode *d_inode(const struct dentry *d) { return d ? d->inode : NULL; }
static inline struct super_block *inode_sb(const struct inode *i) { return i ? i->i_sb : NULL; }

#endif /* VFS_TYPE_H */