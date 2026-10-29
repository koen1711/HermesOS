#include "devfs.h"

#include <drivers/fs/memfd/memfd.h>
#include <drivers/terminal/tty.h>
#include <hardware/memory/alloc.h>
#include <utils/random/random.h>
#include <utils/str/str.h>
#include <os/errno.h>

#define DEV_ID(major, minor) (((major) << 8) | (minor))

static int dev_open(struct inode_s* node, struct file_s* f)
{
    (void)node;
    (void)f;
    return 0;
}

static int dev_poll(struct file_s* f)
{
    (void)f;
    return VFS_POLLIN | VFS_POLLOUT;
}

static ssize_t eof_read(struct file_s* f, void* buf, const size_t len)
{
    (void)f;
    (void)buf;
    (void)len;
    return 0;
}

static ssize_t zero_read(struct file_s* f, void* buf, const size_t len)
{
    (void)f;
    memset(buf, 0, len);
    return (ssize_t)len;
}

static ssize_t random_read(struct file_s* f, void* buf, const size_t len)
{
    (void)f;
    random_fill(buf, len);
    return (ssize_t)len;
}

static ssize_t sink_write(struct file_s* f, const void* buf, const size_t len)
{
    (void)f;
    (void)buf;
    return (ssize_t)len;
}

static ssize_t full_write(struct file_s* f, const void* buf, const size_t len)
{
    (void)f;
    (void)buf;
    return len ? -ENOSPC : 0;
}

static file_operations_t null_fops = { .open = dev_open, .read = eof_read, .write = sink_write, .poll = dev_poll };
static file_operations_t zero_fops = { .open = dev_open, .read = zero_read, .write = sink_write, .poll = dev_poll };
static file_operations_t full_fops = { .open = dev_open, .read = zero_read, .write = full_write, .poll = dev_poll };
static file_operations_t random_fops = { .open = dev_open, .read = random_read, .write = sink_write, .poll = dev_poll };

static inode_t null_inode = { .id = DEV_ID(1, 3), .type = INODE_CHAR, .fops = &null_fops };
static inode_t zero_inode = { .id = DEV_ID(1, 5), .type = INODE_CHAR, .fops = &zero_fops };
static inode_t full_inode = { .id = DEV_ID(1, 7), .type = INODE_CHAR, .fops = &full_fops };
static inode_t random_inode = { .id = DEV_ID(1, 8), .type = INODE_CHAR, .fops = &random_fops };
static inode_t urandom_inode = { .id = DEV_ID(1, 9), .type = INODE_CHAR, .fops = &random_fops };

struct shm_entry {
    char name[MAX_FILE_PATH];
    inode_t* node;
    struct shm_entry* next;
};

static struct shm_entry* shm_files;

static struct shm_entry* shm_find(const char* name, struct shm_entry*** link)
{
    for (struct shm_entry** l = &shm_files; *l; l = &(*l)->next) {
        if (strcmp((*l)->name, name) == 0) {
            if (link)
                *link = l;
            return *l;
        }
    }
    return nullptr;
}

static struct inode_s* shm_lookup(const struct inode_s* dir, const char* name)
{
    (void)dir;
    const struct shm_entry* e = shm_find(name, nullptr);
    return e ? e->node : nullptr;
}

static int shm_create(struct inode_s* dir, const char* name, const bool directory)
{
    (void)dir;
    if (directory)
        return -EPERM;
    if (shm_find(name, nullptr))
        return -EEXIST;
    if (strlen(name) >= MAX_FILE_PATH)
        return -ENAMETOOLONG;
    struct shm_entry* e = malloc(sizeof(struct shm_entry));
    inode_t* node = mem_inode_create();
    if (!e || !node) {
        free(e);
        if (node)
            mem_inode_unlink(node);
        return -ENOSPC;
    }
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->node = node;
    e->next = shm_files;
    shm_files = e;
    return 0;
}

static void shm_remove(struct shm_entry** link)
{
    struct shm_entry* e = *link;
    *link = e->next;
    mem_inode_unlink(e->node);
    free(e);
}

static int shm_unlink(struct inode_s* dir, const char* name, const bool directory)
{
    (void)dir;
    struct shm_entry** link;
    if (shm_find(name, &link) == nullptr)
        return -ENOENT;
    if (directory)
        return -ENOTDIR;
    shm_remove(link);
    return 0;
}

static inode_t shm_dir;

static int shm_rename(struct inode_s* old_dir, const char* old_name, struct inode_s* new_dir, const char* new_name,
                      const bool no_replace)
{
    if (old_dir != &shm_dir || new_dir != &shm_dir)
        return -EXDEV;
    struct shm_entry* e = shm_find(old_name, nullptr);
    if (e == nullptr)
        return -ENOENT;
    if (strlen(new_name) >= MAX_FILE_PATH)
        return -ENAMETOOLONG;
    struct shm_entry** link;
    struct shm_entry* target = shm_find(new_name, &link);
    if (target == e)
        return 0;
    if (target) {
        if (no_replace)
            return -EEXIST;
        shm_remove(link);
    }
    memset(e->name, 0, sizeof(e->name));
    strncpy(e->name, new_name, sizeof(e->name) - 1);
    return 0;
}

static int shm_readdir(struct file_s* f, struct vfs_dirent* out)
{
    const struct shm_entry* e = shm_files;
    for (size_t i = 0; e && i < f->file_position; i++)
        e = e->next;
    if (e == nullptr)
        return 0;
    f->file_position++;
    strncpy(out->name, e->name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    out->ino = e->node->id;
    out->type = INODE_FILE;
    return 1;
}

static inode_operations_t shm_iops = {
    .lookup = shm_lookup,
    .create = shm_create,
    .unlink = shm_unlink,
    .rename = shm_rename,
};
static file_operations_t shm_fops = { .open = dev_open, .readdir = shm_readdir };

static inode_t shm_dir = { .id = 2, .type = INODE_DIR, .path = "/shm", .fops = &shm_fops, .iops = &shm_iops };

static file_operations_t stdio_fops = { 0 };
static inode_t stdin_inode = { .id = 3, .type = INODE_CHAR, .fops = &stdio_fops };
static inode_t stdout_inode = { .id = 4, .type = INODE_CHAR, .fops = &stdio_fops };
static inode_t stderr_inode = { .id = 5, .type = INODE_CHAR, .fops = &stdio_fops };

struct dev_entry {
    const char* name;
    inode_t* node;
};

static struct dev_entry entries[] = {
    { "null", &null_inode },
    { "zero", &zero_inode },
    { "full", &full_inode },
    { "random", &random_inode },
    { "urandom", &urandom_inode },
    { "tty", nullptr },
    { "console", nullptr },
    { "shm", &shm_dir },
    { "stdin", &stdin_inode },
    { "stdout", &stdout_inode },
    { "stderr", &stderr_inode },
};

#define ENTRY_COUNT (sizeof(entries) / sizeof(entries[0]))

static inode_t* entry_node(const struct dev_entry* e)
{
    return e->node ? e->node : tty_node();
}

static struct inode_s* devfs_lookup(const struct inode_s* dir, const char* name)
{
    (void)dir;
    for (size_t i = 0; i < ENTRY_COUNT; i++)
        if (strcmp(entries[i].name, name) == 0)
            return entry_node(&entries[i]);
    return nullptr;
}

static int devfs_readdir(struct file_s* f, struct vfs_dirent* out)
{
    if (f->file_position >= ENTRY_COUNT)
        return 0;
    const struct dev_entry* e = &entries[f->file_position++];
    const inode_t* node = entry_node(e);
    strncpy(out->name, e->name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    out->ino = node->id;
    out->type = node->type;
    return 1;
}

static inode_operations_t devfs_dir_iops = { .lookup = devfs_lookup };
static file_operations_t devfs_dir_fops = { .open = dev_open, .readdir = devfs_readdir };

static inode_t devfs_root = {
    .id = 1,
    .type = INODE_DIR,
    .path = "/",
    .fops = &devfs_dir_fops,
    .iops = &devfs_dir_iops,
};

static struct inode_s* devfs_mount(void* blob)
{
    (void)blob;
    return &devfs_root;
}

static void devfs_unmount(struct inode_s* root)
{
    (void)root;
}

file_system_driver_t devfs_driver = {
    .mount = devfs_mount,
    .unmount = devfs_unmount,
};
