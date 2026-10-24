#include "vfs.h"
#include "drivers/terminal/terminal.h"
#include "utils/str/str.h"


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


int vfs_open(const char* path, const int flags)
{
    const char* rest;
    mounted_file_system_t* mount = find_mount(path, &rest);
    if (mount == nullptr)
        return -1;
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
            release_node(mount, current);
            return -1;
        }
        memcpy(name, rest, n);
        name[n] = '\0';

        struct inode_s* next = current->iops->lookup(current, name);
        release_node(mount, current);
        current = next;
        if (current == nullptr)
            return -1;

        rest += n;
    }
    if (current->fops == nullptr || current->fops->open == nullptr)
    {
        release_node(mount, current);
        return -1;
    }

    struct file_s* f = malloc(sizeof(struct file_s));
    if (f == nullptr)
    {
        release_node(mount, current);
        return -1;
    }
    memset(f, 0, sizeof(*f));
    f->node = current;
    f->flags = flags;
    f->file_position = 0;
    f->mount = mount;
    f->ref_count = 1;

    if (current->fops->open(current, f) < 0)
    {
        release_node(mount, current);
        free(f);
        return -1;
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
    return -1;
}

ssize_t vfs_read(const int fd, void* buf, const size_t length)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr) 
        return -1;

    if (!(f->flags & O_RDONLY)) 
        return -1;

    if (f->node->fops == nullptr || f->node->fops->read == nullptr) 
        return -1;
    
    return f->node->fops->read(f, buf, length);
}

ssize_t vfs_write(const int fd, const void* buf, const size_t length)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr) 
        return -1;

    if (!(f->flags & O_WRONLY)) 
        return -1;

    if (f->node->fops == nullptr || f->node->fops->write == nullptr)
        return -1;
    return f->node->fops->write(f, buf, length);
}

int vfs_seek(const int fd, const size_t offset, enum seek_origin origin)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr) 
        return -1;

    switch (origin)
    {
        case SEEK_SET:
            if (offset > f->node->size) 
                return -1;
            f->file_position = offset;
            break;
        case SEEK_CUR:
            if (f->file_position + offset > f->node->size) 
                return -1;
            f->file_position += offset;
            break;
        case SEEK_END:
            if (offset > f->node->size) 
                return -1;
            f->file_position = f->node->size - offset;
            break;
        default:
            return -1;
    }

    return 0;
}


int vfs_stat(const int fd, vfs_stat_t* st)
{
    struct file_s* f = get_file(fd);
    if (f == nullptr || st == nullptr)
        return -1;

    st->size = f->node->size;
    st->type = f->node->type;
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
