#include <drivers/net/core/netisr.h>

#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/netqueue.h>
#include <hardware/interrupts/irq.h>
#include <hardware/smp/smp.h>
#include <os/errno.h>

#define NETISR_POLL_BUDGET 64

struct netisr_proto_state {
    netisr_handler_t handler;
    struct netqueue queue;
    bool mpsafe;
};

static struct netisr_proto_state protos[NETISR_MAX];

void netisr_initialize()
{
    for (int i = 0; i < NETISR_MAX; i++) {
        protos[i].handler = nullptr;
        netqueue_init(&protos[i].queue, 0);
    }

    irq_register_softintr(netisr_poll);
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

int netisr_register_mpsafe(const enum netisr_proto proto, const netisr_handler_t handler, const uint32_t queue_len)
{
    const int err = netisr_register(proto, handler, queue_len);
    if (!err)
        protos[proto].mpsafe = true;
    return err;
}

void netisr_unregister(const enum netisr_proto proto)
{
    if (proto <= 0 || proto >= NETISR_MAX)
        return;
    protos[proto].handler = nullptr;
    struct netbuf* nb;
    while ((nb = netqueue_dequeue(&protos[proto].queue))) {
        struct netif* rcvif = nb->rcvif;
        netbuf_free_chain(nb);
        if (rcvif)
            netif_rele(rcvif);
    }
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

    struct netif* rcvif = nb->rcvif;
    if (rcvif)
        netif_ref(rcvif);
    if (!netqueue_enqueue(&protos[proto].queue, nb)) {
        netbuf_free_chain(nb);
        if (rcvif)
            netif_rele(rcvif);
        return -ENOBUFS;
    }
    return 0;
}

bool netisr_pending()
{
    for (int p = 1; p < NETISR_MAX; p++)
        if (netqueue_len(&protos[p].queue))
            return true;
    return false;
}

// One drainer at a time keeps segments in order. Queues that are not mpsafe need the net lock,
// taken only by try so the drainer never waits on a holder that is waiting on it.
static bool netisr_drain()
{
    static bool running;
    bool did = false;
    while (netisr_pending() && !__atomic_exchange_n(&running, true, __ATOMIC_ACQUIRE)) {
        bool more = true, blocked = false;
        netif_tx_batch_begin();
        while (more) {
            more = false;
            for (int p = 1; p < NETISR_MAX; p++) {
                if (!netqueue_len(&protos[p].queue))
                    continue;
                bool locked = false;
                if (!protos[p].mpsafe && !net_lock_held() && !(locked = net_lock_try_enter())) {
                    blocked = true;
                    continue;
                }
                for (int i = 0; i < NETISR_POLL_BUDGET; i++) {
                    struct netbuf* nb = netqueue_dequeue(&protos[p].queue);
                    if (!nb)
                        break;
                    struct netif* rcvif = nb->rcvif;
                    const netisr_handler_t handler = protos[p].handler;
                    if (handler)
                        handler(nb);
                    else
                        netbuf_free_chain(nb);
                    if (rcvif)
                        netif_rele(rcvif);
                    more = true;
                    did = true;
                }
                if (locked)
                    net_lock_exit();
            }
        }
        netif_tx_batch_end();
        __atomic_store_n(&running, false, __ATOMIC_RELEASE);
        if (blocked)
            break;
    }
    return did;
}

void netisr_poll()
{
    netisr_drain();
}

void netisr_purge(struct netif* nif)
{
    for (int p = 1; p < NETISR_MAX; p++) {
        for (size_t n = netqueue_purge(&protos[p].queue, nif); n; n--)
            netif_rele(nif);
    }
}

bool netisr_poll_pending()
{
    return netisr_drain();
}
