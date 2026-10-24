#ifndef NET_IPV6_ND6_H
#define NET_IPV6_ND6_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <utils/queue/queue.h>
#include <drivers/net/ipv6/ipv6.h>

#define ND6_MAX_MULTICAST_SOLICIT 3
#define ND6_MAX_UNICAST_SOLICIT   3
#define ND6_RETRANS_TIMER_MS      1000
#define ND6_REACHABLE_TIME_MS     30000
#define ND6_DELAY_FIRST_PROBE_MS  5000
#define ND6_MAX_RTR_SOLICITATIONS 3
#define ND6_RTR_SOLICIT_INTERVAL_MS 4000
#define ND6_DAD_TRANSMITS         1

enum nd6_option
{
    ND6_OPT_SOURCE_LLADDR = 1,
    ND6_OPT_TARGET_LLADDR = 2,
    ND6_OPT_PREFIX_INFO = 3,
    ND6_OPT_MTU = 5,
    ND6_OPT_RDNSS = 25
};

enum nd6_state {
    ND6_INCOMPLETE,
    ND6_REACHABLE,
    ND6_STALE,
    ND6_DELAY,
    ND6_PROBE,
};

struct netif;
struct netbuf;

struct nd6_neighbor {
    struct dlist_node link;
    struct in6_addr addr;
    uint8_t hw[6];
    enum nd6_state state;
    bool is_router;
    uint8_t probes;
    uint64_t expires;
    struct netbuf* pending;
};

struct nd6_prefix {
    struct dlist_node link;
    struct in6_addr prefix;
    uint8_t prefix_len;
    bool on_link;
    bool autonomous;
    uint64_t preferred_until;
    uint64_t valid_until;
};

struct nd6_router {
    struct dlist_node link;
    struct in6_addr addr;
    uint64_t expires;
    bool managed;
    bool other_config;
};

struct nd6_ifinfo {
    struct dlist neighbors;
    struct dlist prefixes;
    struct dlist routers;
    uint32_t link_mtu;
    uint32_t reachable_time_ms;
    uint32_t retrans_time_ms;
    uint8_t cur_hop_limit;
    uint8_t rs_sent;
    bool rs_done;
    uint64_t next_rs;
    bool has_default;
    struct in6_addr default_gw;
};

void nd6_initialize();
void nd6_ifattach(struct netif* nif);
void nd6_ifdetach(struct netif* nif);

int nd6_resolve(struct netif* nif, const struct in6_addr* addr, struct netbuf* nb, uint8_t* out_hw);

void nd6_ns_input(struct netbuf* nb, size_t offset);
int nd6_ns_output(struct netif* nif, const struct in6_addr* target, const struct in6_addr* dst, bool dad);
void nd6_na_input(struct netbuf* nb, size_t offset);
int nd6_na_output(struct netif* nif, const struct in6_addr* target, const struct in6_addr* dst,
                  bool solicited, bool override);
int nd6_rs_output(struct netif* nif);
void nd6_ra_input(struct netbuf* nb, size_t offset);
void nd6_redirect_input(struct netbuf* nb, size_t offset);

void nd6_dad_start(struct netif* nif, const struct in6_addr* addr);
void nd6_dad_stop(const struct in6_addr* addr);
void nd6_dad_timer(void* arg);

int nd6_slaac_prefix_add(struct netif* nif, const struct in6_addr* prefix, uint8_t prefix_len,
                         uint32_t valid_lifetime, uint32_t preferred_lifetime);

struct nd6_neighbor* nd6_cache_lookup(struct netif* nif, const struct in6_addr* addr);
void nd6_cache_flush(struct netif* nif);
struct nd6_router* nd6_default_router(struct netif* nif);

void nd6_timer(void* arg);

#endif //NET_IPV6_ND6_H
