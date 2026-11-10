#include <drivers/net/core/netqueue.h>

#include "netbuf.h"

void netqueue_init(struct netqueue* q, const size_t max_len)
{
    q->head = q->tail = nullptr;
    q->len = 0;
    q->drops = 0;
    spinlock_init(&q->lock, "netqueue");
    q->max_len = max_len;
    q->mask = nullptr;
    q->bit = 0;
}

void netqueue_set_mask(struct netqueue* q, uint64_t* mask, const uint64_t bit)
{
    q->mask = mask;
    q->bit = bit;
}

static void netqueue_mark(const struct netqueue* q)
{
    if (q->mask)
        __atomic_fetch_or(q->mask, q->bit, __ATOMIC_RELEASE);
}

static void netqueue_unmark(const struct netqueue* q)
{
    if (q->mask)
        __atomic_fetch_and(q->mask, ~q->bit, __ATOMIC_RELAXED);
}

bool netqueue_enqueue(struct netqueue* q, struct netbuf* nb)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&q->lock, &flags);
    if (q->len >= q->max_len) {
        q->drops++;
        spinlock_release_irqrestore(&q->lock, flags);
        return false;
    }
    nb->next_pkt = nullptr;
    if (q->tail) {
        q->tail->next_pkt = nb;
    } else {
        q->head = nb;
    }
    q->tail = nb;
    if (q->len++ == 0)
        netqueue_mark(q);
    spinlock_release_irqrestore(&q->lock, flags);
    return true;
}

struct netbuf* netqueue_dequeue(struct netqueue* q)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&q->lock, &flags);
    struct netbuf* nb = q->head;
    if (nb) {
        q->head = nb->next_pkt;
        if (!q->head) {
            q->tail = nullptr;
        }
        nb->next_pkt = nullptr;
        if (--q->len == 0)
            netqueue_unmark(q);
    }
    spinlock_release_irqrestore(&q->lock, flags);
    return nb;
}

struct netbuf* netqueue_dequeue_batch(struct netqueue* q, const size_t max, size_t* n)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&q->lock, &flags);
    struct netbuf* head = q->head;
    struct netbuf* last = nullptr;
    size_t got = 0;
    for (struct netbuf* nb = head; nb && got < max; nb = nb->next_pkt) {
        last = nb;
        got++;
    }
    if (last) {
        q->head = last->next_pkt;
        if (!q->head)
            q->tail = nullptr;
        last->next_pkt = nullptr;
        q->len -= got;
        if (!q->len)
            netqueue_unmark(q);
    }
    spinlock_release_irqrestore(&q->lock, flags);
    *n = got;
    return got ? head : nullptr;
}

void netqueue_drain(struct netqueue* q)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&q->lock, &flags);
    struct netbuf* nb = q->head;
    q->head = q->tail = nullptr;
    q->len = 0;
    netqueue_unmark(q);
    spinlock_release_irqrestore(&q->lock, flags);

    // free outside the lock
    while (nb) {
        struct netbuf* next = nb->next_pkt;
        netbuf_free_chain(nb);
        nb = next;
    }
}

size_t netqueue_purge(struct netqueue* q, const struct netif* rcvif)
{
    struct netbuf* dropped = nullptr;
    size_t count = 0;
    uint64_t flags;
    spinlock_acquire_irqsave(&q->lock, &flags);
    struct netbuf** pp = &q->head;
    q->tail = nullptr;
    while (*pp) {
        struct netbuf* nb = *pp;
        if (nb->rcvif == rcvif) {
            *pp = nb->next_pkt;
            nb->next_pkt = dropped;
            dropped = nb;
            q->len--;
            count++;
        } else {
            q->tail = nb;
            pp = &nb->next_pkt;
        }
    }
    if (!q->len)
        netqueue_unmark(q);
    spinlock_release_irqrestore(&q->lock, flags);

    while (dropped) {
        struct netbuf* next = dropped->next_pkt;
        netbuf_free_chain(dropped);
        dropped = next;
    }
    return count;
}
