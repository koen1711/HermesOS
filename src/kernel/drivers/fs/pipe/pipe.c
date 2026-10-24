#include "pipe.h"

#include <drivers/fs/vfs/vfs.h>
#include <drivers/scheduler/process.h>
#include <drivers/scheduler/signal.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

#define O_NONBLOCK_K 0x800

struct pipe {
    uint8_t* buf;
    size_t head;
    size_t len;
    int readers;
    int writers;
};

static struct pipe* pipe_of(const struct file_s* f)
{
    return f->node->private_data;
}

static int pipe_wait_id(const struct pipe* p)
{
    return (int)(((uintptr_t)p >> 4) & 0x0FFFFFFF) | 0x50000000;
}

static void pipe_notify(const struct pipe* p)
{
    process_wakeup(pipe_wait_id(p));
    process_wakeup(WAIT_CHANNEL_POLL);
}

static ssize_t pipe_read(struct file_s* f, void* buf, const size_t len)
{
    struct pipe* p = pipe_of(f);
    while (p->len == 0) {
        if (p->writers == 0 || len == 0)
            return 0;
        if (f->status_flags & O_NONBLOCK_K)
            return -EAGAIN;
        if (process_sleep_interruptible(pipe_wait_id(p)) < 0)
            return -ERESTARTSYS;
    }
    const size_t n = p->len < len ? p->len : len;
    for (size_t i = 0; i < n; i++)
        ((uint8_t*)buf)[i] = p->buf[(p->head + i) % PIPE_BUFFER_SIZE];
    p->head = (p->head + n) % PIPE_BUFFER_SIZE;
    p->len -= n;
    pipe_notify(p);
    return (ssize_t)n;
}

static ssize_t pipe_write(struct file_s* f, const void* buf, const size_t len)
{
    struct pipe* p = pipe_of(f);
    size_t written = 0;
    while (written < len) {
        if (p->readers == 0) {
            signal_send(scheduler_get()->current->tgid, SIGPIPE);
            return written ? (ssize_t)written : -EPIPE;
        }
        const size_t space = PIPE_BUFFER_SIZE - p->len;

        const size_t need = len - written <= PIPE_ATOMIC ? len - written : 1;
        if (space < need) {
            if (f->status_flags & O_NONBLOCK_K)
                return written ? (ssize_t)written : -EAGAIN;
            if (process_sleep_interruptible(pipe_wait_id(p)) < 0)
                return written ? (ssize_t)written : -ERESTARTSYS;
            continue;
        }
        const size_t n = len - written < space ? len - written : space;
        for (size_t i = 0; i < n; i++)
            p->buf[(p->head + p->len + i) % PIPE_BUFFER_SIZE] = ((const uint8_t*)buf)[written + i];
        p->len += n;
        written += n;
        pipe_notify(p);
    }
    return (ssize_t)written;
}

static int pipe_close(struct file_s* f)
{
    struct pipe* p = pipe_of(f);
    if (f->flags & O_RDONLY)
        p->readers--;
    else
        p->writers--;
    pipe_notify(p);
    if (p->readers == 0 && p->writers == 0) {
        free(p->buf);
        free(p);
    }
    free(f->node);
    return 0;
}

static int pipe_poll(struct file_s* f)
{
    const struct pipe* p = pipe_of(f);
    if (f->flags & O_RDONLY)
        return (p->len ? VFS_POLLIN : 0) | (p->writers == 0 ? VFS_POLLHUP : 0);
    return (PIPE_BUFFER_SIZE - p->len >= PIPE_ATOMIC ? VFS_POLLOUT : 0) | (p->readers == 0 ? VFS_POLLERR : 0);
}

static file_operations_t pipe_fops = {
    .read = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
    .poll = pipe_poll,
};

static struct file_s* pipe_end(struct pipe* p, const int flags)
{
    inode_t* node = malloc(sizeof(inode_t));
    struct file_s* f = malloc(sizeof(struct file_s));
    if (!node || !f) {
        free(node);
        free(f);
        return nullptr;
    }
    memset(node, 0, sizeof(*node));
    node->type = INODE_PIPE;
    node->fops = &pipe_fops;
    node->private_data = p;
    memset(f, 0, sizeof(*f));
    f->node = node;
    f->flags = flags;
    f->ref_count = 1;
    if (vfs_install(f) < 0) {
        free(node);
        free(f);
        return nullptr;
    }
    return f;
}

int pipe_create(struct file_s** read_end, struct file_s** write_end)
{
    struct pipe* p = malloc(sizeof(struct pipe));
    if (!p)
        return -ENOMEM;
    memset(p, 0, sizeof(*p));
    p->buf = malloc(PIPE_BUFFER_SIZE);
    if (!p->buf) {
        free(p);
        return -ENOMEM;
    }
    p->readers = p->writers = 1;

    *read_end = pipe_end(p, O_RDONLY);
    *write_end = *read_end ? pipe_end(p, O_WRONLY) : nullptr;
    if (!*write_end) {
        if (*read_end) {
            p->writers = 0;
            vfs_close((int)(*read_end)->id);
        } else {
            free(p->buf);
            free(p);
        }
        return -ENFILE;
    }
    return 0;
}
