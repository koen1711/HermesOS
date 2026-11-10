#include <drivers/net/core/netisr.h>

#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/netqueue.h>
#include <hardware/interrupts/irq.h>
#include <hardware/smp/smp.h>
#include <drivers/scheduler/process.h>
#include <hardware/timer/tsc.h>
#include <os/errno.h>

#define NETISR_POLL_BUDGET 64
#define NETISR_SOFTINTR_BUDGET 256
#define NETISR_FLOWS 8
#define NETISR_RX_BUDGET 128
#define NETISR_IDLE_POLL_US 30
#define NETISR_IDLE_POLL_MAX_US 100
#define NETISR_MAX_POLLERS 4
#define NETISR_FLOW_MASK ((1u << NETISR_FLOWS) - 1)

static_assert(NETISR_MAX * NETISR_FLOWS <= 64);

struct netisr_queue {
    struct netqueue q;
    bool running;
} __attribute__((aligned(64)));

struct netisr_proto_state {
    netisr_handler_t handler;
    struct netisr_queue queues[NETISR_FLOWS];
    uint32_t nqueues;
    bool mpsafe;
};

static struct netisr_proto_state protos[NETISR_MAX];
static netisr_poller_t pollers[NETISR_MAX_POLLERS];
static bool rx_wanted;
static uint64_t netisr_mask __attribute__((aligned(64)));

static void netisr_queue_init(const int proto, const uint32_t i, const uint32_t queue_len)
{
    netqueue_init(&protos[proto].queues[i].q, queue_len);
    netqueue_set_mask(&protos[proto].queues[i].q, &netisr_mask, 1ull << (proto * NETISR_FLOWS + i));
}

void netisr_initialize()
{
    for (int p = 0; p < NETISR_MAX; p++) {
        protos[p].handler = nullptr;
        protos[p].nqueues = 1;
        for (uint32_t i = 0; i < NETISR_FLOWS; i++)
            netisr_queue_init(p, i, 0);
    }

    irq_register_softintr(netisr_softintr);
}

static int netisr_register_queues(const enum netisr_proto proto, const netisr_handler_t handler,
                                  const uint32_t queue_len, const bool mpsafe)
{
    if (proto <= 0 || proto >= NETISR_MAX || !handler)
        return -EINVAL;
    if (protos[proto].handler)
        return -EEXIST;
    protos[proto].nqueues = mpsafe ? NETISR_FLOWS : 1;
    for (uint32_t i = 0; i < protos[proto].nqueues; i++)
        netisr_queue_init(proto, i, queue_len);
    protos[proto].mpsafe = mpsafe;
    __atomic_store_n(&protos[proto].handler, handler, __ATOMIC_RELEASE);
    return 0;
}

int netisr_register(const enum netisr_proto proto, const netisr_handler_t handler, const uint32_t queue_len)
{
    return netisr_register_queues(proto, handler, queue_len, false);
}

int netisr_register_mpsafe(const enum netisr_proto proto, const netisr_handler_t handler, const uint32_t queue_len)
{
    return netisr_register_queues(proto, handler, queue_len, true);
}

static void netisr_free_queued(struct netqueue* q)
{
    struct netbuf* nb;
    while ((nb = netqueue_dequeue(q))) {
        struct netif* rcvif = nb->rcvif;
        netbuf_free_chain(nb);
        if (rcvif)
            netif_rele(rcvif);
    }
}

void netisr_unregister(const enum netisr_proto proto)
{
    if (proto <= 0 || proto >= NETISR_MAX)
        return;
    protos[proto].handler = nullptr;
    for (uint32_t i = 0; i < protos[proto].nqueues; i++)
        netisr_free_queued(&protos[proto].queues[i].q);
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

int netisr_queue_flow(const enum netisr_proto proto, struct netbuf* nb, const uint32_t hash)
{
    if (proto <= 0 || proto >= NETISR_MAX || !protos[proto].handler) {
        netbuf_free_chain(nb);
        return -EPROTONOSUPPORT;
    }

    struct netif* rcvif = nb->rcvif;
    if (rcvif)
        netif_ref(rcvif);
    struct netqueue* q = &protos[proto].queues[hash % protos[proto].nqueues].q;
    if (!netqueue_enqueue(q, nb)) {
        netbuf_free_chain(nb);
        if (rcvif)
            netif_rele(rcvif);
        return -ENOBUFS;
    }
    return 0;
}

int netisr_queue(const enum netisr_proto proto, struct netbuf* nb)
{
    return netisr_queue_flow(proto, nb, 0);
}

bool netisr_pending()
{
    return __atomic_load_n(&netisr_mask, __ATOMIC_ACQUIRE) != 0;
}

static unsigned netisr_drain_queue(struct netisr_proto_state* ps, struct netisr_queue* nq, const unsigned budget,
                                   bool* blocked)
{
    unsigned did = 0;
    while (did < budget && netqueue_len(&nq->q) && !__atomic_exchange_n(&nq->running, true, __ATOMIC_ACQUIRE)) {
        bool locked = false;
        if (!ps->mpsafe && !net_lock_held() && !(locked = net_lock_try_enter())) {
            __atomic_store_n(&nq->running, false, __ATOMIC_RELEASE);
            *blocked = true;
            break;
        }
        while (did < budget) {
            size_t n;
            struct netbuf* nb = netqueue_dequeue_batch(&nq->q, budget - did, &n);
            if (!nb)
                break;
            did += n;
            const netisr_handler_t handler = __atomic_load_n(&ps->handler, __ATOMIC_ACQUIRE);
            while (nb) {
                struct netbuf* next = nb->next_pkt;
                nb->next_pkt = nullptr;
                struct netif* rcvif = nb->rcvif;
                if (handler)
                    handler(nb);
                else
                    netbuf_free_chain(nb);
                if (rcvif)
                    netif_rele(rcvif);
                nb = next;
            }
        }
        if (locked)
            net_lock_exit();
        __atomic_store_n(&nq->running, false, __ATOMIC_RELEASE);
    }
    return did;
}

static unsigned netisr_drain(const unsigned budget)
{
    uint64_t pending = __atomic_load_n(&netisr_mask, __ATOMIC_ACQUIRE);
    if (!pending)
        return 0;
    unsigned did = 0;
    const uint32_t start = (uint32_t)cpu_this()->index;
    netif_tx_batch_begin();
    while (pending && did < budget) {
        bool progress = false;
        for (int p = 1; p < NETISR_MAX && did < budget; p++) {
            uint32_t bits = (uint32_t)(pending >> (p * NETISR_FLOWS)) & NETISR_FLOW_MASK;
            if (!bits)
                continue;
            struct netisr_proto_state* ps = &protos[p];
            const uint32_t nq_count = ps->nqueues;
            const uint32_t rot = start % nq_count;
            bits = ((bits >> rot) | (bits << (nq_count - rot))) & ((1u << nq_count) - 1);
            bool blocked = false;
            while (bits && did < budget && !blocked) {
                const uint32_t k = (uint32_t)__builtin_ctz(bits);
                bits &= bits - 1;
                struct netisr_queue* nq = &ps->queues[(k + rot) % nq_count];
                const unsigned left = budget - did;
                const unsigned n = netisr_drain_queue(ps, nq, left < NETISR_POLL_BUDGET ? left : NETISR_POLL_BUDGET,
                                                      &blocked);
                did += n;
                progress |= n != 0;
            }
        }
        if (!progress)
            break;
        pending = __atomic_load_n(&netisr_mask, __ATOMIC_ACQUIRE);
    }
    netif_tx_batch_end();
    return did;
}

void netisr_poll()
{
    netisr_drain(~0u);
}

int netisr_register_poller(const netisr_poller_t poller)
{
    if (!poller)
        return -EINVAL;
    for (int i = 0; i < NETISR_MAX_POLLERS; i++) {
        netisr_poller_t expected = nullptr;
        if (__atomic_compare_exchange_n(&pollers[i], &expected, poller, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return 0;
    }
    return -ENOSPC;
}

void netisr_unregister_poller(const netisr_poller_t poller)
{
    for (int i = 0; i < NETISR_MAX_POLLERS; i++) {
        netisr_poller_t expected = poller;
        __atomic_compare_exchange_n(&pollers[i], &expected, nullptr, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
}

void netisr_rx_schedule()
{
    __atomic_store_n(&rx_wanted, true, __ATOMIC_RELEASE);
}

void netisr_softintr()
{
    const process_t* current = scheduler_get()->current;
    if (current && current->pid != IDLE_PROC_PID
        && (__atomic_load_n(&rx_wanted, __ATOMIC_ACQUIRE) || netisr_pending()) && process_kick_idle_other())
        return;

    __atomic_store_n(&rx_wanted, false, __ATOMIC_RELAXED);
    const bool idle = !current || current->pid == IDLE_PROC_PID;
    unsigned polled = 0;
    unsigned drained = 0;
    uint64_t deadline = 0;
    const uint64_t poll_ticks = tsc_ticks_from_micros(NETISR_IDLE_POLL_US);
    const uint64_t limit = idle ? tsc_read() + tsc_ticks_from_micros(NETISR_IDLE_POLL_MAX_US) : 0;
    bool arm = !idle;
    bool more;
    for (;;) {
        unsigned got = 0;
        for (int i = 0; i < NETISR_MAX_POLLERS; i++) {
            const netisr_poller_t poller = __atomic_load_n(&pollers[i], __ATOMIC_ACQUIRE);
            if (poller && polled + got < NETISR_RX_BUDGET)
                got += poller(NETISR_RX_BUDGET - polled - got, arm);
        }
        polled += got;
        drained += netisr_drain(NETISR_SOFTINTR_BUDGET - drained);
        if (polled >= NETISR_RX_BUDGET || drained >= NETISR_SOFTINTR_BUDGET) {
            more = polled >= NETISR_RX_BUDGET || netisr_pending();
            break;
        }
        if (arm) {
            more = false;
            break;
        }
        const uint64_t now = tsc_read();
        if (got)
            deadline = now + poll_ticks < limit ? now + poll_ticks : limit;
        if (now >= deadline || process_ready_pending())
            arm = true;
        else
            asm volatile("pause");
    }
    if (more) {
        __atomic_store_n(&rx_wanted, true, __ATOMIC_RELEASE);
        process_kick_idle_other();
    }
}

void netisr_purge(struct netif* nif)
{
    for (int p = 1; p < NETISR_MAX; p++)
        for (uint32_t i = 0; i < protos[p].nqueues; i++)
            for (size_t n = netqueue_purge(&protos[p].queues[i].q, nif); n; n--)
                netif_rele(nif);
}

bool netisr_poll_pending()
{
    return netisr_drain(~0u) != 0;
}
