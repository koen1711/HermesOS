#include "sys.h"

#include "fd.h"
#include "uaccess.h"
#include <drivers/fs/vfs/vfs.h>
#include <drivers/fs/pipe/pipe.h>
#include <drivers/net/socket/socket_file.h>
#include <drivers/scheduler/process.h>
#include <drivers/timer/clock.h>
#include <hardware/memory/alloc.h>
#include <utils/str/str.h>
#include <os/errno.h>

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EMPTY_PATH 0x1000
#define FD_CLOEXEC 1
#define IOV_MAX 1024
#define RENAME_NOREPLACE 1
#define COPY_CHUNK (64 * 1024)
#define SPLICE_F_ALL 0xF
#define MFD_CLOEXEC 0x1
#define MFD_ALLOW_SEALING 0x2
#define MFD_NAME_MAX 249
#define CLOSE_RANGE_UNSHARE 0x2
#define CLOSE_RANGE_CLOEXEC 0x4

enum linux_open_flags {
    LO_ACCMODE = 0x3,
    LO_WRONLY = 0x1,
    LO_RDWR = 0x2,
    LO_CREAT = 0x40,
    LO_EXCL = 0x80,
    LO_TRUNC = 0x200,
    LO_APPEND = 0x400,
    LO_NONBLOCK = 0x800,
    LO_DIRECTORY = 0x10000,
    LO_CLOEXEC = 0x80000,
    LO_PATH = 0x200000
};

enum fcntl_cmd {
    F_DUPFD = 0,
    F_GETFD = 1,
    F_SETFD = 2,
    F_GETFL = 3,
    F_SETFL = 4,
    F_GETLK = 5,
    F_SETLK = 6,
    F_SETLKW = 7,
    F_DUPFD_CLOEXEC = 1030,
    F_SETPIPE_SZ = 1031,
    F_GETPIPE_SZ = 1032
};

enum ioctl_generic {
    FIONBIO = 0x5421,
    FIONCLEX = 0x5450,
    FIOCLEX = 0x5451
};

enum stat_mode {
    S_IFIFO = 0010000,
    S_IFCHR = 0020000,
    S_IFDIR = 0040000,
    S_IFBLK = 0060000,
    S_IFREG = 0100000,
    S_IFSOCK = 0140000
};

enum dirent_type {
    DT_FIFO = 1,
    DT_CHR = 2,
    DT_DIR = 4,
    DT_REG = 8,
    DT_SOCK = 12
};

struct stat_k {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    struct timespec_k st_atim;
    struct timespec_k st_mtim;
    struct timespec_k st_ctim;
    int64_t reserved[3];
};

struct statfs_k {
    int64_t f_type;
    int64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    int64_t f_namelen;
    int64_t f_frsize;
    int64_t f_flags;
    int64_t f_spare[4];
};

struct dirent64_k {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} __attribute__((packed));

struct iovec_k {
    void* iov_base;
    size_t iov_len;
};

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

static int vfs_access_flags(const int flags)
{
    switch (flags & LO_ACCMODE) {
    case LO_WRONLY:
        return O_WRONLY;
    case LO_RDWR:
        return O_RDONLY | O_WRONLY;
    default:
        return O_RDONLY;
    }
}

long sys_read(const int fd, void* buf, const size_t len)
{
    const struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    if (!uaccess_ok_write(buf, len))
        return -EFAULT;
    return vfs_read((int)f->id, buf, len);
}

long sys_write(const int fd, const void* buf, const size_t len)
{
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    if (!uaccess_ok(buf, len))
        return -EFAULT;
    if ((f->status_flags & LO_APPEND) && f->node->type == INODE_FILE)
        f->file_position = f->node->size;
    return vfs_write((int)f->id, buf, len);
}

long sys_openat(const int dirfd, const char* upath, const int flags, const uint32_t mode)
{
    (void)mode;
    char path[VFS_PATH_MAX];
    long err = resolve_at(dirfd, upath, path, false);
    if (err)
        return err;
    if ((flags & LO_ACCMODE) == LO_ACCMODE)
        return -EINVAL;

    const bool writing = (flags & LO_ACCMODE) != 0;
    const int access = flags & LO_PATH ? 0 : vfs_access_flags(flags);
    int id = vfs_open(path, access);
    bool created = false;
    if (id == -ENOENT && (flags & LO_CREAT) && !(flags & LO_DIRECTORY)) {
        err = vfs_create(path, false);
        if (err && err != -EEXIST)
            return err;
        created = err == 0;
        err = 0;
        id = vfs_open(path, access);
    }
    if (id < 0)
        return id;

    struct file_s* f = vfs_get_file(id);
    if ((flags & LO_CREAT) && (flags & LO_EXCL) && !created)
        err = -EEXIST;
    else if ((flags & LO_DIRECTORY) && f->node->type != INODE_DIR)
        err = -ENOTDIR;
    else if (writing && f->node->type == INODE_DIR)
        err = -EISDIR;
    else if ((flags & LO_TRUNC) && writing && f->node->type == INODE_FILE && !created)
        err = vfs_truncate(id, 0);
    if (err) {
        vfs_close(id);
        return err;
    }

    f->status_flags = flags & (LO_APPEND | LO_NONBLOCK);
    const int fd = fd_install_from(f, 0, flags & LO_CLOEXEC);
    if (fd < 0)
        vfs_close(id);
    return fd;
}

long sys_close(const int fd)
{
    return fd_close(fd);
}

static long iov_in(const void* uiov, const int iov_cnt, struct iovec_k** out)
{
    *out = nullptr;
    if (iov_cnt < 0 || iov_cnt > IOV_MAX)
        return -EINVAL;
    if (iov_cnt == 0)
        return 0;
    struct iovec_k* iov = malloc(iov_cnt * sizeof(struct iovec_k));
    if (iov == nullptr)
        return -ENOMEM;
    if (copy_from_user(iov, uiov, iov_cnt * sizeof(struct iovec_k)) < 0) {
        free(iov);
        return -EFAULT;
    }
    *out = iov;
    return 0;
}

static long rw_vector(const int fd, const void* uiov, const int iov_cnt, const int64_t offset, const bool write)
{
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    if (offset >= 0 && f->node->type != INODE_FILE)
        return -ESPIPE;
    struct iovec_k* iov;
    const long err = iov_in(uiov, iov_cnt, &iov);
    if (err)
        return err;

    const size_t saved = f->file_position;
    if (offset >= 0)
        f->file_position = (size_t)offset;
    long total = 0;
    for (int i = 0; i < iov_cnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        const long n = write ? sys_write(fd, iov[i].iov_base, iov[i].iov_len)
                             : sys_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            if (total == 0)
                total = n;
            break;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;
    }
    if (offset >= 0)
        f->file_position = saved;
    free(iov);
    return total;
}

long sys_readv(const int fd, const void* iov, const int iov_cnt)
{
    return rw_vector(fd, iov, iov_cnt, -1, false);
}

long sys_writev(const int fd, const void* iov, const int iov_cnt)
{
    return rw_vector(fd, iov, iov_cnt, -1, true);
}

long sys_preadv(const int fd, const void* iov, const int iov_cnt, const int64_t offset)
{
    return offset < 0 ? -EINVAL : rw_vector(fd, iov, iov_cnt, offset, false);
}

long sys_pwritev(const int fd, const void* iov, const int iov_cnt, const int64_t offset)
{
    return offset < 0 ? -EINVAL : rw_vector(fd, iov, iov_cnt, offset, true);
}

long sys_preadv2(const int fd, const void* iov, const int iov_cnt, const int64_t offset, const int flags)
{
    if (flags != 0)
        return -EOPNOTSUPP;
    return offset < -1 ? -EINVAL : rw_vector(fd, iov, iov_cnt, offset, false);
}

long sys_pwritev2(const int fd, const void* iov, const int iov_cnt, const int64_t offset, const int flags)
{
    if (flags != 0)
        return -EOPNOTSUPP;
    return offset < -1 ? -EINVAL : rw_vector(fd, iov, iov_cnt, offset, true);
}

static long rw_at(const int fd, void* buf, const size_t len, const int64_t offset, const bool write)
{
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    if (f->node->type != INODE_FILE)
        return -ESPIPE;
    if (offset < 0)
        return -EINVAL;
    const size_t saved = f->file_position;
    f->file_position = (size_t)offset;
    const long n = write ? sys_write(fd, buf, len) : sys_read(fd, buf, len);
    f->file_position = saved;
    return n;
}

long sys_pread64(const int fd, void* buf, const size_t len, const int64_t offset)
{
    return rw_at(fd, buf, len, offset, false);
}

long sys_pwrite64(const int fd, const void* buf, const size_t len, const int64_t offset)
{
    return rw_at(fd, (void*)buf, len, offset, true);
}

long sys_lseek(const int fd, const int64_t offset, const int whence)
{
    const struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    return vfs_seek((int)f->id, offset, (enum seek_origin)whence);
}

static long stat_out(const struct file_s* f, void* ust)
{
    vfs_stat_t vs;
    if (vfs_stat((int)f->id, &vs) < 0)
        return -EBADF;

    struct stat_k st = {0};

    st.st_dev = 1;
    st.st_ino = vs.ino ? vs.ino : f->id + 1;
    st.st_nlink = vs.type == INODE_DIR ? 2 : 1;
    st.st_size = (int64_t)vs.size;
    st.st_blksize = PAGE_SIZE;
    st.st_blocks = (int64_t)((vs.size + 511) / 512);
    st.st_mtim.tv_sec = (int64_t)vs.mtime;
    st.st_atim = st.st_mtim;
    st.st_ctim = st.st_mtim;

    switch (vs.type) {
    case INODE_DIR:
        st.st_mode = S_IFDIR | 0755;
        break;
    case INODE_SOCKET:
        st.st_mode = S_IFSOCK | 0777;
        break;
    case INODE_PIPE:
        st.st_mode = S_IFIFO | 0600;
        break;
    case INODE_CHAR:
        st.st_mode = S_IFCHR | 0620;
        st.st_rdev = (5 << 8) | 1;
        break;
    default:
        st.st_mode = S_IFREG | 0755;
        break;
    }
    return copy_to_user(ust, &st, sizeof(st));
}

long sys_fstat(const int fd, void* st)
{
    const struct file_s* f = fd_get(fd);
    return f ? stat_out(f, st) : -EBADF;
}

long sys_newfstatat(const int dirfd, const char* upath, void* st, const int flags)
{
    char path[VFS_PATH_MAX];
    const long err = resolve_at(dirfd, upath, path, flags & AT_EMPTY_PATH);
    if (err)
        return err;
    if (path[0] == '\0') {
        if (dirfd != AT_FDCWD)
            return sys_fstat(dirfd, st);
        strncpy(path, files()->cwd, sizeof(path) - 1);
    }

    const int id = vfs_open(path, 0);
    if (id < 0)
        return id;
    const long r = stat_out(vfs_get_file(id), st);
    vfs_close(id);
    return r;
}

static long statfs_out(const struct file_s* f, void* ubuf)
{
    struct vfs_statfs vs = {.block_size = PAGE_SIZE, .name_max = MAX_FILE_PATH - 1};
    switch (f->node->type) {
    case INODE_PIPE:
        vs.magic = 0x50495045;
        break;
    case INODE_SOCKET:
        vs.magic = 0x534f434b;
        break;
    default: {
        const int err = vfs_statfs(f, &vs);
        if (err == -ENOSYS)
            vs.magic = 0x09041934;
        else if (err < 0)
            return err;
        break;
    }
    }

    struct statfs_k s = {0};
    s.f_type = (int64_t)vs.magic;
    s.f_bsize = (int64_t)vs.block_size;
    s.f_frsize = (int64_t)vs.block_size;
    s.f_blocks = vs.total_blocks;
    s.f_bfree = vs.free_blocks;
    s.f_bavail = vs.free_blocks;
    s.f_files = vs.total_files;
    s.f_ffree = vs.free_files;
    s.f_namelen = (int64_t)vs.name_max;
    return copy_to_user(ubuf, &s, sizeof(s));
}

long sys_statfs(const char* upath, void* buf)
{
    char path[VFS_PATH_MAX];
    const long err = resolve_at(AT_FDCWD, upath, path, false);
    if (err)
        return err;

    const int id = vfs_open(path, 0);
    if (id < 0)
        return id;
    const long r = statfs_out(vfs_get_file(id), buf);
    vfs_close(id);
    return r;
}

long sys_fstatfs(const int fd, void* buf)
{
    const struct file_s* f = fd_get(fd);
    return f ? statfs_out(f, buf) : -EBADF;
}

static uint8_t dirent_type(const size_t type)
{
    switch (type) {
    case INODE_DIR:
        return DT_DIR;
    case INODE_SOCKET:
        return DT_SOCK;
    case INODE_PIPE:
        return DT_FIFO;
    case INODE_CHAR:
        return DT_CHR;
    default:
        return DT_REG;
    }
}

long sys_getdents64(const int fd, void* dirp, const size_t count)
{
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    if (f->node->type != INODE_DIR)
        return -ENOTDIR;

    struct vfs_dirent* ent = malloc(sizeof(struct vfs_dirent));
    if (ent == nullptr)
        return -ENOMEM;
    uint8_t* rec = malloc(sizeof(struct dirent64_k) + MAX_FILE_PATH + 8);
    if (rec == nullptr) {
        free(ent);
        return -ENOMEM;
    }

    size_t written = 0;
    long err = 0;
    for (;;) {
        const size_t pos = f->file_position;
        const int r = vfs_readdir((int)f->id, ent);
        if (r <= 0) {
            err = r;
            break;
        }
        const size_t name_len = strlen(ent->name);
        const size_t reclen = (sizeof(struct dirent64_k) + name_len + 1 + 7) & ~(size_t)7;
        if (written + reclen > count) {
            f->file_position = pos;
            if (written == 0)
                err = -EINVAL;
            break;
        }
        memset(rec, 0, reclen);
        struct dirent64_k* d = (struct dirent64_k*)rec;
        d->d_ino = ent->ino ? ent->ino : 1;
        d->d_off = (int64_t)f->file_position;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = dirent_type(ent->type);
        memcpy(d->d_name, ent->name, name_len + 1);
        if (copy_to_user((uint8_t*)dirp + written, rec, reclen) < 0) {
            f->file_position = pos;
            err = -EFAULT;
            break;
        }
        written += reclen;
    }
    free(rec);
    free(ent);
    return written ? (long)written : err;
}

long sys_getcwd(char* buf, const size_t size)
{
    const char* cwd = vfs_strip_root(files()->root, files()->cwd);
    if (cwd == nullptr)
        cwd = "/";
    const size_t len = strlen(cwd) + 1;
    if (size < len)
        return -ERANGE;
    if (copy_to_user(buf, cwd, len) < 0)
        return -EFAULT;
    return (long)len;
}

static long set_cwd(const char* path)
{
    const int id = vfs_open(path, 0);
    if (id < 0)
        return id;
    const bool dir = vfs_get_file(id)->node->type == INODE_DIR;
    vfs_close(id);
    if (!dir)
        return -ENOTDIR;
    strncpy(files()->cwd, path, VFS_PATH_MAX - 1);
    return 0;
}

long sys_chdir(const char* upath)
{
    char path[VFS_PATH_MAX];
    const long err = resolve_at(AT_FDCWD, upath, path, false);
    return err ? err : set_cwd(path);
}

long sys_fchdir(const int fd)
{
    const struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    if (f->node->type != INODE_DIR)
        return -ENOTDIR;
    return set_cwd(f->filename);
}

static long dup_from(const int fd, const int min_fd, const bool cloexec)
{
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    f->ref_count++;
    const int nfd = fd_install_from(f, min_fd, cloexec);
    if (nfd < 0)
        f->ref_count--;
    return nfd;
}

long sys_dup(const int fd)
{
    return dup_from(fd, 0, false);
}

long sys_dup3(const int oldfd, const int newfd, const int flags)
{
    if (oldfd == newfd || (flags & ~LO_CLOEXEC))
        return -EINVAL;
    struct file_s* f = fd_get(oldfd);
    if (f == nullptr)
        return -EBADF;
    if (newfd < 0 || newfd >= PROC_MAX_FILE_DESCRIPTORS)
        return -EBADF;
    f->ref_count++;
    return fd_install_at(newfd, f, flags & LO_CLOEXEC);
}

long sys_dup2(const int oldfd, const int newfd)
{
    if (oldfd == newfd)
        return fd_get(oldfd) ? newfd : -EBADF;
    return sys_dup3(oldfd, newfd, 0);
}

static void set_nonblock(struct file_s* f, const bool on)
{
    if (f->node->type == INODE_SOCKET)
        socket_file_set_nonblock(f, on);
    else
        f->status_flags = on ? f->status_flags | LO_NONBLOCK : f->status_flags & ~LO_NONBLOCK;
}

long sys_fcntl(const int fd, const int cmd, const uint64_t arg)
{
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if (arg >= PROC_MAX_FILE_DESCRIPTORS)
            return -EINVAL;
        return dup_from(fd, (int)arg, cmd == F_DUPFD_CLOEXEC);
    case F_GETFD:
        return fd_cloexec(fd) ? FD_CLOEXEC : 0;
    case F_SETFD:
        fd_set_cloexec(fd, arg & FD_CLOEXEC);
        return 0;
    case F_GETFL: {
        const bool r = f->flags & O_RDONLY;
        const bool w = f->flags & O_WRONLY;
        return (r && w ? LO_RDWR : w ? LO_WRONLY : 0) | f->status_flags;
    }
    case F_SETFL:
        f->status_flags = (f->status_flags & ~LO_APPEND) | (int)(arg & LO_APPEND);
        set_nonblock(f, arg & LO_NONBLOCK);
        return 0;
    case F_GETLK: {
        constexpr int16_t unlocked = 2;
        return copy_to_user((void*)arg, &unlocked, sizeof(unlocked));
    }
    case F_SETLK:
    case F_SETLKW:
        return 0;
    case F_GETPIPE_SZ:
    case F_SETPIPE_SZ:
        return f->node->type == INODE_PIPE ? PIPE_BUFFER_SIZE : -EBADF;
    default:
        return -EINVAL;
    }
}

long sys_ioctl(const int fd, const unsigned long request, void* arg)
{
    struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;

    switch (request) {
    case FIOCLEX:
    case FIONCLEX:
        fd_set_cloexec(fd, request == FIOCLEX);
        return 0;
    case FIONBIO:
        if (f->node->type != INODE_SOCKET && f->node->type != INODE_CHAR) {
            int on;
            if (copy_from_user(&on, arg, sizeof(on)) < 0)
                return -EFAULT;
            set_nonblock(f, on != 0);
            return 0;
        }
        break;
    default:
        break;
    }
    if (f->node->fops && f->node->fops->ioctl)
        return f->node->fops->ioctl(f, request, arg);
    return -ENOTTY;
}

static long path_lookup(const int dirfd, const char* upath, char* path, size_t* type)
{
    const long err = resolve_at(dirfd, upath, path, false);
    if (err)
        return err;
    const int id = vfs_open(path, 0);
    if (id < 0)
        return id;
    if (type)
        *type = vfs_get_file(id)->node->type;
    vfs_close(id);
    return 0;
}

long sys_faccessat(const int dirfd, const char* upath, const int mode, const int flags)
{
    (void)flags;
    char path[VFS_PATH_MAX];
    size_t type;
    const long err = path_lookup(dirfd, upath, path, &type);
    (void)mode;
    (void)type;
    return err;
}

long sys_readlinkat(const int dirfd, const char* upath, char* buf, const size_t size)
{
    (void)buf;
    (void)size;
    char path[VFS_PATH_MAX];
    const long err = path_lookup(dirfd, upath, path, nullptr);

    return err ? err : -EINVAL;
}

long sys_mkdirat(const int dirfd, const char* upath, const uint32_t mode)
{
    (void)mode;
    char path[VFS_PATH_MAX];
    const long err = resolve_at(dirfd, upath, path, false);
    return err ? err : vfs_create(path, true);
}

long sys_unlinkat(const int dirfd, const char* upath, const int flags)
{
    if (flags & ~AT_REMOVEDIR)
        return -EINVAL;
    char path[VFS_PATH_MAX];
    const long err = resolve_at(dirfd, upath, path, false);
    return err ? err : vfs_unlink(path, flags & AT_REMOVEDIR);
}

long sys_renameat(const int olddirfd, const char* uold, const int newdirfd, const char* unew, const unsigned int flags)
{
    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;
    char old_path[VFS_PATH_MAX];
    char new_path[VFS_PATH_MAX];
    long err = resolve_at(olddirfd, uold, old_path, false);
    if (err == 0)
        err = resolve_at(newdirfd, unew, new_path, false);
    return err ? err : vfs_rename(old_path, new_path, flags & RENAME_NOREPLACE);
}

static long truncate_file(struct file_s* f, const int64_t length)
{
    if (length < 0)
        return -EINVAL;
    if (f->node->type == INODE_DIR)
        return -EISDIR;
    if (f->node->type != INODE_FILE || !(f->flags & O_WRONLY))
        return -EINVAL;
    return vfs_truncate((int)f->id, (size_t)length);
}

long sys_ftruncate(const int fd, const int64_t length)
{
    struct file_s* f = fd_get(fd);
    return f ? truncate_file(f, length) : -EBADF;
}

long sys_truncate(const char* upath, const int64_t length)
{
    char path[VFS_PATH_MAX];
    const long err = resolve_at(AT_FDCWD, upath, path, false);
    if (err)
        return err;
    const int id = vfs_open(path, O_WRONLY);
    if (id < 0)
        return id;
    const long r = truncate_file(vfs_get_file(id), length);
    vfs_close(id);
    return r;
}

long sys_path_exists(const int dirfd, const char* upath)
{
    if (upath == nullptr)
        return fd_get(dirfd) ? 0 : -EBADF;
    char path[VFS_PATH_MAX];
    return path_lookup(dirfd, upath, path, nullptr);
}

long sys_umask(const uint32_t mask)
{
    const uint32_t old = files()->umask;
    files()->umask = mask & 0777;
    return old;
}

long sys_fsync(const int fd)
{
    return fd_get(fd) ? vfs_sync() : -EBADF;
}

long sys_pipe2(int* ufds, const int flags)
{
    if (flags & ~(LO_CLOEXEC | LO_NONBLOCK))
        return -EINVAL;
    if (!uaccess_ok_write(ufds, 2 * sizeof(int)))
        return -EFAULT;
    struct file_s* rd;
    struct file_s* wr;
    const int err = pipe_create(&rd, &wr);
    if (err)
        return err;
    rd->status_flags = wr->status_flags = flags & LO_NONBLOCK;

    const int fds[2] = { fd_install_from(rd, 0, flags & LO_CLOEXEC), fd_install_from(wr, 0, flags & LO_CLOEXEC) };
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0)
            fd_close(fds[0]);
        else
            vfs_close((int)rd->id);
        if (fds[1] >= 0)
            fd_close(fds[1]);
        else
            vfs_close((int)wr->id);
        return -EMFILE;
    }
    return copy_to_user(ufds, fds, sizeof(fds));
}

long sys_sendfile(const int out_fd, const int in_fd, int64_t* uoffset, const size_t count)
{
    struct file_s* in = fd_get(in_fd);
    const struct file_s* out = fd_get(out_fd);
    if (in == nullptr || out == nullptr)
        return -EBADF;
    if (in->node->type != INODE_FILE)
        return -EINVAL;

    int64_t offset = 0;
    if (uoffset && copy_from_user(&offset, uoffset, sizeof(offset)) < 0)
        return -EFAULT;
    const size_t saved = in->file_position;
    if (uoffset)
        in->file_position = (size_t)offset;

    uint8_t* buf = malloc(PAGE_SIZE);
    if (buf == nullptr)
        return -ENOMEM;
    size_t done = 0;
    long err = 0;
    while (done < count) {
        const size_t want = count - done < PAGE_SIZE ? count - done : PAGE_SIZE;
        const ssize_t n = vfs_read((int)in->id, buf, want);
        if (n <= 0) {
            err = n;
            break;
        }
        const ssize_t w = vfs_write((int)out->id, buf, (size_t)n);
        if (w < 0) {
            err = w;
            in->file_position -= (size_t)n;
            break;
        }
        done += (size_t)w;
        if (w < n) {
            in->file_position -= (size_t)(n - w);
            break;
        }
    }
    free(buf);

    if (uoffset) {
        offset += (int64_t)done;
        in->file_position = saved;
        if (copy_to_user(uoffset, &offset, sizeof(offset)) < 0)
            return -EFAULT;
    }
    return done ? (long)done : err;
}

long sys_memfd_create(const char* uname, const unsigned int flags)
{
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))
        return -EINVAL;
    char name[MFD_NAME_MAX + 1];
    const long len = copy_string_from_user(name, uname, sizeof(name));
    if (len < 0)
        return len == -ENAMETOOLONG ? -EINVAL : len;

    const int id = vfs_memfd_create(name);
    if (id < 0)
        return id;
    const int fd = fd_install_from(vfs_get_file(id), 0, flags & MFD_CLOEXEC);
    if (fd < 0)
        vfs_close(id);
    return fd;
}

long sys_chroot(const char* upath)
{
    char path[VFS_PATH_MAX];
    const long err = resolve_at(AT_FDCWD, upath, path, false);
    if (err)
        return err;
    const int id = vfs_open(path, 0);
    if (id < 0)
        return id;
    const bool dir = vfs_get_file(id)->node->type == INODE_DIR;
    vfs_close(id);
    if (!dir)
        return -ENOTDIR;
    strncpy(files()->root, path, VFS_PATH_MAX - 1);
    return 0;
}

long sys_close_range(const unsigned int first, unsigned int last, const unsigned int flags)
{
    if (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC))
        return -EINVAL;
    if (first > last)
        return -EINVAL;

    process_t* current = scheduler_get()->current;
    if ((flags & CLOSE_RANGE_UNSHARE) && current->files->ref_count > 1) {
        struct proc_files* own = files_clone(current->files);
        if (own == nullptr)
            return -ENOMEM;
        files_release(current->files);
        current->files = own;
    }

    if (first >= PROC_MAX_FILE_DESCRIPTORS)
        return 0;
    if (last >= PROC_MAX_FILE_DESCRIPTORS)
        last = PROC_MAX_FILE_DESCRIPTORS - 1;
    for (unsigned int fd = first; fd <= last; fd++) {
        if (flags & CLOSE_RANGE_CLOEXEC)
            fd_set_cloexec((int)fd, true);
        else if (fd_get((int)fd))
            fd_close((int)fd);
    }
    return 0;
}

long sys_mknodat(const int dirfd, const char* upath, const uint32_t mode, const uint32_t dev)
{
    (void)dev;
    switch (mode & 0170000) {
    case 0:
    case S_IFREG:
        break;
    case S_IFIFO:
    case S_IFCHR:
    case S_IFBLK:
    case S_IFSOCK:
        return -EPERM;
    default:
        return -EINVAL;
    }
    char path[VFS_PATH_MAX];
    const long err = resolve_at(dirfd, upath, path, false);
    return err ? err : vfs_create(path, false);
}

long sys_fallocate(const int fd, const int mode, const int64_t offset, const int64_t len)
{
    const struct file_s* file = fd_get(fd);
    if (file == nullptr || !(file->flags & O_WRONLY))
        return -EBADF;
    if (offset < 0 || len <= 0)
        return -EINVAL;
    switch (file->node->type) {
    case INODE_FILE:
        break;
    case INODE_DIR:
        return -EISDIR;
    case INODE_PIPE:
        return -ESPIPE;
    default:
        return -ENODEV;
    }
    return vfs_fallocate((int)file->id, mode, offset, len);
}

static long copy_range(struct file_s* in, int64_t* in_pos, struct file_s* out, int64_t* out_pos, const size_t len,
                       const bool once)
{
    uint8_t* buf = malloc(COPY_CHUNK);
    if (buf == nullptr)
        return -ENOMEM;
    size_t done = 0;
    long err = 0;
    while (done < len) {
        const size_t want = len - done < COPY_CHUNK ? len - done : COPY_CHUNK;
        const size_t in_saved = in->file_position;
        if (in_pos)
            in->file_position = (size_t)*in_pos;
        const ssize_t n = vfs_read((int)in->id, buf, want);
        if (in_pos)
            in->file_position = in_saved;
        if (n <= 0) {
            err = n;
            break;
        }
        if (in_pos)
            *in_pos += n;

        size_t put = 0;
        while (put < (size_t)n) {
            const size_t out_saved = out->file_position;
            if (out_pos)
                out->file_position = (size_t)*out_pos;
            const ssize_t w = vfs_write((int)out->id, buf + put, (size_t)n - put);
            if (out_pos)
                out->file_position = out_saved;
            if (w <= 0) {
                err = w ? w : -EIO;
                break;
            }
            if (out_pos)
                *out_pos += w;
            put += (size_t)w;
        }
        done += put;
        if (put < (size_t)n) {
            if (in_pos)
                *in_pos -= (int64_t)((size_t)n - put);
            else if (in->node->type == INODE_FILE)
                in->file_position -= (size_t)n - put;
            break;
        }
        if (once || (size_t)n < want)
            break;
    }
    free(buf);
    return done ? (long)done : err;
}

static long offset_in(const int64_t* uoff, int64_t* off)
{
    if (uoff == nullptr)
        return 0;
    if (copy_from_user(off, uoff, sizeof(*off)) < 0)
        return -EFAULT;
    return *off < 0 ? -EINVAL : 0;
}

static long offsets_out(int64_t* uoff_in, const int64_t* in_pos, int64_t* uoff_out, const int64_t* out_pos)
{
    if (uoff_in && copy_to_user(uoff_in, in_pos, sizeof(*in_pos)) < 0)
        return -EFAULT;
    if (uoff_out && copy_to_user(uoff_out, out_pos, sizeof(*out_pos)) < 0)
        return -EFAULT;
    return 0;
}

long sys_splice(const int fd_in, int64_t* off_in, const int fd_out, int64_t* off_out, const size_t len,
                const unsigned int flags)
{
    struct file_s* in = fd_get(fd_in);
    struct file_s* out = fd_get(fd_out);
    if (in == nullptr || out == nullptr || !(in->flags & O_RDONLY) || !(out->flags & O_WRONLY))
        return -EBADF;
    if (flags & ~SPLICE_F_ALL)
        return -EINVAL;
    const bool in_pipe = in->node->type == INODE_PIPE;
    const bool out_pipe = out->node->type == INODE_PIPE;
    if (!in_pipe && !out_pipe)
        return -EINVAL;
    if ((in_pipe && off_in) || (out_pipe && off_out))
        return -ESPIPE;
    if (out->status_flags & LO_APPEND)
        return -EINVAL;
    if (len == 0)
        return 0;

    int64_t in_pos = 0;
    int64_t out_pos = 0;
    long err = offset_in(off_in, &in_pos);
    if (err == 0)
        err = offset_in(off_out, &out_pos);
    if (err)
        return err;
    const long r = copy_range(in, off_in ? &in_pos : nullptr, out, off_out ? &out_pos : nullptr, len, true);
    if (r > 0 && offsets_out(off_in, &in_pos, off_out, &out_pos) < 0)
        return -EFAULT;
    return r;
}

long sys_copy_file_range(const int fd_in, int64_t* off_in, const int fd_out, int64_t* off_out, const size_t len,
                         const uint32_t flags)
{
    struct file_s* in = fd_get(fd_in);
    struct file_s* out = fd_get(fd_out);
    if (in == nullptr || out == nullptr || !(in->flags & O_RDONLY) || !(out->flags & O_WRONLY)
        || (out->status_flags & LO_APPEND))
        return -EBADF;
    if (flags != 0)
        return -EINVAL;
    if (in->node->type == INODE_DIR || out->node->type == INODE_DIR)
        return -EISDIR;
    if (in->node->type != INODE_FILE || out->node->type != INODE_FILE)
        return -EINVAL;

    int64_t in_pos = (int64_t)in->file_position;
    int64_t out_pos = (int64_t)out->file_position;
    long err = offset_in(off_in, &in_pos);
    if (err == 0)
        err = offset_in(off_out, &out_pos);
    if (err)
        return err;
    if (in->node == out->node && in_pos < out_pos + (int64_t)len && out_pos < in_pos + (int64_t)len)
        return -EINVAL;
    if (len == 0)
        return 0;

    const long r = copy_range(in, &in_pos, out, &out_pos, len, false);
    if (r > 0) {
        if (!off_in)
            in->file_position = (size_t)in_pos;
        if (!off_out)
            out->file_position = (size_t)out_pos;
        if (offsets_out(off_in, &in_pos, off_out, &out_pos) < 0)
            return -EFAULT;
    }
    return r;
}