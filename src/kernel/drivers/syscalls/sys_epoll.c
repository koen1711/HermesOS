#include "sys.h"

#include "fd.h"
#include "uaccess.h"
#include <drivers/fs/vfs/vfs.h>
#include <drivers/scheduler/process.h>
#include <hardware/memory/alloc.h>
#include <utils/queue/queue.h>
#include <os/errno.h>

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_CLOEXEC 0x80000
#define EPOLLRDHUP 0x2000
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP (1U << 29)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET (1U << 31)
#define EPOLL_FLAGS (EPOLLEXCLUSIVE | EPOLLWAKEUP | EPOLLONESHOT | EPOLLET)
#define EPOLL_MAX_EVENTS 4096
#define EPOLL_MAX_DEPTH 4

#define EFD_SEMAPHORE 0x1
#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x80000
#define EVENTFD_MAX 0xFFFFFFFFFFFFFFFEULL

struct epoll_event_k {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct epoll_entry {
    struct dlist_node link;
    int fd;
    struct file_s* file;
    uint32_t events;
    uint64_t data;
    uint32_t reported;
    bool disabled;
};

struct epoll {
    struct dlist entries;
};

struct epoll_scan {
    struct epoll* ep;
    struct epoll_event_k* out;
    int max;
};

struct eventfd {
    uint64_t count;
    bool semaphore;
};

static file_operations_t epoll_fops;

static struct epoll* epoll_of(const struct file_s* f)
{
    return f->node->fops == &epoll_fops ? f->node->private_data : nullptr;
}

static struct epoll_entry* entry_of(const struct dlist_node* node)
{
    return dlist_entry(node, struct epoll_entry, link);
}

static struct epoll_entry* epoll_find(const struct epoll* ep, const int fd, const struct file_s* f)
{
    for (struct dlist_node* n = dlist_first(&ep->entries); n; n = dlist_next(n)) {
        struct epoll_entry* e = entry_of(n);
        if (e->fd == fd && e->file == f)
            return e;
    }
    return nullptr;
}

static uint32_t entry_ready(const struct epoll_entry* e)
{
    int ready = poll_file(e->file);
    if (ready & VFS_POLLHUP)
        ready |= EPOLLRDHUP;
    return (uint32_t)ready & (e->events | VFS_POLLERR | VFS_POLLHUP) & ~EPOLL_FLAGS;
}

static int epoll_collect(struct epoll* ep, struct epoll_event_k* out, const int max, const bool consume)
{
    int count = 0;
    const struct dlist_node* n = dlist_first(&ep->entries);
    while (n && count < max) {
        const struct dlist_node* next = dlist_next(n);
        struct epoll_entry* e = entry_of(n);
        n = next;
        if (fd_get(e->fd) != e->file) {
            if (consume) {
                dlist_remove(&ep->entries, &e->link);
                free(e);
            }
            continue;
        }
        if (e->disabled)
            continue;

        const uint32_t ready = entry_ready(e);
        uint32_t report = ready;
        if (e->events & EPOLLET) {
            const uint32_t fresh = ready & ~e->reported;
            if (consume)
                e->reported = ready;
            report = fresh ? ready : 0;
        }
        if (!report)
            continue;
        if (out)
            out[count] = (struct epoll_event_k){ .events = report, .data = e->data };
        count++;
        if (consume && (e->events & EPOLLONESHOT))
            e->disabled = true;
    }
    return count;
}

static long epoll_scan(void* ctx)
{
    const struct epoll_scan* s = ctx;
    return epoll_collect(s->ep, s->out, s->max, true);
}

static int epoll_poll(struct file_s* f)
{
    return epoll_collect(epoll_of(f), nullptr, 1, false) ? VFS_POLLIN : 0;
}

static int epoll_close(struct file_s* f)
{
    struct epoll* ep = epoll_of(f);
    while (!dlist_empty(&ep->entries)) {
        struct dlist_node* n = dlist_first(&ep->entries);
        dlist_remove(&ep->entries, n);
        free(entry_of(n));
    }
    free(ep);
    free(f->node);
    return 0;
}

static file_operations_t epoll_fops = {
    .poll = epoll_poll,
    .close = epoll_close,
};

static long anon_install(file_operations_t* fops, void* data, const bool cloexec, const int status_flags)
{
    inode_t* node = malloc(sizeof(inode_t));
    struct file_s* f = malloc(sizeof(struct file_s));
    if (!node || !f) {
        free(node);
        free(f);
        return -ENOMEM;
    }
    memset(node, 0, sizeof(*node));
    node->type = INODE_ANON;
    node->fops = fops;
    node->private_data = data;
    memset(f, 0, sizeof(*f));
    f->node = node;
    f->flags = O_RDONLY | O_WRONLY;
    f->status_flags = status_flags;
    f->ref_count = 1;
    if (vfs_install(f) < 0) {
        free(node);
        free(f);
        return -ENFILE;
    }
    const int fd = fd_install_from(f, 0, cloexec);
    if (fd < 0)
        vfs_close((int)f->id);
    return fd;
}

long sys_epoll_create1(const int flags)
{
    if (flags & ~EPOLL_CLOEXEC)
        return -EINVAL;
    struct epoll* ep = malloc(sizeof(struct epoll));
    if (!ep)
        return -ENOMEM;
    dlist_init(&ep->entries);
    const long fd = anon_install(&epoll_fops, ep, flags & EPOLL_CLOEXEC, 0);
    if (fd < 0)
        free(ep);
    return fd;
}

static bool epoll_loops(const struct epoll* ep, const struct epoll* target, const int depth)
{
    if (target == ep)
        return true;
    if (depth >= EPOLL_MAX_DEPTH)
        return true;
    for (struct dlist_node* n = dlist_first(&target->entries); n; n = dlist_next(n)) {
        const struct epoll_entry* e = entry_of(n);
        if (fd_get(e->fd) != e->file)
            continue;
        const struct epoll* inner = epoll_of(e->file);
        if (inner && epoll_loops(ep, inner, depth + 1))
            return true;
    }
    return false;
}

long sys_epoll_ctl(const int epfd, const int op, const int fd, const void* uevent)
{
    const struct file_s* epf = fd_get(epfd);
    struct file_s* f = fd_get(fd);
    if (!epf || !f)
        return -EBADF;
    struct epoll* ep = epoll_of(epf);
    if (!ep || epfd == fd)
        return -EINVAL;
    if (f->node->type == INODE_FILE || f->node->type == INODE_DIR)
        return -EPERM;

    struct epoll_event_k ev = {0};
    if (op != EPOLL_CTL_DEL && copy_from_user(&ev, uevent, sizeof(ev)) < 0)
        return -EFAULT;
    struct epoll_entry* e = epoll_find(ep, fd, f);

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (e)
            return -EEXIST;
        const struct epoll* inner = epoll_of(f);
        if (inner && epoll_loops(ep, inner, 0))
            return -ELOOP;
        e = malloc(sizeof(struct epoll_entry));
        if (!e)
            return -ENOMEM;
        memset(e, 0, sizeof(*e));
        e->fd = fd;
        e->file = f;
        e->events = ev.events;
        e->data = ev.data;
        dlist_insert_tail(&ep->entries, &e->link);
        break;
    }
    case EPOLL_CTL_MOD:
        if (!e)
            return -ENOENT;
        if ((ev.events & EPOLLEXCLUSIVE) || (e->events & EPOLLEXCLUSIVE))
            return -EINVAL;
        e->events = ev.events;
        e->data = ev.data;
        e->reported = 0;
        e->disabled = false;
        break;
    case EPOLL_CTL_DEL:
        if (!e)
            return -ENOENT;
        dlist_remove(&ep->entries, &e->link);
        free(e);
        break;
    default:
        return -EINVAL;
    }
    process_wakeup(WAIT_CHANNEL_POLL);
    return 0;
}

long sys_epoll_pwait(const int epfd, void* uevents, const int maxevents, const int64_t timeout_ns,
                     const uint64_t* sigmask, const size_t sigsetsize)
{
    const struct file_s* epf = fd_get(epfd);
    if (!epf)
        return -EBADF;
    struct epoll* ep = epoll_of(epf);
    if (!ep || maxevents <= 0)
        return -EINVAL;
    const int max = maxevents < EPOLL_MAX_EVENTS ? maxevents : EPOLL_MAX_EVENTS;
    if (!uaccess_ok_write(uevents, max * sizeof(struct epoll_event_k)))
        return -EFAULT;

    struct epoll_event_k* out = malloc(max * sizeof(struct epoll_event_k));
    if (!out)
        return -ENOMEM;
    struct epoll_scan scan = { ep, out, max };
    long r = poll_wait_sigmask(sigmask, sigsetsize, epoll_scan, &scan, timeout_ns);
    if (r > 0 && copy_to_user(uevents, out, r * sizeof(struct epoll_event_k)) < 0)
        r = -EFAULT;
    free(out);
    return r;
}

long sys_epoll_pwait2(const int epfd, void* uevents, const int maxevents, const struct timespec_k* timeout,
                      const uint64_t* sigmask, const size_t sigsetsize)
{
    int64_t ns;
    const long err = poll_timespec_ns(timeout, &ns);
    return err ? err : sys_epoll_pwait(epfd, uevents, maxevents, ns, sigmask, sigsetsize);
}

static struct eventfd* eventfd_of(const struct file_s* f)
{
    return f->node->private_data;
}

static int eventfd_wait_id(const struct eventfd* e)
{
    return (int)(((uintptr_t)e >> 4) & 0x0FFFFFFF) | 0x58000000;
}

static void eventfd_notify(const struct eventfd* e)
{
    process_wakeup(eventfd_wait_id(e));
    process_wakeup(WAIT_CHANNEL_POLL);
}

static ssize_t eventfd_read(struct file_s* f, void* buf, const size_t len)
{
    struct eventfd* e = eventfd_of(f);
    if (len < sizeof(uint64_t))
        return -EINVAL;
    while (e->count == 0) {
        if (f->status_flags & EFD_NONBLOCK)
            return -EAGAIN;
        if (process_sleep_interruptible(eventfd_wait_id(e)) < 0)
            return -ERESTARTSYS;
    }
    const uint64_t value = e->semaphore ? 1 : e->count;
    e->count -= value;
    memcpy(buf, &value, sizeof(value));
    eventfd_notify(e);
    return sizeof(value);
}

static ssize_t eventfd_write(struct file_s* f, const void* buf, const size_t len)
{
    struct eventfd* e = eventfd_of(f);
    if (len < sizeof(uint64_t))
        return -EINVAL;
    uint64_t value;
    memcpy(&value, buf, sizeof(value));
    if (value == ~0ULL)
        return -EINVAL;
    while (EVENTFD_MAX - e->count < value) {
        if (f->status_flags & EFD_NONBLOCK)
            return -EAGAIN;
        if (process_sleep_interruptible(eventfd_wait_id(e)) < 0)
            return -ERESTARTSYS;
    }
    e->count += value;
    eventfd_notify(e);
    return sizeof(value);
}

static int eventfd_poll(struct file_s* f)
{
    const struct eventfd* e = eventfd_of(f);
    return (e->count ? VFS_POLLIN : 0) | (e->count < EVENTFD_MAX ? VFS_POLLOUT : 0);
}

static int eventfd_close(struct file_s* f)
{
    free(eventfd_of(f));
    free(f->node);
    return 0;
}

static file_operations_t eventfd_fops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .poll = eventfd_poll,
    .close = eventfd_close,
};

long sys_eventfd2(const unsigned int initval, const int flags)
{
    if (flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC))
        return -EINVAL;
    struct eventfd* e = malloc(sizeof(struct eventfd));
    if (!e)
        return -ENOMEM;
    e->count = initval;
    e->semaphore = flags & EFD_SEMAPHORE;
    const long fd = anon_install(&eventfd_fops, e, flags & EFD_CLOEXEC, flags & EFD_NONBLOCK);
    if (fd < 0)
        free(e);
    return fd;
}
