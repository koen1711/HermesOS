#include <drivers/net/ipv6/ipv6.h>

#include <drivers/net/core/netif.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netisr.h>
#include <drivers/net/core/route.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/random.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/icmp6.h>
#include <drivers/net/ipv6/mld6.h>
#include <drivers/net/ipv6/nd6.h>
#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#define IPV6_INPUT_QUEUE_LEN 256
#define IPV6_MAX_PAYLOAD     65535
#define IPV6_MAX_UNFRAG      256
#define IPV6_REASSEMBLE_TIMEOUT_S 60
#define IPV6_REASSEMBLE_MAX_QUEUES 16
#define IPV6_REASSEMBLE_MAX_FRAGS 64

#define IPV6_PARAM_HEADER  0
#define IPV6_PARAM_NEXTHDR 1
#define IPV6_PARAM_OPTION  2

const struct in6_addr in6addr_any = { 0 };
const struct in6_addr in6addr_loopback = { .s6_addr = { [15] = 1 } };

static ipv6_proto_input_t protocols[256];

struct ipv6_frag {
    struct ipv6_frag* next;
    struct netbuf* nb;
    uint32_t offset;
    uint32_t len;
};

struct ipv6_reassemble {
    struct dlist_node link;
    struct in6_addr src, dst;
    uint32_t id;
    struct ipv6_frag* frags;
    uint16_t n_frags;
    uint32_t total_len;
    uint8_t unfrag[IPV6_MAX_UNFRAG];
    uint16_t unfrag_len;
    uint16_t prev_nh;
    uint8_t next_header;
    struct netif* rcvif;
    uint32_t flags;
    uint64_t expires;
};

static struct dlist reassemble_queues;
static spinlock_t reassemble_lock;
static struct callout reassemble_callout;

static struct dlist all_addrs;
static spinlock_t addr_lock;

void ipv6_initialize()
{
    for (int i = 0; i < 256; i++)
        protocols[i] = nullptr;
    dlist_init(&reassemble_queues);
    spinlock_init(&reassemble_lock, "ipv6_reassemble");
    dlist_init(&all_addrs);
    spinlock_init(&addr_lock, "ipv6_addr");
    netisr_register(NETISR_IPV6, ipv6_input, IPV6_INPUT_QUEUE_LEN);
    callout_init(&reassemble_callout);
    callout_reset(&reassemble_callout, ticks_hz(), ipv6_reassemble_timer, nullptr);
}

int ipv6_register_protocol(const uint8_t next_header, const ipv6_proto_input_t input)
{
    if (!input)
        return -EINVAL;
    if (protocols[next_header])
        return -EEXIST;
    protocols[next_header] = input;
    return 0;
}

bool ipv6_addr_equal(const struct in6_addr* a, const struct in6_addr* b)
{
    for (int i = 0; i < 16; i++)
        if (a->s6_addr[i] != b->s6_addr[i])
            return false;
    return true;
}

bool ipv6_addr_is_unspecified(const struct in6_addr* addr)
{
    return ipv6_addr_equal(addr, &in6addr_any);
}

bool ipv6_addr_is_loopback(const struct in6_addr* addr)
{
    return ipv6_addr_equal(addr, &in6addr_loopback);
}

bool ipv6_addr_is_linklocal(const struct in6_addr* addr)
{
    return addr->s6_addr[0] == 0xFE && (addr->s6_addr[1] & 0xC0) == 0x80;   // fe80::/10
}

bool ipv6_addr_is_multicast(const struct in6_addr* addr)
{
    return addr->s6_addr[0] == 0xFF;
}

bool ipv6_prefix_match(const struct in6_addr* a, const struct in6_addr* b, const uint8_t prefix_len)
{
    const int bytes = prefix_len / 8;
    for (int i = 0; i < bytes; i++)
        if (a->s6_addr[i] != b->s6_addr[i])
            return false;
    const int bits = prefix_len % 8;
    if (!bits)
        return true;
    const uint8_t mask = (uint8_t)(0xFF << (8 - bits));
    return (a->s6_addr[bytes] & mask) == (b->s6_addr[bytes] & mask);
}

void ipv6_linklocal_from_hw(const uint8_t* hw_addr, struct in6_addr* out)
{
    memset(out, 0, sizeof(struct in6_addr));
    out->s6_addr[0] = 0xFE;
    out->s6_addr[1] = 0x80;
    out->s6_addr[8] = hw_addr[0] ^ 0x02;
    out->s6_addr[9] = hw_addr[1];
    out->s6_addr[10] = hw_addr[2];
    out->s6_addr[11] = 0xFF;
    out->s6_addr[12] = 0xFE;
    out->s6_addr[13] = hw_addr[3];
    out->s6_addr[14] = hw_addr[4];
    out->s6_addr[15] = hw_addr[5];
}

void ipv6_solicited_node(const struct in6_addr* addr, struct in6_addr* out)
{
    memset(out, 0, sizeof(struct in6_addr));
    out->s6_addr[0] = 0xFF;
    out->s6_addr[1] = 0x02;
    out->s6_addr[11] = 0x01;
    out->s6_addr[12] = 0xFF;
    out->s6_addr[13] = addr->s6_addr[13];
    out->s6_addr[14] = addr->s6_addr[14];
    out->s6_addr[15] = addr->s6_addr[15];
}

static int ipv6_scope(const struct in6_addr* addr)
{
    if (ipv6_addr_is_multicast(addr))
        return addr->s6_addr[1] & 0x0F;
    if (ipv6_addr_is_linklocal(addr) || ipv6_addr_is_loopback(addr))
        return 2;
    if (addr->s6_addr[0] == 0xFE && (addr->s6_addr[1] & 0xC0) == 0xC0)
        return 5;
    return 14;
}

static int ipv6_common_prefix(const struct in6_addr* a, const struct in6_addr* b)
{
    int n = 0;
    for (int i = 0; i < 16; i++) {
        const uint8_t x = a->s6_addr[i] ^ b->s6_addr[i];
        if (!x) {
            n += 8;
            continue;
        }
        for (uint8_t m = 0x80; m && !(x & m); m >>= 1)
            n++;
        break;
    }
    return n;
}

static bool ipv6_addr_usable(const struct ipv6_ifaddr* ifa)
{
    return !(ifa->flags & (IPV6_ADDR_TENTATIVE | IPV6_ADDR_DUPLICATED));
}

static bool ipv6_addr_wants_route(const struct ipv6_ifaddr* ifa)
{
    return !(ifa->flags & (IPV6_ADDR_AUTOCONF | IPV6_ADDR_LINKLOCAL)) && ifa->prefix_len < 128;
}

static struct ipv6_ifaddr* ipv6_addr_find_locked(const struct in6_addr* addr)
{
    for (struct dlist_node* n = dlist_first(&all_addrs); n; n = dlist_next(n)) {
        const auto ifa = dlist_entry(n, struct ipv6_ifaddr, all_link);
        if (ipv6_addr_equal(&ifa->addr, addr))
            return ifa;
    }
    return nullptr;
}

static void ipv6_addr_unlink_locked(struct ipv6_ifaddr* ifa)
{
    dlist_remove(&ifa->nif->ipv6_addrs, &ifa->link);
    dlist_remove(&all_addrs, &ifa->all_link);
}

int ipv6_addr_add(struct netif* nif, const struct in6_addr* addr, const uint8_t prefix_len,
                  const uint32_t flags, const uint64_t preferred_until, const uint64_t valid_until)
{
    if (prefix_len > 128 || ipv6_addr_is_multicast(addr) || ipv6_addr_is_unspecified(addr))
        return -EINVAL;

    struct ipv6_ifaddr* ifa = malloc(sizeof(struct ipv6_ifaddr));
    if (!ifa)
        return -ENOMEM;
    memset(ifa, 0, sizeof(struct ipv6_ifaddr));
    ifa->nif = nif;
    ifa->addr = *addr;
    ifa->prefix_len = prefix_len;
    ifa->flags = flags | (ipv6_addr_is_linklocal(addr) ? IPV6_ADDR_LINKLOCAL : 0);
    if (nif->flags & NETIF_LOOPBACK)
        ifa->flags &= ~IPV6_ADDR_TENTATIVE;
    ifa->preferred_until = preferred_until;
    ifa->valid_until = valid_until;
    const bool wants_route = ipv6_addr_wants_route(ifa);
    const bool tentative = ifa->flags & IPV6_ADDR_TENTATIVE;

    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    if (ipv6_addr_find_locked(addr)) {
        spinlock_release_irqrestore(&addr_lock, irq);
        free(ifa);
        return -EEXIST;
    }
    dlist_insert_tail(&nif->ipv6_addrs, &ifa->link);
    dlist_insert_tail(&all_addrs, &ifa->all_link);
    spinlock_release_irqrestore(&addr_lock, irq);

    if (wants_route) {
        const int rc = route_add6(addr, prefix_len, nullptr, nif, 0);
        if (rc < 0) {
            spinlock_acquire_irqsave(&addr_lock, &irq);
            ipv6_addr_unlink_locked(ifa);
            spinlock_release_irqrestore(&addr_lock, irq);
            free(ifa);
            return rc;
        }
    }

    if (nif->flags & NETIF_LOOPBACK)
        return 0;

    struct in6_addr group;
    ipv6_solicited_node(addr, &group);
    mld6_join(nif, &group);
    if (tentative)
        nd6_dad_start(nif, addr);
    return 0;
}

int ipv6_addr_del(struct netif* nif, const struct in6_addr* addr)
{
    if (ipv6_addr_flags(addr, nif) < 0)
        return -EADDRNOTAVAIL;
    nd6_dad_stop(addr);

    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    struct ipv6_ifaddr* ifa = ipv6_addr_find_locked(addr);
    if (!ifa || ifa->nif != nif) {
        spinlock_release_irqrestore(&addr_lock, irq);
        return -EADDRNOTAVAIL;
    }
    ipv6_addr_unlink_locked(ifa);
    spinlock_release_irqrestore(&addr_lock, irq);

    if (ipv6_addr_wants_route(ifa))
        route_del6(addr, ifa->prefix_len);
    if (!(nif->flags & NETIF_LOOPBACK)) {
        struct in6_addr group;
        ipv6_solicited_node(addr, &group);
        mld6_leave(nif, &group);
    }
    free(ifa);
    return 0;
}

int ipv6_addr_flags(const struct in6_addr* addr, const struct netif* nif)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    const struct ipv6_ifaddr* ifa = ipv6_addr_find_locked(addr);
    const int flags = ifa && (!nif || ifa->nif == nif) ? (int)ifa->flags : -EADDRNOTAVAIL;
    spinlock_release_irqrestore(&addr_lock, irq);
    return flags;
}

int ipv6_addr_update(const struct in6_addr* addr, const uint32_t set, const uint32_t clear)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    struct ipv6_ifaddr* ifa = ipv6_addr_find_locked(addr);
    int flags = -EADDRNOTAVAIL;
    if (ifa) {
        ifa->flags = (ifa->flags & ~clear) | set;
        flags = (int)ifa->flags;
    }
    spinlock_release_irqrestore(&addr_lock, irq);
    return flags;
}

int ipv6_addr_modify(const struct in6_addr* addr, int (*fn)(struct ipv6_ifaddr* ifa, void* arg), void* arg)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    struct ipv6_ifaddr* ifa = ipv6_addr_find_locked(addr);
    const int rc = ifa ? fn(ifa, arg) : -EADDRNOTAVAIL;
    spinlock_release_irqrestore(&addr_lock, irq);
    return rc;
}

static struct netif* ipv6_addr_netif(const struct in6_addr* addr)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    const struct ipv6_ifaddr* ifa = ipv6_addr_find_locked(addr);
    struct netif* nif = ifa ? ifa->nif : nullptr;
    if (nif)
        netif_ref(nif);
    spinlock_release_irqrestore(&addr_lock, irq);
    return nif;
}

bool ipv6_addr_linklocal(const struct netif* nif, struct in6_addr* out)
{
    bool found = false;
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    for (struct dlist_node* n = dlist_first(&nif->ipv6_addrs); n; n = dlist_next(n)) {
        const auto ifa = dlist_entry(n, struct ipv6_ifaddr, link);
        if (ifa->flags & IPV6_ADDR_LINKLOCAL) {
            found = ipv6_addr_usable(ifa);
            if (found)
                *out = ifa->addr;
            break;
        }
    }
    spinlock_release_irqrestore(&addr_lock, irq);
    return found;
}

#define IPV6_EXPIRE_BATCH 8

void ipv6_addr_expire(struct netif* nif, const uint64_t now)
{
    struct in6_addr expired[IPV6_EXPIRE_BATCH];
    size_t n_expired;
    do {
        n_expired = 0;
        uint64_t irq;
        spinlock_acquire_irqsave(&addr_lock, &irq);
        for (struct dlist_node* n = dlist_first(&nif->ipv6_addrs); n; n = dlist_next(n)) {
            const auto ifa = dlist_entry(n, struct ipv6_ifaddr, link);
            if (ifa->valid_until && ifa->valid_until <= now) {
                if (n_expired < IPV6_EXPIRE_BATCH)
                    expired[n_expired++] = ifa->addr;
            } else if (ifa->preferred_until && ifa->preferred_until <= now) {
                ifa->flags |= IPV6_ADDR_DEPRECATED;
            }
        }
        spinlock_release_irqrestore(&addr_lock, irq);
        for (size_t i = 0; i < n_expired; i++)
            ipv6_addr_del(nif, &expired[i]);
    } while (n_expired == IPV6_EXPIRE_BATCH);
}

bool ipv6_is_local(const struct in6_addr* addr)
{
    if (ipv6_addr_is_loopback(addr))
        return true;
    const int flags = ipv6_addr_flags(addr, nullptr);
    return flags >= 0 && !(flags & IPV6_ADDR_DUPLICATED);
}

static struct netif* ipv6_default_netif()
{
    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif))
        if (nif->nd6_info && (nif->flags & NETIF_UP))
            return nif;
    return nullptr;
}

static struct netif* ipv6_netif_for(const struct in6_addr* dst)
{
    if (ipv6_addr_is_multicast(dst) || ipv6_addr_is_linklocal(dst))
        return ipv6_default_netif();
    struct route_nexthop nh;
    return route_lookup6(dst, &nh) ? nh.nif : nullptr;
}

static bool ipv6_source_better(const struct ipv6_ifaddr* cand, const struct ipv6_ifaddr* best, const struct in6_addr* dst)
{
    if (!best)
        return true;
    if (ipv6_addr_equal(&cand->addr, dst))
        return true;
    if (ipv6_addr_equal(&best->addr, dst))
        return false;
    const int sd = ipv6_scope(dst), sc = ipv6_scope(&cand->addr), sb = ipv6_scope(&best->addr);
    if (sc != sb)
        return sc < sb ? sc >= sd : sb < sd;
    const bool dc = cand->flags & IPV6_ADDR_DEPRECATED, db = best->flags & IPV6_ADDR_DEPRECATED;
    if (dc != db)
        return db;
    return ipv6_common_prefix(&cand->addr, dst) > ipv6_common_prefix(&best->addr, dst);
}

int ipv6_select_source(struct netif* nif, const struct in6_addr* dst, struct in6_addr* out)
{
    if (ipv6_addr_is_loopback(dst)) {
        *out = in6addr_loopback;
        return 0;
    }
    struct netif* held = nif ? nullptr : ipv6_netif_for(dst);
    if (!nif)
        nif = held;
    if (!nif)
        return -ENETUNREACH;

    int result = 0;
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    const struct ipv6_ifaddr* best = nullptr;
    for (struct dlist_node* n = dlist_first(&nif->ipv6_addrs); n; n = dlist_next(n)) {
        const auto ifa = dlist_entry(n, struct ipv6_ifaddr, link);
        if (ipv6_addr_usable(ifa) && ipv6_source_better(ifa, best, dst))
            best = ifa;
    }
    if (!best || (ipv6_scope(&best->addr) < ipv6_scope(dst) && !ipv6_addr_is_multicast(dst)))
        result = -EADDRNOTAVAIL;
    else
        *out = best->addr;
    spinlock_release_irqrestore(&addr_lock, irq);
    if (held)
        netif_rele(held);
    return result;
}

static bool ipv6_is_ext_header(const uint8_t nh)
{
    return nh == IPV6_NH_HOPOPTS || nh == IPV6_NH_ROUTING || nh == IPV6_NH_DSTOPTS;
}

int ipv6_process_ext_headers(struct netbuf* nb, uint8_t* out_proto, size_t* out_offset)
{
    struct ipv6_header ip;
    if (nb->pkt_len < sizeof(ip))
        return -EINVAL;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    uint8_t nh = ip.next_header;
    size_t off = IPV6_HEADER_LEN;
    while (ipv6_is_ext_header(nh)) {
        uint8_t h[2];
        if (off + 2 > nb->pkt_len)
            return -EINVAL;
        netbuf_copydata(nb, off, 2, h);
        nh = h[0];
        off += (h[1] + 1) * 8;
    }
    if (off > nb->pkt_len)
        return -EINVAL;
    *out_proto = nh;
    *out_offset = off;
    return 0;
}

static bool ipv6_options_ok(struct netbuf* nb, const size_t off, const size_t len, const bool multicast)
{
    size_t i = 2;
    while (i < len) {
        uint8_t opt[2];
        netbuf_copydata(nb, off + i, 1, opt);
        if (opt[0] == 0) {
            i++;
            continue;
        }
        if (i + 2 > len)
            break;
        netbuf_copydata(nb, off + i, 2, opt);
        switch (opt[0]) {
        case 1:
        case 5:
            break;
        default:
            switch (opt[0] >> 6) {
            case 0:
                break;
            case 1:
                netbuf_free_chain(nb);
                return false;
            case 3:
                if (multicast) {
                    netbuf_free_chain(nb);
                    return false;
                }
                [[fallthrough]];
            default:
                icmp6_error(nb, ICMP6_PARAM_PROB, IPV6_PARAM_OPTION, (uint32_t)(off + i));
                return false;
            }
        }
        i += 2 + opt[1];
    }
    return true;
}

static bool ipv6_accept_dst(const struct netif* nif, const struct netbuf* nb, const struct in6_addr* dst)
{
    if (ipv6_addr_is_multicast(dst))
        return nif && mld6_is_member((struct netif*)nif, dst);
    const int flags = ipv6_addr_flags(dst, nullptr);
    if (flags >= 0)
        return !(flags & (IPV6_ADDR_TENTATIVE | IPV6_ADDR_DUPLICATED));
    return ipv6_addr_is_loopback(dst) && nb->rcvif && (nb->rcvif->flags & NETIF_LOOPBACK);
}

void ipv6_input(struct netbuf* nb)
{
again:
    nb = netbuf_pullup(nb, IPV6_HEADER_LEN);
    if (!nb)
        return;
    struct ipv6_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    if ((ntohl(ip.ver_tc_flow) >> 28) != IPV6_VERSION)
        goto drop;
    const size_t plen = ntohs(ip.payload_len);
    if (plen == 0 && ip.next_header == IPV6_NH_HOPOPTS)
        goto drop;
    if (nb->pkt_len < IPV6_HEADER_LEN + plen)
        goto drop;
    if (nb->pkt_len > IPV6_HEADER_LEN + plen)
        netbuf_adj(nb, -(ssize_t)(nb->pkt_len - IPV6_HEADER_LEN - plen));

    const bool from_loopback = nb->rcvif && (nb->rcvif->flags & NETIF_LOOPBACK);
    if (ipv6_addr_is_multicast(&ip.src) || (ipv6_addr_is_loopback(&ip.src) && !from_loopback))
        goto drop;
    if (!ipv6_accept_dst(nb->rcvif, nb, &ip.dst))
        goto drop;
    const bool multicast = ipv6_addr_is_multicast(&ip.dst);
    if (multicast)
        nb->flags |= NETBUF_MCAST;

    uint8_t nh = ip.next_header;
    size_t nh_field = 6;
    size_t off = IPV6_HEADER_LEN;
    for (;;) {
        if (nh == IPV6_NH_FRAGMENT) {
            nb = ipv6_reassemble(nb, off);
            if (!nb)
                return;
            goto again;
        }
        if (nh == IPV6_NH_NONE)
            goto drop;
        if (!ipv6_is_ext_header(nh))
            break;
        if (nh == IPV6_NH_HOPOPTS && off != IPV6_HEADER_LEN) {
            icmp6_error(nb, ICMP6_PARAM_PROB, IPV6_PARAM_NEXTHDR, (uint32_t)nh_field);
            return;
        }
        uint8_t h[4];
        if (off + 8 > nb->pkt_len)
            goto drop;
        netbuf_copydata(nb, off, 4, h);
        const size_t len = (h[1] + 1) * 8;
        if (off + len > nb->pkt_len)
            goto drop;
        if (nh == IPV6_NH_ROUTING) {
            if (h[3] != 0) {
                icmp6_error(nb, ICMP6_PARAM_PROB, IPV6_PARAM_HEADER, (uint32_t)(off + 2));
                return;
            }
        } else if (!ipv6_options_ok(nb, off, len, multicast)) {
            return;
        }
        nh_field = off;
        nh = h[0];
        off += len;
    }

    nb->l3_offset = 0;
    nb->l4_offset = (uint16_t)off;
    if (protocols[nh]) {
        protocols[nh](nb, off);
        return;
    }
    if (!multicast) {
        icmp6_error(nb, ICMP6_PARAM_PROB, IPV6_PARAM_NEXTHDR, (uint32_t)nh_field);
        return;
    }
drop:
    netbuf_free_chain(nb);
}

static void ipv6_reassemble_free(struct ipv6_reassemble* q)
{
    while (q->frags) {
        struct ipv6_frag* f = q->frags;
        q->frags = f->next;
        netbuf_free_chain(f->nb);
        free(f);
    }
    free(q);
}

static struct ipv6_reassemble* ipv6_reassemble_find(const struct ipv6_header* ip, const uint32_t id)
{
    for (struct dlist_node* n = dlist_first(&reassemble_queues); n; n = dlist_next(n)) {
        const auto q = dlist_entry(n, struct ipv6_reassemble, link);
        if (q->id == id && ipv6_addr_equal(&q->src, &ip->src) && ipv6_addr_equal(&q->dst, &ip->dst))
            return q;
    }
    return nullptr;
}

static int ipv6_find_prev_nh(const struct netbuf* nb, const size_t target)
{
    size_t field = 6;
    size_t off = IPV6_HEADER_LEN;
    while (off < target) {
        uint8_t h[2];
        netbuf_copydata(nb, off, 2, h);
        field = off;
        off += (h[1] + 1) * 8;
    }
    return off == target ? (int)field : -1;
}

static struct netbuf* ipv6_reassemble_complete(struct ipv6_reassemble* q)
{
    if (!q->unfrag_len || !q->total_len)
        return nullptr;
    uint32_t end = 0;
    for (const struct ipv6_frag* f = q->frags; f; f = f->next) {
        if (f->offset != end)
            return nullptr;
        end += f->len;
    }
    if (end != q->total_len)
        return nullptr;

    q->unfrag[q->prev_nh] = q->next_header;
    const auto ip = (struct ipv6_header*)q->unfrag;
    ip->payload_len = htons((uint16_t)(q->unfrag_len - IPV6_HEADER_LEN + q->total_len));
    struct netbuf* head = netbuf_alloc_packet(q->unfrag_len);
    if (!head || !netbuf_append(head, q->unfrag, q->unfrag_len)) {
        if (head)
            netbuf_free_chain(head);
        return nullptr;
    }
    for (struct ipv6_frag* f = q->frags; f;) {
        struct ipv6_frag* next = f->next;
        netbuf_cat(head, f->nb);
        free(f);
        f = next;
    }
    q->frags = nullptr;
    head->pkt_len = q->unfrag_len + q->total_len;
    head->rcvif = q->rcvif;
    head->flags |= q->flags & (NETBUF_MCAST | NETBUF_BCAST);
    return head;
}

struct netbuf* ipv6_reassemble(struct netbuf* nb, const size_t frag_offset)
{
    struct ipv6_header ip;
    struct ipv6_frag_header fh;
    if (frag_offset + sizeof(fh) > nb->pkt_len)
        goto drop;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    netbuf_copydata(nb, frag_offset, sizeof(fh), &fh);
    const uint16_t of = ntohs(fh.offset_flags);
    const uint32_t offset = of & 0xFFF8;
    const bool more = of & 1;
    const uint32_t len = nb->pkt_len - frag_offset - sizeof(fh);

    if (more && (len == 0 || len % 8)) {
        icmp6_error(nb, ICMP6_PARAM_PROB, IPV6_PARAM_HEADER, 4);
        return nullptr;
    }
    if (offset + len > IPV6_MAX_PAYLOAD) {
        icmp6_error(nb, ICMP6_PARAM_PROB, IPV6_PARAM_HEADER, (uint32_t)(frag_offset + 2));
        return nullptr;
    }
    if (frag_offset > IPV6_MAX_UNFRAG)
        goto drop;
    const int prev_nh = offset == 0 ? ipv6_find_prev_nh(nb, frag_offset) : 0;
    if (prev_nh < 0)
        goto drop;

    uint64_t irq;
    spinlock_acquire_irqsave(&reassemble_lock, &irq);
    struct ipv6_reassemble* q = ipv6_reassemble_find(&ip, fh.id);
    if (!q) {
        if (reassemble_queues.count >= IPV6_REASSEMBLE_MAX_QUEUES) {
            const auto old = dlist_entry(dlist_first(&reassemble_queues), struct ipv6_reassemble, link);
            dlist_remove(&reassemble_queues, &old->link);
            ipv6_reassemble_free(old);
        }
        q = malloc(sizeof(struct ipv6_reassemble));
        if (!q) {
            spinlock_release_irqrestore(&reassemble_lock, irq);
            goto drop;
        }
        memset(q, 0, sizeof(struct ipv6_reassemble));
        q->src = ip.src;
        q->dst = ip.dst;
        q->id = fh.id;
        q->expires = ticks_get() + ticks_from_ms(IPV6_REASSEMBLE_TIMEOUT_S * 1000);
        dlist_insert_tail(&reassemble_queues, &q->link);
    }

    bool bad = q->n_frags >= IPV6_REASSEMBLE_MAX_FRAGS;
    if (!more) {
        bad |= q->total_len && q->total_len != offset + len;
        q->total_len = offset + len;
    }
    struct ipv6_frag** pp = &q->frags;
    for (const struct ipv6_frag* f = q->frags; f && !bad; f = f->next) {
        if (offset < f->offset + f->len && f->offset < offset + len)
            bad = true;
        if (q->total_len && f->offset + f->len > q->total_len)
            bad = true;
    }
    if (q->total_len && offset + len > q->total_len)
        bad = true;
    struct ipv6_frag* frag = bad ? nullptr : malloc(sizeof(struct ipv6_frag));
    if (!frag) {
        dlist_remove(&reassemble_queues, &q->link);
        ipv6_reassemble_free(q);
        spinlock_release_irqrestore(&reassemble_lock, irq);
        goto drop;
    }

    if (offset == 0) {
        netbuf_copydata(nb, 0, frag_offset, q->unfrag);
        q->unfrag_len = (uint16_t)frag_offset;
        q->prev_nh = (uint16_t)prev_nh;
        q->next_header = fh.next_header;
        q->rcvif = nb->rcvif;
        q->flags = nb->flags;
    }
    netbuf_adj(nb, (ssize_t)(frag_offset + sizeof(fh)));
    frag->nb = nb;
    frag->offset = offset;
    frag->len = len;
    while (*pp && (*pp)->offset < offset)
        pp = &(*pp)->next;
    frag->next = *pp;
    *pp = frag;
    q->n_frags++;

    struct netbuf* done = ipv6_reassemble_complete(q);
    if (done || !q->frags) {
        dlist_remove(&reassemble_queues, &q->link);
        ipv6_reassemble_free(q);
    }
    spinlock_release_irqrestore(&reassemble_lock, irq);
    return done;

drop:
    netbuf_free_chain(nb);
    return nullptr;
}

// TODO: ICMPv6 time exceeded (code 1) when the first fragment had arrived; we no longer
// hold it intact once its headers are split off
void ipv6_reassemble_timer(void* arg)
{
    (void)arg;
    const uint64_t now = ticks_get();
    uint64_t irq;
    spinlock_acquire_irqsave(&reassemble_lock, &irq);
    struct dlist_node* n = dlist_first(&reassemble_queues);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        struct ipv6_reassemble* q = dlist_entry(n, struct ipv6_reassemble, link);
        if (q->expires <= now) {
            dlist_remove(&reassemble_queues, n);
            ipv6_reassemble_free(q);
        }
        n = next;
    }
    spinlock_release_irqrestore(&reassemble_lock, irq);
    callout_reset(&reassemble_callout, ticks_hz(), ipv6_reassemble_timer, nullptr);
}

static size_t ipv6_unfrag_len(const struct netbuf* nb, size_t* prev_nh)
{
    struct ipv6_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    uint8_t nh = ip.next_header;
    size_t off = IPV6_HEADER_LEN;
    *prev_nh = 6;
    while ((nh == IPV6_NH_HOPOPTS || nh == IPV6_NH_ROUTING) && off + 2 <= nb->pkt_len) {
        uint8_t h[2];
        netbuf_copydata(nb, off, 2, h);
        *prev_nh = off;
        nh = h[0];
        off += (h[1] + 1) * 8;
    }
    return off;
}

int ipv6_fragment(struct netbuf* nb, struct netif* nif, const uint32_t mtu, struct netbuf** out_list)
{
    (void)nif;
    *out_list = nullptr;
    size_t prev_nh;
    const size_t unfrag = ipv6_unfrag_len(nb, &prev_nh);
    const size_t chunk = mtu > unfrag + 8 ? (mtu - unfrag - 8) & ~(size_t)7 : 0;
    if (chunk == 0 || unfrag > IPV6_MAX_UNFRAG || mtu > NETBUF_CLUSTER_SIZE - NETBUF_HEADROOM) {
        netbuf_free_chain(nb);
        return -EMSGSIZE;
    }

    uint8_t hdr[IPV6_MAX_UNFRAG];
    netbuf_copydata(nb, 0, unfrag, hdr);
    const uint8_t next_header = hdr[prev_nh];
    hdr[prev_nh] = IPV6_NH_FRAGMENT;
    const uint32_t id = net_random32();
    const size_t payload = nb->pkt_len - unfrag;

    struct netbuf** tail = out_list;
    for (size_t off = 0; off < payload; off += chunk) {
        const size_t n = payload - off < chunk ? payload - off : chunk;
        const bool more = off + n < payload;
        struct netbuf* f = netbuf_alloc_cluster(true);
        if (!f) {
            while (*out_list) {
                struct netbuf* next = (*out_list)->next_pkt;
                netbuf_free_chain(*out_list);
                *out_list = next;
            }
            netbuf_free_chain(nb);
            return -ENOBUFS;
        }
        const auto ip = (struct ipv6_header*)hdr;
        ip->payload_len = htons((uint16_t)(unfrag - IPV6_HEADER_LEN + 8 + n));
        memcpy(f->data, hdr, unfrag);
        const struct ipv6_frag_header fh = {
            .next_header = next_header,
            .offset_flags = htons((uint16_t)(off | (more ? 1 : 0))),
            .id = id,
        };
        memcpy(f->data + unfrag, &fh, sizeof(fh));
        netbuf_copydata(nb, unfrag + off, n, f->data + unfrag + sizeof(fh));
        f->len = f->pkt_len = unfrag + sizeof(fh) + n;
        f->l3_offset = 0;
        f->l4_offset = (uint16_t)(unfrag + sizeof(fh));
        *tail = f;
        tail = &f->next_pkt;
    }
    netbuf_free_chain(nb);
    return 0;
}

static int ipv6_output_via(struct netbuf* nb, const struct in6_addr* src, const struct in6_addr* dst,
                           const uint8_t next_header, const struct ipv6_options* opts, const uint32_t flags,
                           struct netif* nif, const struct in6_addr* next_hop, const uint32_t route_mtu,
                           const bool local)
{
    if (!(nif->flags & NETIF_UP)) {
        netbuf_free_chain(nb);
        return -ENETDOWN;
    }

    struct in6_addr source = src ? *src : in6addr_any;
    if (ipv6_addr_is_unspecified(&source) && !(flags & IPV6_OUTPUT_UNSPEC_SRC)) {
        if (ipv6_select_source(nif, dst, &source) < 0) {
            netbuf_free_chain(nb);
            return -EADDRNOTAVAIL;
        }
    }
    if (nb->pkt_len > IPV6_MAX_PAYLOAD) {
        netbuf_free_chain(nb);
        return -EMSGSIZE;
    }

    const struct nd6_ifinfo* nd = nif->nd6_info;
    uint8_t hop_limit = opts && opts->hop_limit ? opts->hop_limit : 0;
    if (!hop_limit)
        hop_limit = nd && nd->cur_hop_limit ? nd->cur_hop_limit : IPV6_DEFAULT_HLIM;
    const uint32_t tc = opts ? opts->traffic_class : 0;
    const uint32_t flow = opts ? opts->flow_label & 0xFFFFF : 0;
    const struct ipv6_header hdr = {
        .ver_tc_flow = htonl((uint32_t)IPV6_VERSION << 28 | tc << 20 | flow),
        .payload_len = htons((uint16_t)nb->pkt_len),
        .next_header = next_header,
        .hop_limit = hop_limit,
        .src = source,
        .dst = *dst,
    };
    nb = netbuf_prepend(nb, IPV6_HEADER_LEN);
    if (!nb)
        return -ENOBUFS;
    memcpy(nb->data, &hdr, IPV6_HEADER_LEN);
    nb->l3_offset = 0;
    nb->l4_offset = IPV6_HEADER_LEN;

    if (local) {
        nb->rcvif = nif;
        return netisr_queue(NETISR_IPV6, nb);
    }

    uint32_t mtu = nd && nd->link_mtu ? nd->link_mtu : nif->mtu;
    if (route_mtu && route_mtu < mtu)
        mtu = route_mtu;
    if (nb->pkt_len <= mtu)
        return nif->output(nif, nb, next_hop, AF_INET6);
    if (flags & IPV6_OUTPUT_DONTFRAG) {
        netbuf_free_chain(nb);
        return -EMSGSIZE;
    }
    struct netbuf* frags;
    const int rc = ipv6_fragment(nb, nif, mtu, &frags);
    if (rc < 0)
        return rc;
    int result = 0;
    while (frags) {
        struct netbuf* next = frags->next_pkt;
        frags->next_pkt = nullptr;
        const int r = nif->output(nif, frags, next_hop, AF_INET6);
        if (r < 0 && result == 0)
            result = r;
        frags = next;
    }
    return result;
}

int ipv6_output(struct netbuf* nb, const struct in6_addr* src, const struct in6_addr* dst, const uint8_t next_header,
                const struct ipv6_options* opts, const uint32_t flags)
{
    const bool scoped = ipv6_addr_is_multicast(dst) || ipv6_addr_is_linklocal(dst);
    const bool local = !ipv6_addr_is_multicast(dst) && ipv6_is_local(dst);
    struct netif* nif = opts ? opts->nif : nullptr;
    struct in6_addr next_hop = *dst;
    uint32_t route_mtu = 0;

    if (nif) {
        netif_ref(nif);
    } else if (scoped || local) {
        if (src && !ipv6_addr_is_unspecified(src))
            nif = ipv6_addr_netif(src);
        if (!nif && local)
            nif = ipv6_addr_netif(dst);
        if (!nif)
            nif = ipv6_default_netif();
    }
    if (!scoped && !local) {
        struct route_nexthop nh;
        if (nif)
            netif_rele(nif);
        if (!route_lookup6(dst, &nh)) {
            netbuf_free_chain(nb);
            return -ENETUNREACH;
        }
        nif = nh.nif;
        if (!ipv6_addr_is_unspecified(&nh.gw6))
            next_hop = nh.gw6;
        route_mtu = nh.mtu;
    }
    if (!nif) {
        netbuf_free_chain(nb);
        return -ENETUNREACH;
    }
    const int rc = ipv6_output_via(nb, src, dst, next_header, opts, flags, nif, &next_hop, route_mtu, local);
    netif_rele(nif);
    return rc;
}

void ipv6_ifattach(struct netif* nif)
{
    if (nif->flags & NETIF_LOOPBACK) {
        ipv6_addr_add(nif, &in6addr_loopback, 128, 0, 0, 0);
        return;
    }
    nd6_ifattach(nif);
    static const struct in6_addr all_nodes = { .s6_addr = { 0xFF, 0x02, [15] = 1 } };
    mld6_join(nif, &all_nodes);
    struct in6_addr ll;
    ipv6_linklocal_from_hw(nif->hw_addr, &ll);
    ipv6_addr_add(nif, &ll, 64, IPV6_ADDR_TENTATIVE | IPV6_ADDR_LINKLOCAL, 0, 0);
}

void ipv6_ifdetach(struct netif* nif)
{
    for (;;) {
        uint64_t irq;
        spinlock_acquire_irqsave(&addr_lock, &irq);
        struct dlist_node* first = dlist_first(&nif->ipv6_addrs);
        struct in6_addr addr;
        if (first)
            addr = dlist_entry(first, struct ipv6_ifaddr, link)->addr;
        spinlock_release_irqrestore(&addr_lock, irq);
        if (!first || ipv6_addr_del(nif, &addr) < 0)
            break;
    }
    struct dlist_node* n;
    nd6_ifdetach(nif);
    mld6_ifdetach(nif);

    uint64_t irq;
    spinlock_acquire_irqsave(&reassemble_lock, &irq);
    n = dlist_first(&reassemble_queues);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        struct ipv6_reassemble* q = dlist_entry(n, struct ipv6_reassemble, link);
        if (q->rcvif == nif) {
            dlist_remove(&reassemble_queues, n);
            ipv6_reassemble_free(q);
        }
        n = next;
    }
    spinlock_release_irqrestore(&reassemble_lock, irq);
}

static int ipv6_hex_digit(const char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int ipv6_pton(const char* str, struct in6_addr* out)
{
    uint16_t words[8] = { 0 };
    int n = 0, gap = -1;
    const char* p = str;
    if (p[0] == ':') {
        if (p[1] != ':')
            return -EINVAL;
        p++;
    }
    while (*p) {
        if (*p == ':') {
            if (gap >= 0)
                return -EINVAL;
            gap = n;
            p++;
            if (!*p)
                break;
            continue;
        }

        const char* q = p;
        while (*q && *q != ':' && *q != '.')
            q++;
        if (*q == '.') {
            struct in_addr v4;
            if (n > 6 || ipv4_pton(p, &v4) < 0)
                return -EINVAL;
            const uint8_t* b = (const uint8_t*)&v4;
            words[n++] = (uint16_t)(b[0] << 8 | b[1]);
            words[n++] = (uint16_t)(b[2] << 8 | b[3]);
            break;
        }
        uint32_t w = 0;
        int digits = 0;
        int d;
        while ((d = ipv6_hex_digit(*p)) >= 0) {
            w = w << 4 | (uint32_t)d;
            p++;
            if (++digits > 4)
                return -EINVAL;
        }
        if (!digits || n >= 8)
            return -EINVAL;
        words[n++] = (uint16_t)w;
        if (*p == ':') {
            p++;
            if (!*p)
                return -EINVAL;
        } else if (*p) {
            return -EINVAL;
        }
    }
    if (gap >= 0) {
        if (n == 8)
            return -EINVAL;
        const int tail = n - gap;
        for (int i = 0; i < tail; i++)
            words[7 - i] = words[n - 1 - i];
        for (int i = gap; i < 8 - tail; i++)
            words[i] = 0;
    } else if (n != 8) {
        return -EINVAL;
    }
    for (int i = 0; i < 8; i++) {
        out->s6_addr[2 * i] = (uint8_t)(words[i] >> 8);
        out->s6_addr[2 * i + 1] = (uint8_t)words[i];
    }
    return 0;
}

void ipv6_ntop(const struct in6_addr* addr, char* out)
{
    static constexpr char hex[] = "0123456789abcdef";
    uint16_t w[8];
    for (int i = 0; i < 8; i++)
        w[i] = (uint16_t)(addr->s6_addr[2 * i] << 8 | addr->s6_addr[2 * i + 1]);

    int best = -1, best_len = 1;
    for (int i = 0; i < 8;) {
        if (w[i]) {
            i++;
            continue;
        }
        int j = i;
        while (j < 8 && !w[j])
            j++;
        if (j - i > best_len) {
            best = i;
            best_len = j - i;
        }
        i = j;
    }

    char* o = out;
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            *o++ = ':';
            if (i == 0)
                *o++ = ':';
            i += best_len - 1;
            continue;
        }
        bool started = false;
        for (int s = 12; s >= 0; s -= 4) {
            const int d = (w[i] >> s) & 0xF;
            if (d || started || s == 0) {
                *o++ = hex[d];
                started = true;
            }
        }
        if (i < 7)
            *o++ = ':';
    }
    *o = '\0';
}
