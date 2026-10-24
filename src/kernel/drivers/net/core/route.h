#ifndef NET_CORE_ROUTE_H
#define NET_CORE_ROUTE_H

#include <os/stdint.h>
#include <utils/queue/queue.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/ipv6.h>

enum route_flags
{
    ROUTE_UP = 0x01,
    ROUTE_GATEWAY = 0x02,
    ROUTE_HOST = 0x04,
    ROUTE_STATIC = 0x08,
    ROUTE_DYNAMIC = 0x10
};

struct netif;

struct route_nexthop {
    struct netif* nif;
    uint16_t family;
    union {
        struct in_addr gw4;
        struct in6_addr gw6;
    };
    uint32_t mtu;
};

struct route_entry {
    struct dlist_node link;
    uint16_t family;
    union {
        struct in_addr dst4;
        struct in6_addr dst6;
    };
    uint8_t prefix_len;
    uint32_t flags;
    uint32_t metric;
    uint64_t expires;
    struct route_nexthop nexthop;
};

void route_initialize();

int route_add4(struct in_addr dst, uint8_t prefix_len, struct in_addr gw, struct netif* nif, uint32_t flags);
int route_del4(struct in_addr dst, uint8_t prefix_len);

bool route_lookup4(struct in_addr dst, struct route_nexthop* out);
int route_set_default4(struct in_addr gw, struct netif* nif);

int route_add6(const struct in6_addr* dst, uint8_t prefix_len, const struct in6_addr* gw, struct netif* nif, uint32_t flags);
int route_del6(const struct in6_addr* dst, uint8_t prefix_len);
bool route_lookup6(const struct in6_addr* dst, struct route_nexthop* out);
int route_set_default6(const struct in6_addr* gw, struct netif* nif);

void route_flush_netif(const struct netif* nif);

void route_timer();

#endif //NET_CORE_ROUTE_H
