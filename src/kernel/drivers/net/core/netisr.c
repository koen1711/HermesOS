#include <drivers/net/core/netisr.h>

#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netqueue.h>
#include <os/errno.h>

#define NETISR_POLL_BUDGET 64

struct netisr_proto_state {
    netisr_handler_t handler;
    struct netqueue queue;
};

static struct netisr_proto_state protos[NETISR_MAX];

void netisr_initialize()
{
    for (int i = 0; i < NETISR_MAX; i++) {
        protos[i].handler = nullptr;
        netqueue_init(&protos[i].queue, 0);
    }
}

int netisr_register(const enum netisr_proto proto, const netisr_handler_t handler, const uint32_t queue_len)
{
    if (proto <= 0 || proto >= NETISR_MAX || !handler)
        return -EINVAL;
    if (protos[proto].handler)
        return -EEXIST;
    netqueue_init(&protos[proto].queue, queue_len);
    protos[proto].handler = handler;
    return 0;
}

void netisr_unregister(const enum netisr_proto proto)
{
    if (proto <= 0 || proto >= NETISR_MAX)
        return;
    protos[proto].handler = nullptr;
    netqueue_drain(&protos[proto].queue);
}

int netisr_dispatch(const enum netisr_proto proto, struct netbuf* nb)
{
    if (proto <= 0 || proto >= NETISR_MAX || !protos[proto].handler) {
        netbuf_free_chain(nb);
        return -EPROTONOSUPPORT;
    }
    protos[proto].handler(nb);
    return 0;
}

int netisr_queue(const enum netisr_proto proto, struct netbuf* nb)
{
    if (proto <= 0 || proto >= NETISR_MAX || !protos[proto].handler) {
        netbuf_free_chain(nb);
        return -EPROTONOSUPPORT;
    }
    if (!netqueue_enqueue(&protos[proto].queue, nb)) {
        netbuf_free_chain(nb);
        return -ENOBUFS;
    }
    return 0;
}

void netisr_poll()
{
    bool more = true;
    while (more) {
        more = false;
        for (int p = 1; p < NETISR_MAX; p++) {
            for (int i = 0; i < NETISR_POLL_BUDGET; i++) {
                struct netbuf* nb = netqueue_dequeue(&protos[p].queue);
                if (!nb)
                    break;
                const netisr_handler_t handler = protos[p].handler;
                if (handler)
                    handler(nb);
                else
                    netbuf_free_chain(nb);
                more = true;
            }
        }
    }
}
