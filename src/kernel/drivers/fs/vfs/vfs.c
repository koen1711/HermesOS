#include "vfs.h"
#include "drivers/terminal/terminal.h"
#include "utils/str/str.h"
#include <os/errno.h>


static struct file_s* fd_table[MAX_OPEN_FILES];
static struct registered_driver_s driver_table[MAX_DRIVERS];
static struct mounted_file_system_s mount_table[MAX_MOUNTED_FILESYSTEMS];

static int driver_count = 0;

static struct registered_driver_s* get_driver(const char* fs_name)
{
    for (int i = 0; i < driver_count; i++)
    {
        if ((strcmp((driver_table[i].name), fs_name) == 0))
        {
            return &driver_table[i];
        }
    }
    return nullptr;
}

static struct mounted_file_system_s* find_mount(const char* path, const char** rest)
{
    struct mounted_file_system_s* best = nullptr;
    size_t best_len = 0;

    for (int i = 0; i < MAX_MOUNTED_FILESYSTEMS; i++)
    {
        if (mount_table[i].root == nullptr) continue;

        const size_t len = mount_table[i].mount_path_length;
        if (strncmp(path, mount_table[i].mount_path, len) != 0) { continue; }

        const char next = path[len];
        if (next != '\0' && next != '/' && len != 1) { continue; }

        if (best == nullptr || len > best_len)
        {
            best = &mount_table[i];
            best_len = len;
        }
    }
    if (best != nullptr) 
        *rest = path + best_len;
    
    return best;
}

int vfs_register_driver(const char* name, file_system_driver_t* fsd)
{
    if (name == nullptr || fsd == nullptr)
        return -1;
    if (driver_count >= MAX_DRIVERS) 
        return -1;
    
    for (int i = 0; i < driver_count; i++)
    {
        if (strcmp(name, driver_table[i].name) == 0) 
            return -1;
    }
    strncpy(driver_table[driver_count].name, name, VFS_NAME_MAX - 1);
    driver_table[driver_count].fsd = fsd;
    driver_count++;
    return 0;
}


int vfs_mount(const char* mount_path, const char* fs_name, void* blob)
{
    registered_driver_t* reg_driver = get_driver(fs_name);
    if (reg_driver == nullptr) 
        return -1;

    for (int i = 0; i < MAX_MOUNTED_FILESYSTEMS; i++)
    {
        if (mount_table[i].root != nullptr &&
            strcmp(mount_path, mount_table[i].mount_path) == 0) 
            return -1;
    }

    struct inode_s* root_node = reg_driver->fsd->mount(blob);
    if (root_node == nullptr) 
        return -1;

    for (int i = 0; i < MAX_MOUNTED_FILESYSTEMS; i++)
    {
        if (mount_table[i].root == nullptr)
        {
            mount_table[i].driver = reg_driver;
            mount_table[i].root = root_node;
            strncpy(mount_table[i].mount_path, mount_path, VFS_PATH_MAX - 1);
            mount_table[i].mount_path_length = strlen(mount_table[i].mount_path);
            return 0;
        }
    }
    reg_driver->fsd->unmount(root_node);
    return -1;
}

int vfs_unmount(const char* mount_path)
{
    for (int i = 0; i < MAX_MOUNTED_FILESYSTEMS; i++)
    {
        if (mount_table[i].root != nullptr && strcmp(mount_path, mount_table[i].mount_path) == 0)
        {
            if (mount_table[i].open_count > 0)
                return -1;
            mount_table[i].driver->fsd->unmount(mount_table[i].root);
            memset(&mount_table[i], 0, sizeof(mount_table[i]));
            return 0;
        }
    }
    return -1;
}

static void release_node(const mounted_file_system_t* mount, struct inode_s* node)
{
    if (node == nullptr || node == mount->root)
        return;
    if (node->iops != nullptr && node->iops->release != nullptr)
        node->iops->release(node);
}

static struct file_s* get_file(const int fd)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES) 
        return nullptr;
    
    return fd_table[fd];
}

struct file_s* vfs_get_file(const int fd)
{
    return get_file(fd);
}

int vfs_install(struct file_s* f)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++)
    {
        if (fd_table[i] == nullptr)
        {
            f->id = i;
            fd_table[i] = f;
            return i;
        }
    }
    return -1;
}


static int vfs_walk(const char* path, mounted_file_system_t** mount_out, struct inode_s** out)
{
    const char* rest;
    mounted_file_system_t* mount = find_mount(path, &rest);
    if (mount == nullptr)
        return -ENOENT;
    struct inode_s* current = mount->root;
    while (*rest != '\0')
    {
        while (*rest == '/') rest++;
        if (*rest == '\0') { break; }
        size_t n = 0;
        while (rest[n] != '/' && rest[n] != '\0') n++;

        char name[MAX_FILE_PATH];
        if (n >= sizeof(name) || current->iops == nullptr || current->iops->lookup == nullptr)
        {
            const int err = n >= sizeof(name) ? -ENAMETOOLONG : -ENOTDIR;
            release_node(mount, current);
            return err;
        }
        memcpy(name, rest, n);
        name[n] = '\0';

        struct inode_s* next = current->iops->lookup(current, name);
        release_node(mount, current);
        current = next;
        if (current == nullptr)
            return -ENOENT;

        rest += n;
    }
    *mount_out = mount;
    *out = current;
    return 0;
}

int vfs_open(const char* path, const int flags)
{
    mounted_file_system_t* mount;
    struct inode_s* current;
    const int err = vfs_walk(path, &mount, &current);
    if (err)
        return err;
    if (current->fops == nullptr || current->fops->open == nullptr)
    {
        release_node(mount, current);
        return -EACCES;
    }

    struct file_s* f = malloc(sizeof(struct file_s));
    if (f == nullptr)
    {
        release_node(mount, current);
        return -ENOMEM;
    }
    memset(f, 0, sizeof(*f));
    f->node = current;
    f->flags = flags;
    f->file_position = 0;
    f->mount = mount;
    f->ref_count = 1;

    strncpy(f->filename, path, sizeof(f->filename) - 1);

    if (current->fops->open(current, f) < 0)
    {
        release_node(mount, current);
        free(f);
        return -EACCES;
    }
    for (int i = 0; i < MAX_OPEN_FILES; i++)
    {
        if (fd_table[i] == nullptr)
        {
            f->id = i;
            fd_table[i] = f;
            mount->open_count++;
            return i;
        }
    }

    if (current->fops->close) current->fops->close(f);
    release_node(mount, current);
    free(f);
    return -ENFILE;
}

ssize_t vfs_read(const int fd, void* buf, const size_t length)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr) 
        return -EBADF;

    if (!(f->flags & O_RDONLY)) 
        return -EBADF;

    if (f->node->type == INODE_DIR)
        return -EISDIR;
    if (f->node->fops == nullptr || f->node->fops->read == nullptr) 
        return -EINVAL;
    
    return f->node->fops->read(f, buf, length);
}

ssize_t vfs_write(const int fd, const void* buf, const size_t length)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr) 
        return -EBADF;

    if (!(f->flags & O_WRONLY)) 
        return -EBADF;

    if (f->node->type == INODE_DIR)
        return -EISDIR;
    if (f->node->fops == nullptr || f->node->fops->write == nullptr)
        return -EROFS;
    return f->node->fops->write(f, buf, length);
}

// The directory holding path and the last component of path, which points into path
static int vfs_parent(const char* path, mounted_file_system_t** mount, struct inode_s** dir, const char** name)
{
    const char* slash = nullptr;
    for (const char* p = path; *p; p++)
        if (*p == '/') slash = p;
    if (slash == nullptr || slash[1] == '\0')
        return -EBUSY;

    char parent[VFS_PATH_MAX];
    const size_t len = slash == path ? 1 : (size_t)(slash - path);
    if (len >= sizeof(parent))
        return -ENAMETOOLONG;
    memcpy(parent, path, len);
    parent[len] = '\0';

    const int err = vfs_walk(parent, mount, dir);
    if (err)
        return err;
    if ((*dir)->type != INODE_DIR)
    {
        release_node(*mount, *dir);
        return -ENOTDIR;
    }
    *name = slash + 1;
    return 0;
}

int vfs_create(const char* path, const bool directory)
{
    mounted_file_system_t* mount;
    struct inode_s* dir;
    const char* name;
    int err = vfs_parent(path, &mount, &dir, &name);
    if (err)
        return err;
    err = dir->iops && dir->iops->create ? dir->iops->create(dir, name, directory) : -EROFS;
    release_node(mount, dir);
    return err;
}

int vfs_unlink(const char* path, const bool directory)
{
    mounted_file_system_t* mount;
    struct inode_s* dir;
    const char* name;
    int err = vfs_parent(path, &mount, &dir, &name);
    if (err)
        return err;
    err = dir->iops && dir->iops->unlink ? dir->iops->unlink(dir, name, directory) : -EROFS;
    release_node(mount, dir);
    return err;
}

int vfs_rename(const char* old_path, const char* new_path, const bool no_replace)
{
    const size_t old_len = strlen(old_path);
    if (strncmp(new_path, old_path, old_len) == 0 && new_path[old_len] == '/')
        return -EINVAL;

    mounted_file_system_t* old_mount;
    mounted_file_system_t* new_mount;
    struct inode_s* old_dir;
    struct inode_s* new_dir;
    const char* old_name;
    const char* new_name;
    int err = vfs_parent(old_path, &old_mount, &old_dir, &old_name);
    if (err)
        return err;
    if ((err = vfs_parent(new_path, &new_mount, &new_dir, &new_name)) != 0)
    {
        release_node(old_mount, old_dir);
        return err;
    }
    if (old_mount != new_mount)
        err = -EXDEV;
    else if (!old_dir->iops || !old_dir->iops->rename)
        err = -EROFS;
    else
        err = old_dir->iops->rename(old_dir, old_name, new_dir, new_name, no_replace);
    release_node(new_mount, new_dir);
    release_node(old_mount, old_dir);
    return err;
}

int vfs_truncate(const int fd, const size_t size)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr)
        return -EBADF;
    if (f->node->type == INODE_DIR)
        return -EISDIR;
    if (f->node->fops == nullptr || f->node->fops->truncate == nullptr)
        return -EINVAL;
    return f->node->fops->truncate(f, size);
}

int64_t vfs_seek(const int fd, const int64_t offset, const enum seek_origin origin)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr) 
        return -EBADF;
    if (f->node->type != INODE_FILE && f->node->type != INODE_DIR)
        return -ESPIPE;

    int64_t base;
    switch (origin)
    {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = (int64_t)f->file_position;
            break;
        case SEEK_END:
            base = (int64_t)f->node->size;
            break;
        default:
            return -EINVAL;
    }
    if (offset < 0 && base + offset < 0)
        return -EINVAL;

    if (f->node->type == INODE_DIR && (origin != SEEK_SET || offset != 0))
        return -EINVAL;

    f->file_position = (size_t)(base + offset);
    return (int64_t)f->file_position;
}

int vfs_readdir(const int fd, struct vfs_dirent* out)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr)
        return -EBADF;
    if (f->node->type != INODE_DIR)
        return -ENOTDIR;
    if (f->node->fops == nullptr || f->node->fops->readdir == nullptr)
        return -EINVAL;
    return f->node->fops->readdir(f, out);
}


int vfs_stat(const int fd, vfs_stat_t* st)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr || st == nullptr)
        return -1;

    st->size = f->node->size;
    st->type = f->node->type;
    st->ino = f->node->id;
    st->mtime = f->node->mtime;
    return 0;
}

int vfs_normalize_path(const char* base, const char* path, char* out)
{
    size_t len = 0;
    out[len++] = '/';

    for (int pass = path[0] == '/' ? 1 : 0; pass < 2; pass++)
    {
        const char* p = pass == 0 ? base : path;
        while (*p != '\0')
        {
            while (*p == '/') p++;
            if (*p == '\0') break;
            size_t n = 0;
            while (p[n] != '/' && p[n] != '\0') n++;

            if (n == 2 && p[0] == '.' && p[1] == '.') {
                while (len > 1 && out[len - 1] != '/') len--;
                if (len > 1) len--;
            } else if (n != 1 || p[0] != '.') {
                if (len + (len > 1) + n >= VFS_PATH_MAX)
                    return -ENAMETOOLONG;
                if (len > 1) out[len++] = '/';
                memcpy(out + len, p, n);
                len += n;
            }
            p += n;
        }
    }
    out[len] = '\0';
    return 0;
}


int vfs_close(const int fd)
{
    struct file_s* f = get_file(fd);

    if (f == nullptr) 
        return -1;
    f->ref_count--;


    if (f->ref_count == 0) {
        if (f->node->fops != nullptr && f->node->fops->close != nullptr)
            f->node->fops->close(f);
        if (f->mount != nullptr)
        {
            release_node(f->mount, f->node);
            f->mount->open_count--;
        }
        free(f);
        fd_table[fd] = nullptr;
    }
    return 0;
}

int vfs_sync()
{
    int err = 0;
    for (int i = 0; i < MAX_MOUNTED_FILESYSTEMS; i++)
    {
        const mounted_file_system_t* m = &mount_table[i];
        if (m->root != nullptr && m->driver->fsd->sync != nullptr && m->driver->fsd->sync(m->root) < 0)
            err = -EIO;
    }
    return err;
}
