#include "memfd.h"

#include <drivers/fs/vfs/vfs.h>
#include <hardware/memory/alloc.h>
#include <hardware/memory/mmu.h>
#include <hardware/memory/pmm.h>
#include <utils/str/str.h>
#include <os/errno.h>

#define MEMFD_MAX_SIZE (1ULL << 40)
#define MEMFD_PREFIX "/memfd:"

struct xattr {
    struct xattr* next;
    void* value;
    size_t size;
    char name[];
};

struct memfd {
    uint8_t** pages;
    size_t page_count;
    struct xattr* xattrs;
    int opens;
    bool unlinked;
};

static size_t next_ino = 1;

static struct memfd* memfd_of(const struct file_s* f)
{
    return f->node->private_data;
}

static uint8_t* page_get(struct memfd* m, const size_t index, const bool create)
{
    if (index >= m->page_count) {
        if (!create)
            return nullptr;
        size_t count = m->page_count ? m->page_count * 2 : 16;
        if (count <= index)
            count = index + 1;
        uint8_t** pages = malloc(count * sizeof(uint8_t*));
        if (pages == nullptr)
            return nullptr;
        if (m->page_count)
            memcpy(pages, m->pages, m->page_count * sizeof(uint8_t*));
        memset(pages + m->page_count, 0, (count - m->page_count) * sizeof(uint8_t*));
        free(m->pages);
        m->pages = pages;
        m->page_count = count;
    }
    if (m->pages[index] == nullptr && create) {
        m->pages[index] = alloc_physical_page();
        if (m->pages[index])
            memset(m->pages[index], 0, PAGE_SIZE);
    }
    return m->pages[index];
}

static ssize_t memfd_read(struct file_s* f, void* buf, const size_t len)
{
    struct memfd* m = memfd_of(f);
    const size_t pos = f->file_position;
    const size_t size = f->node->size;
    if (pos >= size)
        return 0;
    const size_t total = len < size - pos ? len : size - pos;
    for (size_t done = 0; done < total;) {
        const size_t in = (pos + done) % PAGE_SIZE;
        const size_t n = total - done < PAGE_SIZE - in ? total - done : PAGE_SIZE - in;
        const uint8_t* page = page_get(m, (pos + done) / PAGE_SIZE, false);
        if (page)
            memcpy((uint8_t*)buf + done, page + in, n);
        else
            memset((uint8_t*)buf + done, 0, n);
        done += n;
    }
    f->file_position += total;
    return (ssize_t)total;
}

static ssize_t memfd_write(struct file_s* f, const void* buf, const size_t len)
{
    struct memfd* m = memfd_of(f);
    const size_t pos = f->file_position;
    if (len == 0)
        return 0;
    if (pos >= MEMFD_MAX_SIZE || len > MEMFD_MAX_SIZE - pos)
        return -EFBIG;
    size_t done = 0;
    while (done < len) {
        const size_t in = (pos + done) % PAGE_SIZE;
        const size_t n = len - done < PAGE_SIZE - in ? len - done : PAGE_SIZE - in;
        uint8_t* page = page_get(m, (pos + done) / PAGE_SIZE, true);
        if (page == nullptr)
            break;
        memcpy(page + in, (const uint8_t*)buf + done, n);
        done += n;
    }
    if (done == 0)
        return -ENOSPC;
    f->file_position += done;
    if (pos + done > f->node->size)
        f->node->size = pos + done;
    return (ssize_t)done;
}

static int memfd_truncate(struct file_s* f, const size_t size)
{
    struct memfd* m = memfd_of(f);
    if (size > MEMFD_MAX_SIZE)
        return -EFBIG;
    if (size < f->node->size) {
        const size_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (size_t i = keep; i < m->page_count; i++) {
            if (m->pages[i])
                free_physical_page(m->pages[i]);
            m->pages[i] = nullptr;
        }
        uint8_t* tail = size % PAGE_SIZE ? page_get(m, size / PAGE_SIZE, false) : nullptr;
        if (tail)
            memset(tail + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
    }
    f->node->size = size;
    return 0;
}

static int memfd_fallocate(struct file_s* f, const int mode, const int64_t offset, const int64_t len)
{
    if (mode != 0)
        return -EOPNOTSUPP;
    const uint64_t end = (uint64_t)offset + (uint64_t)len;
    if (end > MEMFD_MAX_SIZE)
        return -EFBIG;
    for (uint64_t i = (uint64_t)offset / PAGE_SIZE; i < (end + PAGE_SIZE - 1) / PAGE_SIZE; i++)
        if (page_get(memfd_of(f), i, true) == nullptr)
            return -ENOSPC;
    return end > f->node->size ? memfd_truncate(f, end) : 0;
}

static int memfd_mmap_page(struct file_s* f, const uint64_t offset, uint64_t* frame)
{
    if (offset >= MEMFD_MAX_SIZE)
        return -EFBIG;
    uint8_t* page = page_get(memfd_of(f), offset / PAGE_SIZE, true);
    if (page == nullptr)
        return -ENOMEM;
    pmm_share_page(page);
    *frame = (uint64_t)page;
    return 0;
}

static bool xattr_name_ok(const char* name)
{
    static const char* const prefixes[] = { "user.", "trusted.", "security." };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        const size_t n = strlen(prefixes[i]);
        if (strncmp(name, prefixes[i], n) == 0)
            return name[n] != '\0';
    }
    return false;
}

static struct xattr* xattr_find(struct memfd* m, const char* name, struct xattr*** link)
{
    for (struct xattr** l = &m->xattrs; *l; l = &(*l)->next) {
        if (strcmp((*l)->name, name) == 0) {
            if (link)
                *link = l;
            return *l;
        }
    }
    return nullptr;
}

static ssize_t memfd_getxattr(inode_t* node, const char* name, void* value, const size_t size)
{
    if (!xattr_name_ok(name))
        return -EOPNOTSUPP;
    const struct xattr* x = xattr_find(node->private_data, name, nullptr);
    if (x == nullptr)
        return -ENODATA;
    if (size == 0)
        return (ssize_t)x->size;
    if (size < x->size)
        return -ERANGE;
    memcpy(value, x->value, x->size);
    return (ssize_t)x->size;
}

static int memfd_setxattr(inode_t* node, const char* name, const void* value, const size_t size, const int flags)
{
    if (!xattr_name_ok(name))
        return -EOPNOTSUPP;
    struct memfd* m = node->private_data;
    struct xattr* x = xattr_find(m, name, nullptr);
    if (x && (flags & XATTR_CREATE))
        return -EEXIST;
    if (!x && (flags & XATTR_REPLACE))
        return -ENODATA;

    void* copy = nullptr;
    if (size) {
        copy = malloc(size);
        if (copy == nullptr)
            return -ENOMEM;
        memcpy(copy, value, size);
    }
    if (x == nullptr) {
        const size_t len = strlen(name);
        x = malloc(sizeof(struct xattr) + len + 1);
        if (x == nullptr) {
            free(copy);
            return -ENOMEM;
        }
        memcpy(x->name, name, len + 1);
        x->value = nullptr;
        x->next = m->xattrs;
        m->xattrs = x;
    }
    free(x->value);
    x->value = copy;
    x->size = size;
    return 0;
}

static ssize_t memfd_listxattr(inode_t* node, char* list, const size_t size)
{
    const struct memfd* m = node->private_data;
    size_t total = 0;
    for (const struct xattr* x = m->xattrs; x; x = x->next)
        total += strlen(x->name) + 1;
    if (size == 0)
        return (ssize_t)total;
    if (size < total)
        return -ERANGE;
    for (const struct xattr* x = m->xattrs; x; x = x->next) {
        const size_t len = strlen(x->name) + 1;
        memcpy(list, x->name, len);
        list += len;
    }
    return (ssize_t)total;
}

static int memfd_removexattr(inode_t* node, const char* name)
{
    if (!xattr_name_ok(name))
        return -EOPNOTSUPP;
    struct xattr** link;
    struct xattr* x = xattr_find(node->private_data, name, &link);
    if (x == nullptr)
        return -ENODATA;
    *link = x->next;
    free(x->value);
    free(x);
    return 0;
}

static void mem_inode_free(inode_t* node)
{
    struct memfd* m = node->private_data;
    while (m->xattrs) {
        struct xattr* x = m->xattrs;
        m->xattrs = x->next;
        free(x->value);
        free(x);
    }
    for (size_t i = 0; i < m->page_count; i++)
        if (m->pages[i])
            free_physical_page(m->pages[i]);
    free(m->pages);
    free(m);
    free(node);
}

static int memfd_open(struct inode_s* node, struct file_s* f)
{
    (void)f;
    ((struct memfd*)node->private_data)->opens++;
    return 0;
}

static int memfd_close(struct file_s* f)
{
    struct memfd* m = memfd_of(f);
    if (--m->opens == 0 && m->unlinked && f->mount == nullptr)
        mem_inode_free(f->node);
    return 0;
}

static void memfd_release(inode_t* node)
{
    const struct memfd* m = node->private_data;
    if (m->opens == 0 && m->unlinked)
        mem_inode_free(node);
}

static inode_operations_t memfd_iops = {
    .release = memfd_release,
    .getxattr = memfd_getxattr,
    .setxattr = memfd_setxattr,
    .listxattr = memfd_listxattr,
    .removexattr = memfd_removexattr,
};

static file_operations_t memfd_fops = {
    .open = memfd_open,
    .read = memfd_read,
    .write = memfd_write,
    .truncate = memfd_truncate,
    .fallocate = memfd_fallocate,
    .mmap_page = memfd_mmap_page,
    .close = memfd_close,
};

inode_t* mem_inode_create()
{
    struct memfd* m = malloc(sizeof(struct memfd));
    inode_t* node = malloc(sizeof(inode_t));
    if (!m || !node) {
        free(m);
        free(node);
        return nullptr;
    }
    memset(m, 0, sizeof(*m));
    memset(node, 0, sizeof(*node));
    node->id = next_ino++;
    node->type = INODE_FILE;
    node->fops = &memfd_fops;
    node->iops = &memfd_iops;
    node->private_data = m;
    return node;
}

void mem_inode_unlink(inode_t* node)
{
    struct memfd* m = node->private_data;
    m->unlinked = true;
    if (m->opens == 0)
        mem_inode_free(node);
}

int vfs_memfd_create(const char* name)
{
    inode_t* node = mem_inode_create();
    struct file_s* f = malloc(sizeof(struct file_s));
    if (!node || !f) {
        if (node)
            mem_inode_free(node);
        free(f);
        return -ENOMEM;
    }
    struct memfd* m = node->private_data;
    m->opens = 1;
    m->unlinked = true;
    memset(f, 0, sizeof(*f));
    f->node = node;
    f->flags = O_RDONLY | O_WRONLY;
    f->ref_count = 1;
    strncpy(f->filename, MEMFD_PREFIX, sizeof(f->filename) - 1);
    strncpy(f->filename + sizeof(MEMFD_PREFIX) - 1, name, sizeof(f->filename) - sizeof(MEMFD_PREFIX));
    if (vfs_install(f) < 0) {
        mem_inode_free(node);
        free(f);
        return -ENFILE;
    }
    return (int)f->id;
}
