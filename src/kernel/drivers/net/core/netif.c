#include <drivers/net/core/netif.h>

#include <drivers/net/core/netbuf.h>
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
    nif->mtu = 1500; // Default MTU

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
    if (nif->index)
        netif_detach(nif);
    netqueue_drain(&nif->send_queue);
    free(nif);
}

static uint32_t netif_free_unit(const char* prefix)
{
    const size_t plen = strlen(prefix);
    for (uint32_t unit = 0;; unit++) {
        bool used = false;
        for (struct netif* nif = netif_first(); nif && !used; nif = netif_next(nif)) {
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
    netif_down(nif);

    uint64_t flags;
    spinlock_acquire_irqsave(&netif_list_lock, &flags);
    dlist_remove(&netif_list, &nif->link);
    nif->index = 0;
    spinlock_release_irqrestore(&netif_list_lock, flags);
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

struct netif* netif_first()
{
    struct dlist_node* node = dlist_first(&netif_list);
    return node ? dlist_entry(node, struct netif, link) : nullptr;
}

struct netif* netif_next(const struct netif* nif)
{
    struct dlist_node* node = dlist_next(&nif->link);
    return node ? dlist_entry(node, struct netif, link) : nullptr;
}

int netif_up(struct netif* nif)
{
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

int netif_set_hwaddr(struct netif* nif, const uint8_t* hwaddr, const uint8_t len)
{
    if (len > NETIF_HWADDR_LEN)
        return -EINVAL;

    memcpy(nif->hwaddr, hwaddr, len);
    nif->hwaddr_len = len;
    return 0;
}

int netif_ioctl(struct netif* nif, const uint32_t cmd, void* arg)
{
    if (!nif->ops || !nif->ops->ioctl)
        return -EOPNOTSUPP;
    return nif->ops->ioctl(nif, cmd, arg);
}

void netif_link_state_change(struct netif* nif, const uint32_t link_state)
{
    if (nif->link_state == link_state)
        return;
    nif->link_state = link_state;

    // TODO: on NETIF_LINK_UP start dhcp_start / nd6_dad_start once those exist,
    // on NETIF_LINK_DOWN flush the ARP and neighbour caches
}
