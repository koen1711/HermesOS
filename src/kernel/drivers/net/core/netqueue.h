#ifndef NET_CORE_NETQUEUE_H
#define NET_CORE_NETQUEUE_H

#include <os/stddef.h>
#include <os/stdbool.h>
#include <utils/sync/spinlock.h>

struct netbuf;

struct netqueue {
    struct netbuf* head;
    struct netbuf* tail;
    size_t len;
    size_t max_len;
    size_t drops;
    spinlock_t lock;
};

void netqueue_init(struct netqueue* q, size_t max_len);

bool netqueue_enqueue(struct netqueue* q, struct netbuf* nb);
struct netbuf* netqueue_dequeue(struct netqueue* q);

void netqueue_drain(struct netqueue* q);
size_t netqueue_len(const struct netqueue* q);
bool netqueue_full(const struct netqueue* q);
bool netqueue_empty(const struct netqueue* q);

#endif //NET_CORE_NETQUEUE_H
