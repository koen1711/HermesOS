#include "inotify.h"

#include <drivers/fs/vfs/vfs.h>
#include <drivers/scheduler/process.h>
#include <drivers/syscalls/fd.h>
#include <drivers/syscalls/fs/helpers.h>
#include <drivers/syscalls/sys.h>
#include <drivers/syscalls/uaccess.h>
#include <hardware/memory/alloc.h>
#include <utils/queue/queue.h>
#include <utils/str/str.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#define IN_CLOEXEC_K 0x80000
#define IN_NONBLOCK_K 0x800
#define FIONREAD_K 0x541B
#define INOTIFY_QUEUE_BYTES (16 * 1024)
#define INOTIFY_SLEEP_TICKS 10
#define IN_CHILD_ONLY (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)

struct inotify_event_k {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
};

struct inotify_watch {
    struct dlist_node link;
    int wd;
    uint32_t mask;
    char path[VFS_PATH_MAX];
};

struct inotify {
    struct dlist_node link;
    struct dlist watches;
    int next_wd;
    uint8_t* events;
    size_t used;
    size_t last;
    bool has_last;
};

static spinlock_t inotify_lock;
static struct dlist instances;
static int watch_count;
static uint32_t next_cookie;

bool inotify_active(void)
{
    return __atomic_load_n(&watch_count, __ATOMIC_RELAXED) != 0;
}

static int inotify_wait_id(const struct inotify* in)
{
    return (int)(((uintptr_t)in >> 4) & 0x0FFFFFFF) | 0x59000000;
}

static void inotify_wake(const struct inotify* in)
{
    process_wakeup(inotify_wait_id(in));
    process_wakeup_poll(~0ULL);
}

static void queue_append(struct inotify* in, const int wd, const uint32_t mask, const uint32_t cookie,
                         const char* name, const size_t name_size, const size_t padded)
{
    const struct inotify_event_k ev = { wd, mask, cookie, (uint32_t)padded };
    memcpy(in->events + in->used, &ev, sizeof(ev));
    if (padded) {
        memset(in->events + in->used + sizeof(ev), 0, padded);
        memcpy(in->events + in->used + sizeof(ev), name, name_size);
    }
    in->last = in->used;
    in->has_last = true;
    in->used += sizeof(ev) + padded;
}

// One slot of event-header size stays free so an overflow can always be reported
static void queue_event(struct inotify* in, const int wd, const uint32_t mask, const uint32_t cookie, const char* name)
{
    const size_t name_size = name ? strlen(name) + 1 : 0;
    const size_t align = sizeof(struct inotify_event_k);
    const size_t padded = name_size ? (name_size + align - 1) / align * align : 0;

    if (in->has_last) {
        const struct inotify_event_k* last = (const struct inotify_event_k*)(in->events + in->last);
        if (last->wd == wd && last->mask == mask && last->cookie == cookie && last->len == padded
            && (!padded || strcmp((const char*)(last + 1), name) == 0))
            return;
    }
    if (in->used + align + padded + align > INOTIFY_QUEUE_BYTES) {
        const struct inotify_event_k* last = in->has_last ? (const struct inotify_event_k*)(in->events + in->last) : nullptr;
        if (!last || last->mask != IN_Q_OVERFLOW)
            queue_append(in, -1, IN_Q_OVERFLOW, 0, nullptr, 0, 0);
        return;
    }
    queue_append(in, wd, mask, cookie, name, name_size, padded);
}

static void watch_remove(struct inotify* in, struct inotify_watch* w, struct dlist* dead)
{
    queue_event(in, w->wd, IN_IGNORED, 0, nullptr);
    dlist_remove(&in->watches, &w->link);
    dlist_insert_tail(dead, &w->link);
    __atomic_sub_fetch(&watch_count, 1, __ATOMIC_RELAXED);
}

static void free_dead(struct dlist* dead)
{
    struct dlist_node* node;
    while ((node = dlist_first(dead)) != nullptr) {
        dlist_remove(dead, node);
        free(dlist_entry(node, struct inotify_watch, link));
    }
}

static void notify_locked(const char* path, const uint32_t mask, const uint32_t cookie, const bool is_dir, struct dlist* dead)
{
    const char* slash = nullptr;
    for (const char* p = path; *p; p++)
        if (*p == '/')
            slash = p;
    const size_t parent_len = slash == nullptr ? 0 : slash == path ? 1 : (size_t)(slash - path);
    const char* name = slash && slash[1] ? slash + 1 : nullptr;
    const uint32_t dir_flag = is_dir ? IN_ISDIR : 0;

    for (struct dlist_node* n = dlist_first(&instances); n; n = dlist_next(n)) {
        struct inotify* in = dlist_entry(n, struct inotify, link);
        bool fired = false;
        for (struct dlist_node* wn = dlist_first(&in->watches); wn;) {
            struct inotify_watch* w = dlist_entry(wn, struct inotify_watch, link);
            wn = dlist_next(wn);

            const bool self = strcmp(w->path, path) == 0;
            const bool parent = !self && name && strlen(w->path) == parent_len
                && strncmp(w->path, path, parent_len) == 0;
            const uint32_t event = self ? mask & ~IN_CHILD_ONLY : mask;
            if (!(self || parent) || !(event & w->mask))
                continue;
            queue_event(in, w->wd, event | dir_flag, cookie, parent ? name : nullptr);
            if (w->mask & IN_ONESHOT)
                watch_remove(in, w, dead);
            fired = true;
        }
        if (fired)
            inotify_wake(in);
    }
}

static void self_gone_locked(const char* path, struct dlist* dead)
{
    for (struct dlist_node* n = dlist_first(&instances); n; n = dlist_next(n)) {
        struct inotify* in = dlist_entry(n, struct inotify, link);
        bool fired = false;
        for (struct dlist_node* wn = dlist_first(&in->watches); wn;) {
            struct inotify_watch* w = dlist_entry(wn, struct inotify_watch, link);
            wn = dlist_next(wn);
            if (strcmp(w->path, path) != 0)
                continue;
            if (w->mask & IN_DELETE_SELF)
                queue_event(in, w->wd, IN_DELETE_SELF, 0, nullptr);
            watch_remove(in, w, dead);
            fired = true;
        }
        if (fired)
            inotify_wake(in);
    }
}

static void rewrite_paths_locked(const char* old_path, const char* new_path, struct dlist* dead)
{
    const size_t old_len = strlen(old_path);
    const size_t new_len = strlen(new_path);
    for (struct dlist_node* n = dlist_first(&instances); n; n = dlist_next(n)) {
        struct inotify* in = dlist_entry(n, struct inotify, link);
        bool fired = false;
        for (struct dlist_node* wn = dlist_first(&in->watches); wn;) {
            struct inotify_watch* w = dlist_entry(wn, struct inotify_watch, link);
            wn = dlist_next(wn);
            if (strncmp(w->path, old_path, old_len) != 0)
                continue;
            const char* rest = w->path + old_len;
            if (*rest != '\0' && *rest != '/')
                continue;
            if (*rest == '\0' && (w->mask & IN_MOVE_SELF)) {
                queue_event(in, w->wd, IN_MOVE_SELF, 0, nullptr);
                fired = true;
            }
            const size_t rest_len = strlen(rest);
            if (new_len + rest_len >= VFS_PATH_MAX) {
                watch_remove(in, w, dead);
                fired = true;
                continue;
            }
            memmove(w->path + new_len, rest, rest_len + 1);
            memcpy(w->path, new_path, new_len);
        }
        if (fired)
            inotify_wake(in);
    }
}

void inotify_notify(const char* path, const uint32_t mask, const bool is_dir)
{
    if (!inotify_active())
        return;
    struct dlist dead;
    dlist_init(&dead);
    uint64_t flags;
    spinlock_acquire_irqsave(&inotify_lock, &flags);
    notify_locked(path, mask, 0, is_dir, &dead);
    spinlock_release_irqrestore(&inotify_lock, flags);
    free_dead(&dead);
}

void inotify_deleted(const char* path, const bool is_dir)
{
    if (!inotify_active())
        return;
    struct dlist dead;
    dlist_init(&dead);
    uint64_t flags;
    spinlock_acquire_irqsave(&inotify_lock, &flags);
    notify_locked(path, IN_DELETE, 0, is_dir, &dead);
    self_gone_locked(path, &dead);
    spinlock_release_irqrestore(&inotify_lock, flags);
    free_dead(&dead);
}

void inotify_moved(const char* old_path, const char* new_path, const bool is_dir)
{
    if (!inotify_active())
        return;
    struct dlist dead;
    dlist_init(&dead);
    uint64_t flags;
    spinlock_acquire_irqsave(&inotify_lock, &flags);
    const uint32_t cookie = ++next_cookie;
    self_gone_locked(new_path, &dead);
    notify_locked(old_path, IN_MOVED_FROM, cookie, is_dir, &dead);
    notify_locked(new_path, IN_MOVED_TO, cookie, is_dir, &dead);
    rewrite_paths_locked(old_path, new_path, &dead);
    spinlock_release_irqrestore(&inotify_lock, flags);
    free_dead(&dead);
}

static struct inotify* inotify_of(const struct file_s* f)
{
    return f->node->private_data;
}

static ssize_t inotify_read(struct file_s* f, void* buf, const size_t len)
{
    struct inotify* in = inotify_of(f);
    const size_t cap = len < INOTIFY_QUEUE_BYTES ? len : INOTIFY_QUEUE_BYTES;
    uint8_t* tmp = malloc(cap ? cap : 1);
    if (tmp == nullptr)
        return -ENOMEM;

    uint64_t flags;
    for (;;) {
        spinlock_acquire_irqsave(&inotify_lock, &flags);
        if (in->used)
            break;
        spinlock_release_irqrestore(&inotify_lock, flags);
        if (f->status_flags & IN_NONBLOCK_K) {
            free(tmp);
            return -EAGAIN;
        }
        const int err = process_sleep_timeout(inotify_wait_id(in), INOTIFY_SLEEP_TICKS);
        if (err < 0 && err != -ETIMEDOUT) {
            free(tmp);
            return -ERESTARTSYS;
        }
    }

    size_t n = 0;
    while (n < in->used) {
        const auto ev = (const struct inotify_event_k*)(in->events + n);
        const size_t size = sizeof(*ev) + ev->len;
        if (n + size > cap)
            break;
        n += size;
    }
    if (n == 0) {
        spinlock_release_irqrestore(&inotify_lock, flags);
        free(tmp);
        return -EINVAL;
    }
    memcpy(tmp, in->events, n);
    memmove(in->events, in->events + n, in->used - n);
    in->used -= n;
    in->has_last = in->used != 0 && in->last >= n;
    if (in->has_last)
        in->last -= n;
    spinlock_release_irqrestore(&inotify_lock, flags);

    memcpy(buf, tmp, n);
    free(tmp);
    return (ssize_t)n;
}

static int inotify_poll(struct file_s* f)
{
    return inotify_of(f)->used ? VFS_POLLIN : 0;
}

static int inotify_ioctl(struct file_s* f, const unsigned long request, void* arg)
{
    if (request != FIONREAD_K)
        return -ENOTTY;
    const int n = (int)inotify_of(f)->used;
    return copy_to_user(arg, &n, sizeof(n));
}

static int inotify_close(struct file_s* f)
{
    struct inotify* in = inotify_of(f);
    uint64_t flags;
    spinlock_acquire_irqsave(&inotify_lock, &flags);
    dlist_remove(&instances, &in->link);
    __atomic_sub_fetch(&watch_count, (int)in->watches.count, __ATOMIC_RELAXED);
    spinlock_release_irqrestore(&inotify_lock, flags);
    free_dead(&in->watches);
    free(in->events);
    free(in);
    free(f->node);
    return 0;
}

static file_operations_t inotify_fops = {
    .read = inotify_read,
    .poll = inotify_poll,
    .ioctl = inotify_ioctl,
    .close = inotify_close,
};

long sys_inotify_init1(const int flags)
{
    if (flags & ~(IN_CLOEXEC_K | IN_NONBLOCK_K))
        return -EINVAL;
    struct inotify* in = malloc_safe(sizeof(struct inotify));
    uint8_t* events = malloc(INOTIFY_QUEUE_BYTES);
    if (!in || !events) {
        free(in);
        free(events);
        return -ENOMEM;
    }

    dlist_init(&in->watches);
    in->next_wd = 1;
    in->events = events;

    const long fd = anon_install(&inotify_fops, in, flags & IN_CLOEXEC_K, flags & IN_NONBLOCK_K);
    if (fd < 0) {
        free(events);
        free(in);
        return fd;
    }
    uint64_t irq;
    spinlock_acquire_irqsave(&inotify_lock, &irq);
    dlist_insert_tail(&instances, &in->link);
    spinlock_release_irqrestore(&inotify_lock, irq);
    return fd;
}

static struct inotify* inotify_from_fd(const int fd, long* err)
{
    const struct file_s* f = fd_get(fd);
    if (f == nullptr) {
        *err = -EBADF;
        return nullptr;
    }
    if (f->node->fops != &inotify_fops) {
        *err = -EINVAL;
        return nullptr;
    }
    return inotify_of(f);
}

long sys_inotify_add_watch(const int fd, const char* upath, const uint32_t mask)
{
    long err = 0;
    struct inotify* in = inotify_from_fd(fd, &err);
    if (in == nullptr)
        return err;
    if (!(mask & IN_ALL_EVENTS))
        return -EINVAL;

    char path[VFS_PATH_MAX];
    err = resolve_at(AT_FDCWD, upath, path, false);
    if (err)
        return err;
    size_t type;
    err = vfs_path_type(path, &type);
    if (err)
        return err;
    if ((mask & IN_ONLYDIR) && type != INODE_DIR)
        return -ENOTDIR;

    struct inotify_watch* w = malloc(sizeof(struct inotify_watch));
    if (w == nullptr)
        return -ENOMEM;

    uint64_t flags;
    spinlock_acquire_irqsave(&inotify_lock, &flags);
    for (struct dlist_node* n = dlist_first(&in->watches); n; n = dlist_next(n)) {
        struct inotify_watch* existing = dlist_entry(n, struct inotify_watch, link);
        if (strcmp(existing->path, path) == 0) {
            existing->mask = mask & IN_MASK_ADD ? existing->mask | mask : mask;
            const int wd = existing->wd;
            spinlock_release_irqrestore(&inotify_lock, flags);
            free(w);
            return wd;
        }
    }
    w->wd = in->next_wd++;
    w->mask = mask;
    memcpy(w->path, path, sizeof(path));
    dlist_insert_tail(&in->watches, &w->link);
    __atomic_add_fetch(&watch_count, 1, __ATOMIC_RELAXED);
    const int wd = w->wd;
    spinlock_release_irqrestore(&inotify_lock, flags);
    return wd;
}

long sys_inotify_rm_watch(const int fd, const int wd)
{
    long err = 0;
    struct inotify* in = inotify_from_fd(fd, &err);
    if (in == nullptr)
        return err;

    struct dlist dead;
    dlist_init(&dead);
    err = -EINVAL;
    uint64_t flags;
    spinlock_acquire_irqsave(&inotify_lock, &flags);
    for (struct dlist_node* n = dlist_first(&in->watches); n; n = dlist_next(n)) {
        struct inotify_watch* w = dlist_entry(n, struct inotify_watch, link);
        if (w->wd == wd) {
            watch_remove(in, w, &dead);
            inotify_wake(in);
            err = 0;
            break;
        }
    }
    spinlock_release_irqrestore(&inotify_lock, flags);
    free_dead(&dead);
    return err;
}
