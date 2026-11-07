#include "poll_head.h"

void poll_head_init(struct poll_head* ph)
{
    spinlock_init(&ph->lock, "poll_head");
    dlist_init(&ph->waiters);
}

void poll_head_add(struct poll_head* ph, struct poll_waiter* w)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&ph->lock, &flags);
    if (w->exclusive)
        dlist_insert_tail(&ph->waiters, &w->link);
    else
        dlist_insert_head(&ph->waiters, &w->link);
    spinlock_release_irqrestore(&ph->lock, flags);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void poll_head_remove(struct poll_head* ph, struct poll_waiter* w)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&ph->lock, &flags);
    dlist_remove(&ph->waiters, &w->link);
    spinlock_release_irqrestore(&ph->lock, flags);
}

void poll_head_wake(struct poll_head* ph, uint32_t events)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&ph->waiters.count, __ATOMIC_RELAXED))
        return;
    uint64_t flags;
    spinlock_acquire_irqsave(&ph->lock, &flags);
    struct poll_waiter* first = nullptr;
    for (struct dlist_node* n = dlist_first(&ph->waiters); n; n = dlist_next(n)) {
        struct poll_waiter* w = dlist_entry(n, struct poll_waiter, link);
        if (!(w->events & events) || !w->func(w, events))
            continue;
        if (w->exclusive)
            break;
        if (!first)
            first = w;
    }
    if (first && ph->waiters.count > 1) {
        dlist_remove(&ph->waiters, &first->link);
        struct dlist_node* n = dlist_first(&ph->waiters);
        while (n && !dlist_entry(n, struct poll_waiter, link)->exclusive)
            n = dlist_next(n);
        if (n)
            dlist_insert_before(&ph->waiters, n, &first->link);
        else
            dlist_insert_tail(&ph->waiters, &first->link);
    }
    spinlock_release_irqrestore(&ph->lock, flags);
}
