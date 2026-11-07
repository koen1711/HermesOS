#include "sys.h"

#include "fd.h"
#include "uaccess.h"
#include <drivers/fs/vfs/vfs.h>
#include <drivers/fs/vfs/poll_head.h>
#include <drivers/net/core/netisr.h>
#include <drivers/scheduler/process.h>
#include <drivers/timer/callout.h>
#include <drivers/timer/clock.h>
#include <hardware/memory/alloc.h>
#include <hardware/smp/smp.h>
#include <utils/sync/spinlock.h>
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
#define EPOLL_LEGACY_RECHECK_MS 10

#define EFD_SEMAPHORE 0x1
#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x80000
#define EVENTFD_MAX 0xFFFFFFFFFFFFFFFEULL
#define PIDFD_NONBLOCK 0x800

struct epoll_event_k {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct epoll;

struct epoll_entry {
    struct dlist_node link;
    struct dlist_node rdlink;
    struct dlist_node flink;
    struct poll_waiter w;
    struct epoll* ep;
    struct poll_head* ph;
    int fd;
    struct file_s* file;
    uint32_t events;
    uint64_t data;
    uint32_t reported;
    bool on_rdlist;
    bool disabled;
    bool unsafe;
};

struct epoll {
    struct dlist entries;
    struct dlist rdlist;
    struct dlist legacy;
    struct dlist waiters;
    struct poll_head ph;
    spinlock_t lock;
    spinlock_t rdlock;
    int unsafe;
};

struct epoll_wait_rec {
    struct dlist_node link;
    process_t* proc;
    bool queued;
};

struct epoll_scan {
    struct epoll* ep;
    struct epoll_event_k* out;
    int max;
};

struct eventfd {
    uint64_t count;
    bool semaphore;
    struct poll_head ph;
};

static file_operations_t epoll_fops;
static spinlock_t epoll_global_lock = { .name = "epoll_global" };

static struct epoll* epoll_of(const struct file_s* f)
{
    return f->node->fops == &epoll_fops ? f->node->private_data : nullptr;
}

static struct epoll_entry* entry_of(const struct dlist_node* node)
{
    return dlist_entry(node, struct epoll_entry, link);
}

static struct epoll_entry* rd_entry_of(const struct dlist_node* node)
{
    return dlist_entry(node, struct epoll_entry, rdlink);
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

static int ep_peek_nested(struct epoll* inner, struct file_s** deferred);

static uint32_t entry_ready(const struct epoll_entry* e, struct file_s** deferred)
{
    struct epoll* inner = epoll_of(e->file);
    int ready = inner ? (ep_peek_nested(inner, deferred) ? VFS_POLLIN : 0) : poll_file(e->file);
    if (ready & VFS_POLLHUP)
        ready |= EPOLLRDHUP;
    return (uint32_t)ready & (e->events | VFS_POLLERR | VFS_POLLHUP) & ~EPOLL_FLAGS;
}

static bool file_poll_mpsafe(const struct file_s* f)
{
    const file_operations_t* fops = f->node->fops;
    return !fops || !fops->poll || fops->poll_mpsafe;
}

static void file_unpin(struct file_s* f, struct file_s** deferred)
{
    if (f && __atomic_sub_fetch(&f->ref_count, 1, __ATOMIC_ACQ_REL) == 0) {
        f->deferred_next = *deferred;
        *deferred = f;
    }
}

static bool ep_kick_locked(struct epoll* ep)
{
    struct dlist_node* n = dlist_first(&ep->waiters);
    if (!n)
        return false;
    dlist_remove(&ep->waiters, n);
    struct epoll_wait_rec* rec = dlist_entry(n, struct epoll_wait_rec, link);
    rec->queued = false;
    process_poll_kick(rec->proc);
    return true;
}

static void ep_queue_locked(struct epoll_entry* e)
{
    if (e->on_rdlist || e->disabled)
        return;
    e->on_rdlist = true;
    dlist_insert_tail(&e->ep->rdlist, &e->rdlink);
}

static bool ep_poll_callback(struct poll_waiter* w, const uint32_t events)
{
    (void)events;
    struct epoll_entry* e = dlist_entry(w, struct epoll_entry, w);
    struct epoll* ep = e->ep;
    uint64_t flags;
    spinlock_acquire_irqsave(&ep->rdlock, &flags);
    if (e->disabled) {
        spinlock_release_irqrestore(&ep->rdlock, flags);
        return false;
    }
    ep_queue_locked(e);
    const bool woke = ep_kick_locked(ep);
    spinlock_release_irqrestore(&ep->rdlock, flags);
    poll_head_wake(&ep->ph, VFS_POLLIN);
    return woke;
}

static void ep_entry_free(struct epoll* ep, struct epoll_entry* e)
{
    if (e->ph) {
        poll_head_remove(e->ph, &e->w);
        uint64_t flags;
        spinlock_acquire_irqsave(&ep->rdlock, &flags);
        if (e->on_rdlist)
            dlist_remove(&ep->rdlist, &e->rdlink);
        spinlock_release_irqrestore(&ep->rdlock, flags);
    } else {
        dlist_remove(&ep->legacy, &e->rdlink);
    }
    dlist_remove(&e->file->ep_links, &e->flink);
    dlist_remove(&ep->entries, &e->link);
    ep->unsafe -= e->unsafe;
    free(e);
}

void epoll_file_release(struct file_s* f)
{
    if (dlist_empty(&f->ep_links))
        return;
    spinlock_acquire(&epoll_global_lock);
    while (!dlist_empty(&f->ep_links)) {
        struct epoll_entry* e = dlist_entry(dlist_first(&f->ep_links), struct epoll_entry, flink);
        struct epoll* ep = e->ep;
        spinlock_acquire(&ep->lock);
        ep_entry_free(ep, e);
        spinlock_release(&ep->lock);
    }
    spinlock_release(&epoll_global_lock);
}

static int ep_harvest_ready(struct epoll* ep, struct epoll_event_k* out, const int max, const bool consume,
                            struct file_s** deferred)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&ep->rdlock, &flags);
    struct dlist batch = ep->rdlist;
    dlist_init(&ep->rdlist);
    spinlock_release_irqrestore(&ep->rdlock, flags);

    int count = 0;
    while (!dlist_empty(&batch) && count < max) {
        struct epoll_entry* e = rd_entry_of(dlist_first(&batch));
        spinlock_acquire_irqsave(&ep->rdlock, &flags);
        dlist_remove(&batch, &e->rdlink);
        e->on_rdlist = false;
        spinlock_release_irqrestore(&ep->rdlock, flags);

        struct file_s* pinned = fd_pin(e->fd);
        if (pinned != e->file || e->disabled) {
            file_unpin(pinned, deferred);
            continue;
        }
        const uint32_t ready = entry_ready(e, deferred);
        file_unpin(pinned, deferred);
        if (!ready)
            continue;

        if (out)
            out[count] = (struct epoll_event_k){ .events = ready, .data = e->data };
        count++;
        if (consume && (e->events & EPOLLONESHOT)) {
            e->disabled = true;
        } else if (!consume || !(e->events & EPOLLET)) {
            spinlock_acquire_irqsave(&ep->rdlock, &flags);
            ep_queue_locked(e);
            spinlock_release_irqrestore(&ep->rdlock, flags);
        }
    }

    if (!dlist_empty(&batch)) {
        spinlock_acquire_irqsave(&ep->rdlock, &flags);
        while (!dlist_empty(&batch)) {
            struct dlist_node* n = dlist_last(&batch);
            dlist_remove(&batch, n);
            dlist_insert_head(&ep->rdlist, n);
        }
        spinlock_release_irqrestore(&ep->rdlock, flags);
    }
    return count;
}

static int ep_harvest_legacy(struct epoll* ep, struct epoll_event_k* out, const int max, const bool consume,
                             struct file_s** deferred)
{
    int count = 0;
    for (const struct dlist_node* n = dlist_first(&ep->legacy); n && count < max; n = dlist_next(n)) {
        struct epoll_entry* e = rd_entry_of(n);
        struct file_s* pinned = fd_pin(e->fd);
        if (pinned != e->file || e->disabled) {
            file_unpin(pinned, deferred);
            continue;
        }
        const uint32_t ready = entry_ready(e, deferred);
        file_unpin(pinned, deferred);
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
        if (consume && e->events & EPOLLONESHOT)
            e->disabled = true;
    }
    return count;
}

static int ep_collect_locked(struct epoll* ep, struct epoll_event_k* out, const int max, const bool consume,
                             struct file_s** deferred)
{
    int count = ep_harvest_ready(ep, out, max, consume, deferred);
    if (count < max && !dlist_empty(&ep->legacy))
        count += ep_harvest_legacy(ep, out ? out + count : nullptr, max - count, consume, deferred);
    return count;
}

static int ep_peek_nested(struct epoll* inner, struct file_s** deferred)
{
    spinlock_acquire(&inner->lock);
    const int count = ep_collect_locked(inner, nullptr, 1, false, deferred);
    spinlock_release(&inner->lock);
    return count;
}

static int epoll_collect(struct epoll* ep, struct epoll_event_k* out, const int max, const bool consume)
{
    bool kernel_locked = false;
    struct file_s* deferred = nullptr;

    spinlock_acquire(&ep->lock);
    if (ep->unsafe) {
        spinlock_release(&ep->lock);
        kernel_locked = kernel_lock_enter();
        spinlock_acquire(&ep->lock);
    }
    const int count = ep_collect_locked(ep, out, max, consume, &deferred);
    spinlock_release(&ep->lock);

    while (deferred) {
        struct file_s* f = deferred;
        deferred = f->deferred_next;
        vfs_file_release(f);
    }
    if (kernel_locked)
        kernel_lock_exit();
    return count;
}

static void ep_wait_register(struct epoll* ep, struct epoll_wait_rec* rec)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&ep->rdlock, &flags);
    if (!rec->queued) {
        rec->queued = true;
        dlist_insert_tail(&ep->waiters, &rec->link);
    }
    spinlock_release_irqrestore(&ep->rdlock, flags);
}

static void ep_wait_unregister(struct epoll* ep, struct epoll_wait_rec* rec)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&ep->rdlock, &flags);
    if (rec->queued) {
        rec->queued = false;
        dlist_remove(&ep->waiters, &rec->link);
    }
    if (!dlist_empty(&ep->rdlist))
        ep_kick_locked(ep);
    spinlock_release_irqrestore(&ep->rdlock, flags);
}

static long epoll_wait_loop(void* ctx, const int64_t timeout_ns)
{
    const struct epoll_scan* s = ctx;
    struct epoll* ep = s->ep;
    process_t* current = scheduler_get()->current;
    struct epoll_wait_rec rec = { .proc = current, .queued = false };
    const uint64_t start = clock_monotonic_ns();
    long r;
    for (;;) {
        netisr_poll();
        const bool legacy = !dlist_empty(&ep->legacy);
        uint64_t seq = legacy ? process_poll_legacy_enter() : 0;
        ep_wait_register(ep, &rec);
        r = epoll_collect(ep, s->out, s->max, true);
        if (r || timeout_ns == 0) {
            if (legacy)
                process_poll_legacy_exit();
            break;
        }

        uint64_t wait = legacy ? ticks_from_ms(EPOLL_LEGACY_RECHECK_MS) : 0;
        if (timeout_ns > 0) {
            const uint64_t elapsed = clock_monotonic_ns() - start;
            if (elapsed >= (uint64_t)timeout_ns) {
                if (legacy)
                    process_poll_legacy_exit();
                break;
            }
            uint64_t left = clock_ticks_from_ns((uint64_t)timeout_ns - elapsed);
            if (left == 0)
                left = 1;
            if (wait == 0 || left < wait)
                wait = left;
        }

        const int err = process_sleep_poll(legacy ? &seq : nullptr, wait);
        if (legacy)
            process_poll_legacy_exit();
        if (err == -EINTR) {
            r = -EINTR;
            break;
        }
    }
    ep_wait_unregister(ep, &rec);
    __atomic_store_n(&current->poll_pending, false, __ATOMIC_RELAXED);
    return r;
}

static int epoll_poll(struct file_s* f)
{
    return epoll_collect(epoll_of(f), nullptr, 1, false) ? VFS_POLLIN : 0;
}

static struct poll_head* epoll_poll_head(struct file_s* f)
{
    return &epoll_of(f)->ph;
}

static int epoll_close(struct file_s* f)
{
    struct epoll* ep = epoll_of(f);
    spinlock_acquire(&epoll_global_lock);
    spinlock_acquire(&ep->lock);
    while (!dlist_empty(&ep->entries))
        ep_entry_free(ep, entry_of(dlist_first(&ep->entries)));
    spinlock_release(&ep->lock);
    spinlock_release(&epoll_global_lock);
    free(ep);
    free(f->node);
    return 0;
}

static file_operations_t epoll_fops = {
    .poll = epoll_poll,
    .poll_head = epoll_poll_head,
    .close = epoll_close,
};

long anon_install(file_operations_t* fops, void* data, const bool cloexec, const int status_flags)
{
    inode_t* node = malloc(sizeof(inode_t));
    struct file_s* f = malloc(sizeof(struct file_s));
    if (!node || !f) {
        free(node);
        free(f);
        return -ENOMEM;
    }
    memset(node, 0, offsetof(inode_t, path) + 1);
    memset(f, 0, offsetof(struct file_s, filename) + 1);

    node->type = INODE_ANON;
    node->fops = fops;
    node->private_data = data;

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
    dlist_init(&ep->rdlist);
    dlist_init(&ep->legacy);
    dlist_init(&ep->waiters);
    poll_head_init(&ep->ph);
    spinlock_init(&ep->lock, "epoll");
    spinlock_init(&ep->rdlock, "epoll_rd");
    ep->unsafe = 0;
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
    spinlock_acquire((spinlock_t*)&target->lock);
    bool found = false;
    for (const struct dlist_node* n = dlist_first(&target->entries); n && !found; n = dlist_next(n)) {
        const struct epoll_entry* e = entry_of(n);
        if (fd_get(e->fd) != e->file)
            continue;
        const struct epoll* inner = epoll_of(e->file);
        found = inner && epoll_loops(ep, inner, depth + 1);
    }
    spinlock_release((spinlock_t*)&target->lock);
    return found;
}

static uint32_t waiter_events(const uint32_t events)
{
    return (events & ~EPOLL_FLAGS) | VFS_POLLERR | VFS_POLLHUP;
}

static void ep_arm(struct epoll* ep, struct epoll_entry* e)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&ep->rdlock, &flags);
    if (e->ph) {
        ep_queue_locked(e);
        ep_kick_locked(ep);
    } else {
        while (ep_kick_locked(ep)) {
        }
    }
    spinlock_release_irqrestore(&ep->rdlock, flags);
    if (e->ph)
        poll_head_wake(&ep->ph, VFS_POLLIN);
    else
        process_wakeup_poll();
}

static long epoll_ctl_locked(struct epoll* ep, const int op, const int fd, struct file_s* f,
                             const struct epoll_event_k* ev)
{
    struct epoll_entry* e = epoll_find(ep, fd, f);
    switch (op) {
    case EPOLL_CTL_ADD: {
        if (e)
            return -EEXIST;
        const struct epoll* inner = epoll_of(f);
        if (inner && epoll_loops(ep, inner, 0))
            return -ELOOP;
        e = malloc_safe(sizeof(struct epoll_entry));
        if (!e)
            return -ENOMEM;

        const file_operations_t* fops = f->node->fops;
        e->ep = ep;
        e->ph = fops && fops->poll_head ? fops->poll_head(f) : nullptr;
        e->fd = fd;
        e->file = f;
        e->events = ev->events;
        e->data = ev->data;
        e->reported = 0;
        e->on_rdlist = false;
        e->disabled = false;
        e->unsafe = !file_poll_mpsafe(f);
        ep->unsafe += e->unsafe;
        dlist_insert_tail(&ep->entries, &e->link);
        dlist_insert_tail(&f->ep_links, &e->flink);
        if (e->ph) {
            e->w.events = waiter_events(ev->events);
            e->w.exclusive = ev->events & EPOLLEXCLUSIVE;
            e->w.func = ep_poll_callback;
            poll_head_add(e->ph, &e->w);
        } else {
            dlist_insert_tail(&ep->legacy, &e->rdlink);
        }
        ep_arm(ep, e);
        return 0;
    }
    case EPOLL_CTL_MOD:
        if (!e)
            return -ENOENT;
        if ((ev->events & EPOLLEXCLUSIVE) || (e->events & EPOLLEXCLUSIVE))
            return -EINVAL;
        if (e->ph) {
            uint64_t flags;
            spinlock_acquire_irqsave(&e->ph->lock, &flags);
            e->w.events = waiter_events(ev->events);
            spinlock_release_irqrestore(&e->ph->lock, flags);
        }
        e->events = ev->events;
        e->data = ev->data;
        e->reported = 0;
        e->disabled = false;
        ep_arm(ep, e);
        return 0;
    case EPOLL_CTL_DEL:
        if (!e)
            return -ENOENT;
        ep_entry_free(ep, e);
        return 0;
    default:
        return -EINVAL;
    }
}

long sys_epoll_ctl(const int epfd, const int op, const int fd, const void* uevent)
{
    struct epoll_event_k ev = {0};
    if (op != EPOLL_CTL_DEL && copy_from_user(&ev, uevent, sizeof(ev)) < 0)
        return -EFAULT;

    struct file_s* epf = fd_pin(epfd);
    struct file_s* f = fd_pin(fd);
    long ret;
    struct epoll* ep = epf ? epoll_of(epf) : nullptr;
    if (!epf || !f) {
        ret = -EBADF;
    } else if (!ep || epfd == fd) {
        ret = -EINVAL;
    } else if (f->node->type == INODE_FILE || f->node->type == INODE_DIR) {
        ret = -EPERM;
    } else {
        spinlock_acquire(&epoll_global_lock);
        spinlock_acquire(&ep->lock);
        ret = epoll_ctl_locked(ep, op, fd, f, &ev);
        spinlock_release(&ep->lock);
        spinlock_release(&epoll_global_lock);
    }
    if (f)
        vfs_file_put(f);
    if (epf)
        vfs_file_put(epf);
    return ret;
}

long sys_epoll_pwait(const int epfd, void* uevents, const int maxevents, const int64_t timeout_ns,
                     const uint64_t* sigmask, const size_t sigsetsize)
{
    struct file_s* epf = fd_pin(epfd);
    if (!epf)
        return -EBADF;
    struct epoll* ep = epoll_of(epf);
    const int max = maxevents < EPOLL_MAX_EVENTS ? maxevents : EPOLL_MAX_EVENTS;
    long r;
    if (!ep || maxevents <= 0) {
        r = -EINVAL;
    } else if (!uaccess_ok_write(uevents, max * sizeof(struct epoll_event_k))) {
        r = -EFAULT;
    } else {
        struct epoll_event_k* out = malloc(max * sizeof(struct epoll_event_k));
        if (!out) {
            r = -ENOMEM;
        } else {
            struct epoll_scan scan = { ep, out, max };
            r = poll_wait_sigmask(sigmask, sigsetsize, epoll_wait_loop, &scan, timeout_ns);
            if (r > 0 && copy_to_user(uevents, out, r * sizeof(struct epoll_event_k)) < 0)
                r = -EFAULT;
            free(out);
        }
    }
    vfs_file_put(epf);
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

static void eventfd_notify(struct eventfd* e)
{
    process_wakeup(eventfd_wait_id(e));
    poll_head_wake(&e->ph, VFS_POLLIN | VFS_POLLOUT);
    process_wakeup_poll();
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

static struct poll_head* eventfd_poll_head(struct file_s* f)
{
    return &eventfd_of(f)->ph;
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
    .poll_head = eventfd_poll_head,
    .poll_mpsafe = true,
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
    poll_head_init(&e->ph);
    const long fd = anon_install(&eventfd_fops, e, flags & EFD_CLOEXEC, flags & EFD_NONBLOCK);
    if (fd < 0)
        free(e);
    return fd;
}

static int pidfd_poll(struct file_s* f)
{
    return process_find((int)(intptr_t)f->node->private_data) ? 0 : VFS_POLLIN;
}

static int pidfd_close(struct file_s* f)
{
    free(f->node);
    return 0;
}

static file_operations_t pidfd_fops = {
    .poll = pidfd_poll,
    .close = pidfd_close,
};

int pidfd_pid(const struct file_s* f)
{
    return f->node->fops == &pidfd_fops ? (int)(intptr_t)f->node->private_data : -EBADF;
}

long sys_pidfd_open(const int pid, const unsigned int flags)
{
    if (flags & ~PIDFD_NONBLOCK)
        return -EINVAL;
    if (pid <= 0)
        return -EINVAL;
    const process_t* proc = process_find(pid);
    if (proc == nullptr)
        return -ESRCH;
    if (proc->pid != proc->tgid)
        return -EINVAL;
    return anon_install(&pidfd_fops, (void*)(intptr_t)pid, true, (int)(flags & PIDFD_NONBLOCK));
}
