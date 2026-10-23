#include <drivers/net/ipv4/ipv4.h>

#include <drivers/net/core/netif.h>
#include <drivers/net/core/byteorder.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

void ipv4_initialize()
{
    // TODO: netisr_register(NETISR_IP, ipv4_input, ...) once ipv4_input exists
}

static struct in_addr ipv4_prefix_to_mask(const uint8_t prefix_len)
{
    const uint32_t mask = prefix_len ? 0xFFFFFFFFu << (32 - prefix_len) : 0;
    return (struct in_addr){ htonl(mask) };
}

int ipv4_addr_add(struct netif* nif, const struct in_addr addr, const uint8_t prefix_len, const uint64_t valid_until)
{
    if (prefix_len > 32)
        return -EINVAL;
    if (ipv4_addr_find(addr))
        return -EEXIST;

    struct ipv4_ifaddr* ia = malloc(sizeof(struct ipv4_ifaddr));
    if (!ia)
        return -ENOMEM;
    memset(ia, 0, sizeof(struct ipv4_ifaddr));
    ia->nif = nif;
    ia->addr = addr;
    ia->prefix_len = prefix_len;
    ia->netmask = ipv4_prefix_to_mask(prefix_len);
    ia->broadcast.s_addr = addr.s_addr | ~ia->netmask.s_addr;
    ia->valid_until = valid_until;
    dlist_insert_tail(&nif->ipv4_addrs, &ia->link);
    // TODO: route_add4 for the connected prefix
    return 0;
}

int ipv4_addr_del(struct netif* nif, const struct in_addr addr)
{
    for (struct dlist_node* n = dlist_first(&nif->ipv4_addrs); n; n = dlist_next(n)) {
        struct ipv4_ifaddr* ia = dlist_entry(n, struct ipv4_ifaddr, link);
        if (ia->addr.s_addr == addr.s_addr) {
            dlist_remove(&nif->ipv4_addrs, n);
            free(ia);
            return 0;
        }
    }
    return -EADDRNOTAVAIL;
}

struct ipv4_ifaddr* ipv4_addr_for_netif(const struct netif* nif)
{
    struct dlist_node* n = dlist_first(&nif->ipv4_addrs);
    return n ? dlist_entry(n, struct ipv4_ifaddr, link) : nullptr;
}

struct ipv4_ifaddr* ipv4_addr_find(const struct in_addr addr)
{
    for (const struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        for (struct dlist_node* n = dlist_first(&nif->ipv4_addrs); n; n = dlist_next(n)) {
            struct ipv4_ifaddr* ia = dlist_entry(n, struct ipv4_ifaddr, link);
            if (ia->addr.s_addr == addr.s_addr)
                return ia;
        }
    }
    return nullptr;
}

bool ipv4_is_local(const struct in_addr addr)
{
    return ipv4_addr_find(addr) != nullptr;
}

bool ipv4_is_broadcast(const struct netif* nif, const struct in_addr addr)
{
    if (addr.s_addr == INADDR_BROADCAST)
        return true;
    for (struct dlist_node* n = dlist_first(&nif->ipv4_addrs); n; n = dlist_next(n)) {
        const struct ipv4_ifaddr* ia = dlist_entry(n, struct ipv4_ifaddr, link);
        if (ia->prefix_len < 31 && addr.s_addr == ia->broadcast.s_addr)
            return true;
    }
    return false;
}

bool ipv4_is_multicast(const struct in_addr addr)
{
    return (ntohl(addr.s_addr) & 0xF0000000u) == 0xE0000000u; // 224.0.0.0/4
}
