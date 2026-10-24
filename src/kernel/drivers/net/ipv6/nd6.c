#include <drivers/net/ipv6/nd6.h>

#include <drivers/net/core/netif.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/route.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/random.h>
#include <drivers/net/ethernet/ethernet.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/dhcp6/dhcp6.h>
#include <drivers/net/ipv6/icmp6.h>
#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#define ND6_TIMER_MS         100
#define ND6_MAX_NEIGHBORS    256
#define ND6_MAX_PENDING      4
#define ND6_STALE_GC_MS      (20 * 60 * 1000)
#define ND6_MAX_RS_DELAY_MS  1000
#define ND6_MAX_TX           16
#define ND6_TWO_HOURS_S      (2 * 60 * 60)
#define ND6_INFINITE         0xFFFFFFFFu

#define ND6_NA_ROUTER    0x80
#define ND6_NA_SOLICITED 0x40
#define ND6_NA_OVERRIDE  0x20

#define ND6_RA_MANAGED 0x80
#define ND6_RA_OTHER   0x40

#define ND6_PI_ONLINK 0x80
#define ND6_PI_AUTO   0x40

struct nd6_dad {
    struct dlist_node link;
    struct netif* nif;
    struct ipv6_ifaddr* ifa;
    uint8_t ns_sent;
    struct callout timer;
};

struct nd6_opts {
    const uint8_t* src_lladdr;
    const uint8_t* tgt_lladdr;
    uint32_t mtu;
    const uint8_t* prefixes[8];
    uint8_t n_prefixes;
};

struct nd6_tx {
    struct netif* nif;
    struct in6_addr target;
    struct in6_addr dst;
    bool rs;
};

static spinlock_t nd6_lock;
static struct dlist dads;
static struct callout nd6_callout;

static const struct in6_addr nd6_all_nodes = { .s6_addr = { 0xFF, 0x02, [15] = 1 } };
static const struct in6_addr nd6_all_routers = { .s6_addr = { 0xFF, 0x02, [15] = 2 } };

static uint64_t nd6_ms(const uint64_t ms)
{
    return ticks_from_ms(ms);
}

static uint64_t nd6_lifetime(const uint32_t seconds, const uint64_t now)
{
    return seconds == ND6_INFINITE ? 0 : now + nd6_ms((uint64_t)seconds * 1000);
}

static struct nd6_ifinfo* nd6_info(const struct netif* nif)
{
    return nif->nd6_info;
}

void nd6_initialize()
{
    spinlock_init(&nd6_lock, "nd6");
    dlist_init(&dads);
    callout_init(&nd6_callout);
    callout_reset(&nd6_callout, nd6_ms(ND6_TIMER_MS), nd6_timer, nullptr);
}

void nd6_ifattach(struct netif* nif)
{
    if (nif->nd6_info)
        return;
    struct nd6_ifinfo* info = malloc(sizeof(struct nd6_ifinfo));
    if (!info)
        return;
    memset(info, 0, sizeof(struct nd6_ifinfo));
    dlist_init(&info->neighbors);
    dlist_init(&info->prefixes);
    dlist_init(&info->routers);
    info->reachable_time_ms = ND6_REACHABLE_TIME_MS;
    info->retrans_time_ms = ND6_RETRANS_TIMER_MS;
    info->cur_hop_limit = IPV6_DEFAULT_HLIM;
    info->link_mtu = nif->mtu;
    nif->nd6_info = info;
}

static void nd6_free_pending(struct netbuf* nb)
{
    while (nb) {
        struct netbuf* next = nb->next_pkt;
        netbuf_free_chain(nb);
        nb = next;
    }
}

static void nd6_free_list(struct dlist* list, const bool neighbors)
{
    struct dlist_node* n;
    while ((n = dlist_first(list))) {
        dlist_remove(list, n);
        if (neighbors)
            nd6_free_pending(dlist_entry(n, struct nd6_neighbor, link)->pending);
        free(n);
    }
}

void nd6_ifdetach(struct netif* nif)
{
    struct nd6_ifinfo* info = nd6_info(nif);
    if (!info)
        return;
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    nif->nd6_info = nullptr;
    spinlock_release_irqrestore(&nd6_lock, flags);
    if (info->has_default)
        route_del6(&in6addr_any, 0);
    for (struct dlist_node* n = dlist_first(&info->prefixes); n; n = dlist_next(n)) {
        const auto p = dlist_entry(n, struct nd6_prefix, link);
        route_del6(&p->prefix, p->prefix_len);
    }
    nd6_free_list(&info->neighbors, true);
    nd6_free_list(&info->prefixes, false);
    nd6_free_list(&info->routers, false);
    free(info);
}

static struct nd6_neighbor* nd6_find(const struct nd6_ifinfo* info, const struct in6_addr* addr)
{
    for (struct dlist_node* n = dlist_first(&info->neighbors); n; n = dlist_next(n)) {
        const auto e = dlist_entry(n, struct nd6_neighbor, link);
        if (ipv6_addr_equal(&e->addr, addr))
            return e;
    }
    return nullptr;
}

static struct nd6_neighbor* nd6_create(struct nd6_ifinfo* info, const struct in6_addr* addr)
{
    if (info->neighbors.count >= ND6_MAX_NEIGHBORS)
        return nullptr;
    struct nd6_neighbor* e = malloc(sizeof(struct nd6_neighbor));
    if (!e)
        return nullptr;
    memset(e, 0, sizeof(struct nd6_neighbor));
    e->addr = *addr;
    e->state = ND6_INCOMPLETE;
    dlist_insert_tail(&info->neighbors, &e->link);
    return e;
}

static void nd6_set_state(const struct nd6_ifinfo* info, struct nd6_neighbor* e, const enum nd6_state state)
{
    const uint64_t now = ticks_get();
    e->state = state;
    e->probes = 0;
    switch (state) {
    case ND6_REACHABLE:
        e->expires = now + nd6_ms(info->reachable_time_ms);
        break;
    case ND6_STALE:
        e->expires = now + nd6_ms(ND6_STALE_GC_MS);
        break;
    case ND6_DELAY:
        e->expires = now + nd6_ms(ND6_DELAY_FIRST_PROBE_MS);
        break;
    default:
        e->expires = now + nd6_ms(info->retrans_time_ms);
        break;
    }
}

static struct netbuf* nd6_take_pending(struct nd6_neighbor* e)
{
    struct netbuf* nb = e->pending;
    e->pending = nullptr;
    return nb;
}

static void nd6_send_pending(struct netif* nif, struct netbuf* nb, const uint8_t* hw)
{
    while (nb) {
        struct netbuf* next = nb->next_pkt;
        nb->next_pkt = nullptr;
        eth_output_frame(nif, nb, hw, ETHERTYPE_IPV6);
        nb = next;
    }
}

static void nd6_learn(struct netif* nif, const struct in6_addr* addr, const uint8_t* lladdr, const bool is_router)
{
    struct nd6_ifinfo* info = nd6_info(nif);
    if (!info || !lladdr || ipv6_addr_is_unspecified(addr))
        return;
    struct netbuf* pending = nullptr;
    uint8_t hw[ETH_ADDR_LEN];
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    struct nd6_neighbor* e = nd6_find(info, addr);
    if (!e)
        e = nd6_create(info, addr);
    if (e) {
        if (e->state == ND6_INCOMPLETE || memcmp(e->hw, (void*)lladdr, ETH_ADDR_LEN)) {
            memcpy(e->hw, lladdr, ETH_ADDR_LEN);
            nd6_set_state(info, e, ND6_STALE);
        }
        if (is_router)
            e->is_router = true;
        pending = nd6_take_pending(e);
        memcpy(hw, e->hw, ETH_ADDR_LEN);
    }
    spinlock_release_irqrestore(&nd6_lock, flags);
    nd6_send_pending(nif, pending, hw);
}

struct nd6_neighbor* nd6_cache_lookup(struct netif* nif, const struct in6_addr* addr)
{
    const struct nd6_ifinfo* info = nd6_info(nif);
    if (!info)
        return nullptr;
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    struct nd6_neighbor* e = nd6_find(info, addr);
    spinlock_release_irqrestore(&nd6_lock, flags);
    return e;
}

void nd6_cache_flush(struct netif* nif)
{
    struct nd6_ifinfo* info = nd6_info(nif);
    if (!info)
        return;
    struct dlist doomed;
    dlist_init(&doomed);
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    struct dlist_node* n;
    while ((n = dlist_first(&info->neighbors))) {
        dlist_remove(&info->neighbors, n);
        dlist_insert_tail(&doomed, n);
    }
    spinlock_release_irqrestore(&nd6_lock, flags);
    nd6_free_list(&doomed, true);
}

static void nd6_solicit_source(struct netif* nif, const struct netbuf* nb, const struct in6_addr* target,
                               struct in6_addr* out)
{
    if (nb && nb->pkt_len >= IPV6_HEADER_LEN) {
        struct ipv6_header ip;
        netbuf_copydata(nb, 0, sizeof(ip), &ip);
        const struct ipv6_ifaddr* ifa = ipv6_addr_find(&ip.src);
        if (ifa && ifa->nif == nif && !(ifa->flags & (IPV6_ADDR_TENTATIVE | IPV6_ADDR_DUPLICATED))) {
            *out = ip.src;
            return;
        }
    }
    if (ipv6_select_source(nif, target, out) < 0)
        *out = in6addr_any;
}

static int nd6_ns_send(struct netif* nif, const struct in6_addr* target, const struct in6_addr* dst,
                       const struct in6_addr* src);

int nd6_resolve(struct netif* nif, const struct in6_addr* addr, struct netbuf* nb, uint8_t* out_hw)
{
    if (ipv6_addr_is_multicast(addr)) {
        eth_map_multicast6(addr, out_hw);
        return 0;
    }
    struct nd6_ifinfo* info = nd6_info(nif);
    if (!info) {
        if (nb)
            netbuf_free_chain(nb);
        return -ENETDOWN;
    }

    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    struct nd6_neighbor* e = nd6_find(info, addr);
    if (e && e->state != ND6_INCOMPLETE) {
        memcpy(out_hw, e->hw, ETH_ADDR_LEN);
        if (e->state == ND6_STALE)
            nd6_set_state(info, e, ND6_DELAY);
        spinlock_release_irqrestore(&nd6_lock, flags);
        return 0;
    }

    const bool fresh = !e;
    if (!e)
        e = nd6_create(info, addr);
    if (!e) {
        spinlock_release_irqrestore(&nd6_lock, flags);
        if (nb)
            netbuf_free_chain(nb);
        return -ENOBUFS;
    }
    if (nb) {
        size_t queued = 0;
        struct netbuf** pp = &e->pending;
        while (*pp) {
            queued++;
            pp = &(*pp)->next_pkt;
        }
        nb->next_pkt = nullptr;
        *pp = nb;
        if (queued >= ND6_MAX_PENDING) {
            struct netbuf* old = e->pending;
            e->pending = old->next_pkt;
            old->next_pkt = nullptr;
            netbuf_free_chain(old);
        }
    }
    struct in6_addr src = in6addr_any;
    if (fresh) {
        nd6_set_state(info, e, ND6_INCOMPLETE);
        e->probes = 1;
    }
    spinlock_release_irqrestore(&nd6_lock, flags);

    if (fresh) {
        nd6_solicit_source(nif, nb, addr, &src);
        nd6_ns_send(nif, addr, nullptr, &src);
    }
    return -EINPROGRESS;
}

static struct netbuf* nd6_alloc(const size_t len)
{
    struct netbuf* nb = netbuf_alloc_packet(len);
    if (!nb)
        return nullptr;
    const uint8_t zero[64] = { 0 };
    if (!netbuf_append(nb, zero, len)) {
        netbuf_free_chain(nb);
        return nullptr;
    }
    return nb;
}

static void nd6_put_lladdr(uint8_t* opt, const uint8_t type, const struct netif* nif)
{
    opt[0] = type;
    opt[1] = 1;
    memcpy(opt + 2, nif->hw_addr, ETH_ADDR_LEN);
}

static int nd6_ns_send(struct netif* nif, const struct in6_addr* target, const struct in6_addr* dst,
                       const struct in6_addr* src)
{
    const bool unspecified = ipv6_addr_is_unspecified(src);
    const size_t len = 24 + (unspecified ? 0 : 8);
    struct netbuf* nb = nd6_alloc(len);
    if (!nb)
        return -ENOBUFS;
    uint8_t* m = nb->data;
    m[0] = ICMP6_ND_NEIGHBOR_SOLICIT;
    memcpy(m + 8, target, sizeof(struct in6_addr));
    if (!unspecified)
        nd6_put_lladdr(m + 24, ND6_OPT_SOURCE_LLADDR, nif);

    struct in6_addr group;
    if (!dst) {
        ipv6_solicited_node(target, &group);
        dst = &group;
    }
    return icmp6_output(nif, nb, src, dst, 255);
}

int nd6_ns_output(struct netif* nif, const struct in6_addr* target, const struct in6_addr* dst, const bool dad)
{
    struct in6_addr src = in6addr_any;
    if (!dad && ipv6_select_source(nif, target, &src) < 0)
        return -EADDRNOTAVAIL;
    return nd6_ns_send(nif, target, dst, &src);
}

int nd6_na_output(struct netif* nif, const struct in6_addr* target, const struct in6_addr* dst,
                  const bool solicited, const bool override)
{
    struct netbuf* nb = nd6_alloc(32);
    if (!nb)
        return -ENOBUFS;
    uint8_t* m = nb->data;
    m[0] = ICMP6_ND_NEIGHBOR_ADVERT;
    m[4] = (solicited ? ND6_NA_SOLICITED : 0) | (override ? ND6_NA_OVERRIDE : 0);
    memcpy(m + 8, target, sizeof(struct in6_addr));
    nd6_put_lladdr(m + 24, ND6_OPT_TARGET_LLADDR, nif);
    return icmp6_output(nif, nb, target, dst, 255);
}

int nd6_rs_output(struct netif* nif)
{
    const struct ipv6_ifaddr* ll = ipv6_addr_linklocal(nif);
    const bool have_ll = ll && !(ll->flags & (IPV6_ADDR_TENTATIVE | IPV6_ADDR_DUPLICATED));
    struct netbuf* nb = nd6_alloc(8 + (have_ll ? 8 : 0));
    if (!nb)
        return -ENOBUFS;
    uint8_t* m = nb->data;
    m[0] = ICMP6_ND_ROUTER_SOLICIT;
    if (have_ll)
        nd6_put_lladdr(m + 8, ND6_OPT_SOURCE_LLADDR, nif);
    return icmp6_output(nif, nb, have_ll ? &ll->addr : &in6addr_any, &nd6_all_routers, 255);
}

static bool nd6_parse_options(const uint8_t* p, const size_t len, struct nd6_opts* out)
{
    memset(out, 0, sizeof(struct nd6_opts));
    size_t i = 0;
    while (i + 2 <= len) {
        const size_t olen = (size_t)p[i + 1] * 8;
        if (olen == 0 || i + olen > len)
            return false;
        const uint8_t* o = p + i;
        switch (o[0]) {
        case ND6_OPT_SOURCE_LLADDR:
            if (olen >= 8)
                out->src_lladdr = o + 2;
            break;
        case ND6_OPT_TARGET_LLADDR:
            if (olen >= 8)
                out->tgt_lladdr = o + 2;
            break;
        case ND6_OPT_MTU:
            if (olen == 8)
                out->mtu = (uint32_t)o[4] << 24 | (uint32_t)o[5] << 16 | (uint32_t)o[6] << 8 | o[7];
            break;
        case ND6_OPT_PREFIX_INFO:
            if (olen == 32 && out->n_prefixes < 8)
                out->prefixes[out->n_prefixes++] = o;
            break;
        default:
            break;  // TODO: RDNSS (RFC 8106) once the resolver takes v6 servers
        }
        i += olen;
    }
    return i == len;
}

static uint32_t nd6_get32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static const uint8_t* nd6_message(struct netbuf** nbp, const size_t offset, const size_t min_len,
                                  struct ipv6_header* ip, size_t* len)
{
    struct netbuf* nb = *nbp;
    *len = nb->pkt_len - offset;
    netbuf_copydata(nb, 0, sizeof(*ip), ip);
    if (*len < min_len || ip->hop_limit != 255 || offset + *len > NETBUF_CLUSTER_SIZE - NETBUF_HEADROOM)
        goto drop;
    nb = netbuf_pullup(nb, offset + *len);
    *nbp = nb;
    if (!nb)
        return nullptr;
    const uint8_t* m = nb->data + offset;
    if (m[1] != 0)
        goto drop;
    return m;
drop:
    netbuf_free_chain(*nbp);
    *nbp = nullptr;
    return nullptr;
}

static bool nd6_is_solicited_node(const struct in6_addr* a)
{
    static const uint8_t prefix[13] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xFF };
    for (int i = 0; i < 13; i++)
        if (a->s6_addr[i] != prefix[i])
            return false;
    return true;
}

static struct nd6_dad* nd6_dad_find(const struct ipv6_ifaddr* ifa)
{
    for (struct dlist_node* n = dlist_first(&dads); n; n = dlist_next(n)) {
        const auto d = dlist_entry(n, struct nd6_dad, link);
        if (d->ifa == ifa)
            return d;
    }
    return nullptr;
}

void nd6_dad_start(struct netif* nif, struct ipv6_ifaddr* ifa)
{
    struct nd6_dad* d = malloc(sizeof(struct nd6_dad));
    if (!d)
        return;
    memset(d, 0, sizeof(struct nd6_dad));
    d->nif = nif;
    d->ifa = ifa;
    callout_init(&d->timer);
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    if (nd6_dad_find(ifa)) {
        spinlock_release_irqrestore(&nd6_lock, flags);
        free(d);
        return;
    }
    dlist_insert_tail(&dads, &d->link);

    callout_reset(&d->timer, 1 + nd6_ms(net_random32() % ND6_MAX_RS_DELAY_MS), nd6_dad_timer, d);
    spinlock_release_irqrestore(&nd6_lock, flags);
}

void nd6_dad_stop(struct ipv6_ifaddr* ifa)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    struct nd6_dad* d = nd6_dad_find(ifa);
    if (d) {
        callout_stop(&d->timer);
        dlist_remove(&dads, &d->link);
    }
    spinlock_release_irqrestore(&nd6_lock, flags);
    if (d)
        free(d);
}

static void nd6_dad_duplicated(struct ipv6_ifaddr* ifa)
{
    ifa->flags &= ~IPV6_ADDR_TENTATIVE;
    ifa->flags |= IPV6_ADDR_DUPLICATED;
    nd6_dad_stop(ifa);
}

void nd6_dad_timer(void* arg)
{
    struct nd6_dad* d = arg;
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    if (nd6_dad_find(d->ifa) != d) {
        spinlock_release_irqrestore(&nd6_lock, flags);
        return;
    }
    struct netif* nif = d->nif;
    struct ipv6_ifaddr* ifa = d->ifa;
    const struct nd6_ifinfo* info = nd6_info(nif);
    const uint32_t retrans = info ? info->retrans_time_ms : ND6_RETRANS_TIMER_MS;

    if (d->ns_sent < ND6_DAD_TRANSMITS) {
        d->ns_sent++;
        callout_reset(&d->timer, nd6_ms(retrans), nd6_dad_timer, d);
        const struct in6_addr target = ifa->addr;
        spinlock_release_irqrestore(&nd6_lock, flags);
        nd6_ns_send(nif, &target, nullptr, &in6addr_any);
        return;
    }

    dlist_remove(&dads, &d->link);
    ifa->flags &= ~IPV6_ADDR_TENTATIVE;
    struct nd6_ifinfo* nd = nd6_info(nif);
    if ((ifa->flags & IPV6_ADDR_LINKLOCAL) && nd && !nd->rs_done && !nd->next_rs && !nd->rs_sent)
        nd->next_rs = ticks_get() + 1 + nd6_ms(net_random32() % ND6_MAX_RS_DELAY_MS);
    spinlock_release_irqrestore(&nd6_lock, flags);
    free(d);
}

void nd6_ns_input(struct netbuf* nb, const size_t offset)
{
    struct ipv6_header ip;
    size_t len;
    const uint8_t* m = nd6_message(&nb, offset, 24, &ip, &len);
    if (!m)
        return;
    struct netif* nif = nb->rcvif;
    struct in6_addr target;
    memcpy(&target, m + 8, sizeof(target));
    struct nd6_opts opts;
    const bool from_unspec = ipv6_addr_is_unspecified(&ip.src);
    if (!nif || ipv6_addr_is_multicast(&target) || !nd6_parse_options(m + 24, len - 24, &opts) ||
        (from_unspec && (!nd6_is_solicited_node(&ip.dst) || opts.src_lladdr))) {
        netbuf_free_chain(nb);
        return;
    }
    uint8_t sll[ETH_ADDR_LEN];
    const bool have_sll = opts.src_lladdr;
    if (have_sll)
        memcpy(sll, opts.src_lladdr, ETH_ADDR_LEN);
    netbuf_free_chain(nb);

    struct ipv6_ifaddr* ifa = ipv6_addr_find(&target);
    if (!ifa || ifa->nif != nif || (ifa->flags & IPV6_ADDR_DUPLICATED))
        return;
    if (ifa->flags & IPV6_ADDR_TENTATIVE) {
        if (from_unspec)
            nd6_dad_duplicated(ifa);
        return;
    }
    if (from_unspec) {
        nd6_na_output(nif, &target, &nd6_all_nodes, false, true);
        return;
    }
    if (have_sll)
        nd6_learn(nif, &ip.src, sll, false);
    nd6_na_output(nif, &target, &ip.src, true, true);
}

static void nd6_router_remove(struct nd6_ifinfo* info, const struct in6_addr* addr);

void nd6_na_input(struct netbuf* nb, const size_t offset)
{
    struct ipv6_header ip;
    size_t len;
    const uint8_t* m = nd6_message(&nb, offset, 24, &ip, &len);
    if (!m)
        return;
    struct netif* nif = nb->rcvif;
    const uint8_t na_flags = m[4];
    struct in6_addr target;
    memcpy(&target, m + 8, sizeof(target));
    struct nd6_opts opts;
    const bool solicited = na_flags & ND6_NA_SOLICITED;
    if (!nif || ipv6_addr_is_multicast(&target) || (ipv6_addr_is_multicast(&ip.dst) && solicited) ||
        !nd6_parse_options(m + 24, len - 24, &opts)) {
        netbuf_free_chain(nb);
        return;
    }
    uint8_t tll[ETH_ADDR_LEN];
    const bool have_tll = opts.tgt_lladdr;
    if (have_tll)
        memcpy(tll, opts.tgt_lladdr, ETH_ADDR_LEN);
    netbuf_free_chain(nb);

    struct ipv6_ifaddr* ifa = ipv6_addr_find(&target);
    if (ifa) {
        if (ifa->flags & IPV6_ADDR_TENTATIVE)
            nd6_dad_duplicated(ifa);
        // TODO: log a conflict on an address already in use
        return;
    }

    struct nd6_ifinfo* info = nd6_info(nif);
    if (!info)
        return;
    const bool override = na_flags & ND6_NA_OVERRIDE;
    const bool router = na_flags & ND6_NA_ROUTER;
    struct netbuf* pending = nullptr;
    uint8_t hw[ETH_ADDR_LEN];
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    struct nd6_neighbor* e = nd6_find(info, &target);
    if (!e) {
        spinlock_release_irqrestore(&nd6_lock, flags);
        return;
    }
    if (e->state == ND6_INCOMPLETE) {
        if (!have_tll) {
            spinlock_release_irqrestore(&nd6_lock, flags);
            return;
        }
        memcpy(e->hw, tll, ETH_ADDR_LEN);
        nd6_set_state(info, e, solicited ? ND6_REACHABLE : ND6_STALE);
        e->is_router = router;
        pending = nd6_take_pending(e);
    } else {
        const bool differs = have_tll && memcmp(tll, e->hw, ETH_ADDR_LEN);
        if (!override && differs) {
            if (e->state == ND6_REACHABLE)
                nd6_set_state(info, e, ND6_STALE);
        } else {
            if (have_tll)
                memcpy(e->hw, tll, ETH_ADDR_LEN);
            if (solicited)
                nd6_set_state(info, e, ND6_REACHABLE);
            else if (differs)
                nd6_set_state(info, e, ND6_STALE);
            if (e->is_router && !router)
                nd6_router_remove(info, &target);
            e->is_router = router;
        }
    }
    memcpy(hw, e->hw, ETH_ADDR_LEN);
    spinlock_release_irqrestore(&nd6_lock, flags);
    nd6_send_pending(nif, pending, hw);
}

// TODO: honour redirects (RFC 4861 8.3); a host may ignore them
void nd6_redirect_input(struct netbuf* nb, const size_t offset)
{
    (void)offset;
    netbuf_free_chain(nb);
}

static struct nd6_router* nd6_router_find(const struct nd6_ifinfo* info, const struct in6_addr* addr)
{
    for (struct dlist_node* n = dlist_first(&info->routers); n; n = dlist_next(n)) {
        const auto r = dlist_entry(n, struct nd6_router, link);
        if (ipv6_addr_equal(&r->addr, addr))
            return r;
    }
    return nullptr;
}

static void nd6_router_remove(struct nd6_ifinfo* info, const struct in6_addr* addr)
{
    struct nd6_router* r = nd6_router_find(info, addr);
    if (r) {
        dlist_remove(&info->routers, &r->link);
        free(r);
    }
}

struct nd6_router* nd6_default_router(struct netif* nif)
{
    const struct nd6_ifinfo* info = nd6_info(nif);
    if (!info)
        return nullptr;
    struct dlist_node* n = dlist_first(&info->routers);
    return n ? dlist_entry(n, struct nd6_router, link) : nullptr;
}

static void nd6_update_default_route(struct netif* nif)
{
    struct nd6_ifinfo* info = nd6_info(nif);
    if (!info)
        return;
    struct in6_addr gw;
    bool have = false;
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    struct dlist_node* n = dlist_first(&info->routers);
    if (n) {
        gw = dlist_entry(n, struct nd6_router, link)->addr;
        have = true;
    }
    const bool changed = have != info->has_default || (have && !ipv6_addr_equal(&gw, &info->default_gw));
    const bool had = info->has_default;
    info->has_default = have;
    if (have)
        info->default_gw = gw;
    spinlock_release_irqrestore(&nd6_lock, flags);

    if (!changed)
        return;
    if (had)
        route_del6(&in6addr_any, 0);
    if (have && route_add6(&in6addr_any, 0, &gw, nif, ROUTE_DYNAMIC) < 0)
        info->has_default = false;
}

static struct nd6_prefix* nd6_prefix_find(const struct nd6_ifinfo* info, const struct in6_addr* prefix,
                                          const uint8_t len)
{
    for (struct dlist_node* n = dlist_first(&info->prefixes); n; n = dlist_next(n)) {
        const auto p = dlist_entry(n, struct nd6_prefix, link);
        if (p->prefix_len == len && ipv6_prefix_match(&p->prefix, prefix, len))
            return p;
    }
    return nullptr;
}

static void nd6_prefix_onlink(struct netif* nif, const struct in6_addr* prefix, const uint8_t len,
                              const uint32_t valid)
{
    struct nd6_ifinfo* info = nd6_info(nif);
    const uint64_t now = ticks_get();
    bool add_route = false, del_route = false;
    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    struct nd6_prefix* p = nd6_prefix_find(info, prefix, len);
    if (!p && valid) {
        p = malloc(sizeof(struct nd6_prefix));
        if (p) {
            memset(p, 0, sizeof(struct nd6_prefix));
            p->prefix = *prefix;
            p->prefix_len = len;
            p->on_link = true;
            dlist_insert_tail(&info->prefixes, &p->link);
            add_route = true;
        }
    }
    if (p && valid) {
        p->valid_until = nd6_lifetime(valid, now);
    } else if (p) {
        dlist_remove(&info->prefixes, &p->link);
        free(p);
        del_route = true;
    }
    spinlock_release_irqrestore(&nd6_lock, flags);
    if (add_route)
        route_add6(prefix, len, nullptr, nif, ROUTE_DYNAMIC);
    if (del_route)
        route_del6(prefix, len);
}

int nd6_slaac_prefix_add(struct netif* nif, const struct in6_addr* prefix, const uint8_t prefix_len,
                         const uint32_t valid_lifetime, const uint32_t preferred_lifetime)
{
    if (prefix_len != 64 || preferred_lifetime > valid_lifetime || ipv6_addr_is_linklocal(prefix))
        return -EINVAL;
    struct in6_addr addr;
    ipv6_linklocal_from_hw(nif->hw_addr, &addr);
    memcpy(addr.s6_addr, prefix->s6_addr, 8);

    const uint64_t now = ticks_get();
    struct ipv6_ifaddr* ifa = ipv6_addr_find(&addr);
    if (!ifa) {
        if (!valid_lifetime)
            return 0;
        return ipv6_addr_add(nif, &addr, prefix_len, IPV6_ADDR_TENTATIVE | IPV6_ADDR_AUTOCONF,
                             nd6_lifetime(preferred_lifetime, now), nd6_lifetime(valid_lifetime, now));
    }
    if (!(ifa->flags & IPV6_ADDR_AUTOCONF))
        return 0;

    ifa->preferred_until = nd6_lifetime(preferred_lifetime, now);
    if (preferred_lifetime)
        ifa->flags &= ~IPV6_ADDR_DEPRECATED;

    const uint64_t two_hours = now + nd6_ms((uint64_t)ND6_TWO_HOURS_S * 1000);
    const uint64_t proposed = nd6_lifetime(valid_lifetime, now);
    const bool infinite_now = ifa->valid_until == 0;
    if (proposed == 0 || valid_lifetime > ND6_TWO_HOURS_S || (!infinite_now && proposed > ifa->valid_until))
        ifa->valid_until = proposed;
    else if (infinite_now || ifa->valid_until > two_hours)
        ifa->valid_until = two_hours;
    return 0;
}

void nd6_ra_input(struct netbuf* nb, const size_t offset)
{
    struct ipv6_header ip;
    size_t len;
    const uint8_t* m = nd6_message(&nb, offset, 16, &ip, &len);
    if (!m)
        return;
    struct netif* nif = nb->rcvif;
    struct nd6_opts opts;
    struct nd6_ifinfo* info = nif ? nd6_info(nif) : nullptr;
    if (!info || !ipv6_addr_is_linklocal(&ip.src) || !nd6_parse_options(m + 16, len - 16, &opts)) {
        netbuf_free_chain(nb);
        return;
    }

    const uint8_t cur_hlim = m[4];
    const uint8_t ra_flags = m[5];
    const uint16_t lifetime = (uint16_t)(m[6] << 8 | m[7]);
    const uint32_t reachable = nd6_get32(m + 8);
    const uint32_t retrans = nd6_get32(m + 12);
    const uint64_t now = ticks_get();

    uint8_t sll[ETH_ADDR_LEN];
    const bool have_sll = opts.src_lladdr;
    if (have_sll)
        memcpy(sll, opts.src_lladdr, ETH_ADDR_LEN);
    struct {
        struct in6_addr prefix;
        uint8_t len, flags;
        uint32_t valid, preferred;
    } pi[8];
    const uint8_t n_pi = opts.n_prefixes;
    for (uint8_t i = 0; i < n_pi; i++) {
        const uint8_t* o = opts.prefixes[i];
        pi[i].len = o[2];
        pi[i].flags = o[3];
        pi[i].valid = nd6_get32(o + 4);
        pi[i].preferred = nd6_get32(o + 8);
        memcpy(&pi[i].prefix, o + 16, sizeof(struct in6_addr));
    }
    const uint32_t mtu = opts.mtu;
    netbuf_free_chain(nb);

    uint64_t flags;
    spinlock_acquire_irqsave(&nd6_lock, &flags);
    info->rs_done = true;
    info->next_rs = 0;
    if (cur_hlim)
        info->cur_hop_limit = cur_hlim;
    if (reachable)
        info->reachable_time_ms = reachable;
    if (retrans)
        info->retrans_time_ms = retrans;
    if (mtu >= IPV6_MIN_MTU && mtu <= nif->mtu)
        info->link_mtu = mtu;
    struct nd6_router* r = nd6_router_find(info, &ip.src);
    if (lifetime) {
        if (!r) {
            r = malloc(sizeof(struct nd6_router));
            if (r) {
                memset(r, 0, sizeof(struct nd6_router));
                r->addr = ip.src;
                dlist_insert_tail(&info->routers, &r->link);
            }
        }
        if (r) {
            r->expires = now + nd6_ms((uint64_t)lifetime * 1000);
            r->managed = ra_flags & ND6_RA_MANAGED;
            r->other_config = ra_flags & ND6_RA_OTHER;
        }
    } else if (r) {
        nd6_router_remove(info, &ip.src);
    }
    spinlock_release_irqrestore(&nd6_lock, flags);

    if (have_sll)
        nd6_learn(nif, &ip.src, sll, true);
    for (uint8_t i = 0; i < n_pi; i++) {
        if (pi[i].len > 128 || ipv6_addr_is_linklocal(&pi[i].prefix) || pi[i].preferred > pi[i].valid)
            continue;
        if (pi[i].flags & ND6_PI_ONLINK)
            nd6_prefix_onlink(nif, &pi[i].prefix, pi[i].len, pi[i].valid);
        if (pi[i].flags & ND6_PI_AUTO)
            nd6_slaac_prefix_add(nif, &pi[i].prefix, pi[i].len, pi[i].valid, pi[i].preferred);
    }
    nd6_update_default_route(nif);

    if (ra_flags & ND6_RA_MANAGED)
        dhcp6_start(nif, true);
    else if (ra_flags & ND6_RA_OTHER)
        dhcp6_start(nif, false);
}

static void nd6_timer_netif(struct netif* nif, struct nd6_ifinfo* info, const uint64_t now, struct nd6_tx* tx,
                            size_t* n_tx, struct netbuf** doomed, bool* routers_changed)
{
    struct dlist_node* n = dlist_first(&info->neighbors);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        struct nd6_neighbor* e = dlist_entry(n, struct nd6_neighbor, link);
        bool drop = false;
        if (now >= e->expires) {
            switch (e->state) {
            case ND6_INCOMPLETE:
                if (e->probes >= ND6_MAX_MULTICAST_SOLICIT || *n_tx >= ND6_MAX_TX) {
                    drop = e->probes >= ND6_MAX_MULTICAST_SOLICIT;
                    break;
                }
                e->probes++;
                e->expires = now + nd6_ms(info->retrans_time_ms);
                tx[(*n_tx)++] = (struct nd6_tx){ .nif = nif, .target = e->addr };
                break;
            case ND6_REACHABLE:
                nd6_set_state(info, e, ND6_STALE);
                break;
            case ND6_STALE:
                drop = true;
                break;
            case ND6_DELAY:
                nd6_set_state(info, e, ND6_PROBE);
                [[fallthrough]];
            case ND6_PROBE:
                if (e->probes >= ND6_MAX_UNICAST_SOLICIT) {
                    drop = true;
                    break;
                }
                if (*n_tx >= ND6_MAX_TX)
                    break;
                e->probes++;
                e->expires = now + nd6_ms(info->retrans_time_ms);
                tx[(*n_tx)++] = (struct nd6_tx){ .nif = nif, .target = e->addr, .dst = e->addr };
                break;
            }
        }
        if (drop) {
            // TODO: ICMPv6 address unreachable for the packets that waited (RFC 4861 7.2.2)
            struct netbuf* p = nd6_take_pending(e);
            while (p) {
                struct netbuf* pn = p->next_pkt;
                p->next_pkt = *doomed;
                *doomed = p;
                p = pn;
            }
            if (e->is_router)
                nd6_router_remove(info, &e->addr);
            dlist_remove(&info->neighbors, n);
            free(e);
        }
        n = next;
    }

    n = dlist_first(&info->routers);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        struct nd6_router* r = dlist_entry(n, struct nd6_router, link);
        if (r->expires && r->expires <= now) {
            dlist_remove(&info->routers, n);
            free(r);
            *routers_changed = true;
        }
        n = next;
    }

    if (info->next_rs && now >= info->next_rs && *n_tx < ND6_MAX_TX) {
        tx[(*n_tx)++] = (struct nd6_tx){ .nif = nif, .rs = true };
        info->rs_sent++;
        info->next_rs = info->rs_sent < ND6_MAX_RTR_SOLICITATIONS ? now + nd6_ms(ND6_RTR_SOLICIT_INTERVAL_MS) : 0;
    }
}

static void nd6_timer_lifetimes(struct netif* nif, struct nd6_ifinfo* info, const uint64_t now)
{
    for (;;) {
        struct in6_addr prefix;
        uint8_t len = 0;
        uint64_t flags;
        spinlock_acquire_irqsave(&nd6_lock, &flags);
        for (struct dlist_node* n = dlist_first(&info->prefixes); n; n = dlist_next(n)) {
            struct nd6_prefix* p = dlist_entry(n, struct nd6_prefix, link);
            if (p->valid_until && p->valid_until <= now) {
                prefix = p->prefix;
                len = p->prefix_len;
                dlist_remove(&info->prefixes, n);
                free(p);
                break;
            }
        }
        spinlock_release_irqrestore(&nd6_lock, flags);
        if (!len)
            break;
        route_del6(&prefix, len);
    }

    struct dlist_node* n = dlist_first(&nif->ipv6_addrs);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        struct ipv6_ifaddr* ifa = dlist_entry(n, struct ipv6_ifaddr, link);
        if (ifa->valid_until && ifa->valid_until <= now) {
            const struct in6_addr addr = ifa->addr;
            ipv6_addr_del(nif, &addr);
        } else if (ifa->preferred_until && ifa->preferred_until <= now) {
            ifa->flags |= IPV6_ADDR_DEPRECATED;
        }
        n = next;
    }
}

void nd6_timer(void* arg)
{
    (void)arg;
    struct nd6_tx tx[ND6_MAX_TX];
    size_t n_tx = 0;
    struct netbuf* doomed = nullptr;
    const uint64_t now = ticks_get();

    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        struct nd6_ifinfo* info = nd6_info(nif);
        if (!info)
            continue;
        bool routers_changed = false;
        uint64_t flags;
        spinlock_acquire_irqsave(&nd6_lock, &flags);
        nd6_timer_netif(nif, info, now, tx, &n_tx, &doomed, &routers_changed);
        spinlock_release_irqrestore(&nd6_lock, flags);
        if (routers_changed)
            nd6_update_default_route(nif);
        nd6_timer_lifetimes(nif, info, now);
    }

    nd6_free_pending(doomed);
    for (size_t i = 0; i < n_tx; i++) {
        if (tx[i].rs) {
            nd6_rs_output(tx[i].nif);
            continue;
        }
        struct in6_addr src;
        nd6_solicit_source(tx[i].nif, nullptr, &tx[i].target, &src);
        const bool unicast = !ipv6_addr_is_unspecified(&tx[i].dst);
        nd6_ns_send(tx[i].nif, &tx[i].target, unicast ? &tx[i].dst : nullptr, &src);
    }
    callout_reset(&nd6_callout, nd6_ms(ND6_TIMER_MS), nd6_timer, nullptr);
}
