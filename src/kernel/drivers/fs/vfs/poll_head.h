#ifndef DRIVERS_FS_VFS_POLL_HEAD_H
#define DRIVERS_FS_VFS_POLL_HEAD_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <utils/queue/queue.h>
#include <utils/sync/spinlock.h>

struct poll_waiter;
typedef bool (*poll_wake_fn)(struct poll_waiter* w, uint32_t events);

struct poll_waiter {
    struct dlist_node link;
    uint32_t events;
    bool exclusive;
    poll_wake_fn func;
};

struct poll_head {
    spinlock_t lock;
    struct dlist waiters;
};

void poll_head_init(struct poll_head* ph);
void poll_head_add(struct poll_head* ph, struct poll_waiter* w);
void poll_head_remove(struct poll_head* ph, struct poll_waiter* w);
void poll_head_wake(struct poll_head* ph, uint32_t events);

struct file_s;
void epoll_file_release(struct file_s* f);

#endif
