#ifndef NET_CORE_NETQUEUE_H
#define NET_CORE_NETQUEUE_H

#include <os/stddef.h>
#include <os/stdbool.h>
#include <utils/sync/spinlock.h>

struct netbuf;
struct netif;

struct netqueue {
    struct netbuf* head;
    struct netbuf* tail;
    size_t len;
    size_t max_len;
    size_t drops;
    uint64_t* mask;
    uint64_t bit;
    spinlock_t lock;
};

void netqueue_init(struct netqueue* q, size_t max_len);
void netqueue_set_mask(struct netqueue* q, uint64_t* mask, uint64_t bit);

bool netqueue_enqueue(struct netqueue* q, struct netbuf* nb);
struct netbuf* netqueue_dequeue(struct netqueue* q);
struct netbuf* netqueue_dequeue_batch(struct netqueue* q, size_t max, size_t* n);

void netqueue_drain(struct netqueue* q);

size_t netqueue_purge(struct netqueue* q, const struct netif* rcvif);
static inline size_t netqueue_len(const struct netqueue* q)
{
    return __atomic_load_n(&q->len, __ATOMIC_RELAXED);
}

static inline bool netqueue_full(const struct netqueue* q)
{
    return netqueue_len(q) >= q->max_len;
}

static inline bool netqueue_empty(const struct netqueue* q)
{
    return netqueue_len(q) == 0;
}

#endif //NET_CORE_NETQUEUE_H
