#include <drivers/net/core/netif.h>

#include <drivers/net/core/netbuf.h>
#include <drivers/net/arp/arp.h>
#include <drivers/net/core/netisr.h>
#include <drivers/net/core/route.h>
#include <drivers/net/dhcp/dhcp.h>
#include <drivers/net/dhcp6/dhcp6.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/ipv6.h>
#include <hardware/smp/smp.h>
#include <os/errno.h>

#include "hardware/memory/alloc.h"
#include "utils/str/str.h"

#define NETIF_SEND_QUEUE_LEN 256
#define NETIF_MIN_MTU        68

static struct dlist netif_list;
static spinlock_t netif_list_lock;
static uint32_t netif_next_index = 1;

void netif_initialize()
{
    dlist_init(&netif_list);
    spinlock_init(&netif_list_lock, "netif_list");
    netif_next_index = 1;
}

struct netif* netif_alloc(const uint32_t type, const char* name_prefix)
{
    struct netif* nif = malloc(sizeof(struct netif));
    if (nif == nullptr) {
        return nullptr;
    }

    memset(nif, 0, sizeof(struct netif));
    nif->type = type;
    nif->link_state = NETIF_LINK_UNKNOWN;
    nif->mtu = 1500;
    nif->refcount = 1;

    strncpy(nif->name, name_prefix ? name_prefix : "if", NETIF_NAME_LEN - 4);

    dlist_init(&nif->ipv4_addrs);
    dlist_init(&nif->ipv6_addrs);
    netqueue_init(&nif->send_queue, NETIF_SEND_QUEUE_LEN);
    spinlock_init(&nif->lock, nif->name);

    return nif;
}

void netif_free(struct netif* nif)
{
    if (!nif)
        return;
    netif_detach(nif);
    netif_rele(nif);
}

void netif_ref(struct netif* nif)
{
    __atomic_add_fetch(&nif->refcount, 1, __ATOMIC_SEQ_CST);
}

void netif_rele(struct netif* nif)
{
    if (__atomic_sub_fetch(&nif->refcount, 1, __ATOMIC_SEQ_CST))
        return;
    netqueue_drain(&nif->send_queue);
    free(nif);
}

static uint32_t netif_free_unit(const char* prefix)
{
    const size_t plen = strlen(prefix);
    for (uint32_t unit = 0;; unit++) {
        bool used = false;
        for (struct dlist_node* node = dlist_first(&netif_list); node && !used; node = dlist_next(node)) {
            const struct netif* nif = dlist_entry(node, struct netif, link);
            if (strncmp(nif->name, prefix, plen) != 0)
                continue;
            uint32_t n = 0;
            const char* p = nif->name + plen;
            if (*p < '0' || *p > '9')
                continue;
            while (*p >= '0' && *p <= '9')
                n = n * 10 + (*p++ - '0');
            used = *p == '\0' && n == unit;
        }
        if (!used)
            return unit;
    }
}

int netif_attach(struct netif* nif)
{
    if (nif->index)
        return -EEXIST;

    uint64_t flags;
    spinlock_acquire_irqsave(&netif_list_lock, &flags);

    uint32_t unit = netif_free_unit(nif->name);
    char digits[4];
    int nd = 0;
    do {
        digits[nd++] = '0' + unit % 10;
        unit /= 10;
    } while (unit && nd < 3);
    size_t len = strlen(nif->name);
    while (nd)
        nif->name[len++] = digits[--nd];
    nif->name[len] = '\0';

    nif->index = netif_next_index++;
    dlist_insert_tail(&netif_list, &nif->link);

    spinlock_release_irqrestore(&netif_list_lock, flags);
    return 0;
}

void netif_detach(struct netif* nif)
{
    if (!nif->index)
        return;
    dhcp_stop(nif);
    dhcp6_stop(nif);
    netif_down(nif);

    uint64_t flags;
    spinlock_acquire_irqsave(&netif_list_lock, &flags);
    dlist_remove(&netif_list, &nif->link);
    nif->index = 0;
    __atomic_store_n(&nif->gone, true, __ATOMIC_SEQ_CST);
    spinlock_release_irqrestore(&netif_list_lock, flags);

    netisr_purge(nif);
    while (__atomic_load_n(&nif->refcount, __ATOMIC_SEQ_CST) > 1) {
        smp_spin_poll();
        asm volatile("pause");
    }

    ipv4_ifdetach(nif);
    ipv6_ifdetach(nif);
    route_flush_netif(nif);
}

void netif_input(struct netif* nif, struct netbuf* nb)
{
    if (!(nif->flags & NETIF_UP) || !nif->input) {
        nif->stats.rx_dropped++;
        netbuf_free_chain(nb);
        return;
    }
    nb->rcvif = nif;
    nif->input(nif, nb);
}

int netif_output(struct netif* nif, struct netbuf* nb)
{
    if (!(nif->flags & NETIF_RUNNING) || !nif->ops || !nif->ops->transmit) {
        nif->stats.tx_dropped++;
        netbuf_free_chain(nb);
        return -ENETDOWN;
    }

    return nif->ops->transmit(nif, nb);
}

struct netif* netif_find_by_name(const char* name)
{
    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        if (!strcmp(nif->name, name))
            return nif;
    }
    return nullptr;
}

struct netif* netif_find_by_index(const uint32_t index)
{
    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        if (nif->index == index)
            return nif;
    }
    return nullptr;
}

static struct netif* netif_hold(struct dlist_node* node)
{
    if (!node)
        return nullptr;
    struct netif* nif = dlist_entry(node, struct netif, link);
    netif_ref(nif);
    return nif;
}

struct netif* netif_first()
{
    uint64_t flags;
    spinlock_acquire_irqsave(&netif_list_lock, &flags);
    struct netif* first = netif_hold(dlist_first(&netif_list));
    spinlock_release_irqrestore(&netif_list_lock, flags);
    return first;
}

struct netif* netif_next(struct netif* nif)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&netif_list_lock, &flags);

    struct netif* next = netif_hold(nif->gone ? dlist_first(&netif_list) : dlist_next(&nif->link));
    spinlock_release_irqrestore(&netif_list_lock, flags);
    netif_rele(nif);
    return next;
}

int netif_up(struct netif* nif)
{
    if (nif->gone)
        return -ENXIO;
    if (nif->flags & NETIF_UP)
        return 0;
    if (nif->ops && nif->ops->init) {
        const int rc = nif->ops->init(nif);
        if (rc < 0)
            return rc;
    }
    nif->flags |= NETIF_UP | NETIF_RUNNING;
    return 0;
}

int netif_down(struct netif* nif)
{
    if (!(nif->flags & NETIF_UP))
        return 0;
    nif->flags &= ~(NETIF_UP | NETIF_RUNNING);
    if (nif->ops && nif->ops->qflush)
        nif->ops->qflush(nif);
    else
        netqueue_drain(&nif->send_queue);
    return 0;
}

int netif_set_mtu(struct netif* nif, const uint32_t mtu)
{
    if (mtu < NETIF_MIN_MTU)
        return -EINVAL;

    uint32_t arg = mtu;
    const int rc = netif_ioctl(nif, NETIF_IOCTL_SET_MTU, &arg);
    if (rc < 0 && rc != -EOPNOTSUPP)
        return rc;
    nif->mtu = mtu;
    return 0;
}

int netif_set_hw_addr(struct netif* nif, const uint8_t* hwaddr, const uint8_t len)
{
    if (len > NETIF_HWADDR_LEN)
        return -EINVAL;

    memcpy(nif->hw_addr, hwaddr, len);
    nif->hw_addr_len = len;
    return 0;
}

int netif_ioctl(struct netif* nif, const uint32_t cmd, void* arg)
{
    if (__atomic_load_n(&nif->gone, __ATOMIC_SEQ_CST))
        return -ENXIO;
    if (!nif->ops || !nif->ops->ioctl)
        return -EOPNOTSUPP;
    return nif->ops->ioctl(nif, cmd, arg);
}

void netif_link_state_change(struct netif* nif, const uint32_t link_state)
{
    if (nif->link_state == link_state)
        return;
    nif->link_state = link_state;

    // TODO: nd6_dad_start for IPv6, flush the neighbour cache on link down
    if (link_state == NETIF_LINK_UP && !(nif->flags & NETIF_LOOPBACK))
        dhcp_start(nif);
    else if (link_state == NETIF_LINK_DOWN)
        arp_cache_flush(nif);
}

void netif_poll_all()
{
    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif))
        if ((nif->flags & NETIF_UP) && nif->ops && nif->ops->poll)
            nif->ops->poll(nif);
}
