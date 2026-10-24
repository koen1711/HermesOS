#include <drivers/net/ipv4/ipv4.h>

#include <drivers/net/core/netif.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netisr.h>
#include <drivers/net/core/route.h>
#include <drivers/net/core/checksum.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#include "icmp.h"

#define IPV4_INPUT_QUEUE_LEN 256
#define IPV4_MAX_DATAGRAM    65535
#define IPV4_REASSEMBLE_TIMEOUT_S 60
#define IPV4_REASSEMBLE_MAX_QUEUES 16
#define IPV4_REASSEMBLE_MAX_FRAGS 64

static ipv4_proto_input_t protocols[256];
static uint16_t next_id;

struct ipv4_frag {
    struct ipv4_frag* next;
    struct netbuf* nb;
    uint16_t offset;
    uint16_t len;
};

struct ipv4_reassemble {
    struct dlist_node link;
    struct in_addr src, dst;
    uint16_t id;
    uint8_t protocol;
    struct ipv4_frag* frags;
    uint16_t n_frags;
    uint32_t total_len;
    uint64_t expires;
};

static struct dlist reassemble_queues;
static spinlock_t reassemble_lock;
static struct callout reassemble_callout;

static struct dlist all_addrs;
static spinlock_t addr_lock;

void ipv4_initialize()
{
    for (int i = 0; i < 256; i++)
        protocols[i] = nullptr;
    next_id = (uint16_t)ticks_get();
    dlist_init(&reassemble_queues);
    spinlock_init(&reassemble_lock, "ipv4_reass");
    dlist_init(&all_addrs);
    spinlock_init(&addr_lock, "ipv4_addr");
    netisr_register(NETISR_IP, ipv4_input, IPV4_INPUT_QUEUE_LEN);
    callout_init(&reassemble_callout);
    callout_reset(&reassemble_callout, ticks_hz(), ipv4_reassemble_timer, nullptr);
}

int ipv4_register_protocol(const uint8_t protocol, const ipv4_proto_input_t input)
{
    if (!input)
        return -EINVAL;
    if (protocols[protocol])
        return -EEXIST;
    protocols[protocol] = input;
    return 0;
}

static struct in_addr ipv4_prefix_to_mask(const uint8_t prefix_len)
{
    const uint32_t mask = prefix_len ? 0xFFFFFFFFu << (32 - prefix_len) : 0;
    return (struct in_addr){ htonl(mask) };
}

static uint16_t ipv4_header_checksum(const void* header, const size_t len)
{
    return htons(inet_checksum(header, len));
}

static struct ipv4_ifaddr* ipv4_addr_find_locked(const struct in_addr addr)
{
    for (struct dlist_node* n = dlist_first(&all_addrs); n; n = dlist_next(n)) {
        const auto ia = dlist_entry(n, struct ipv4_ifaddr, all_link);
        if (ia->addr.s_addr == addr.s_addr)
            return ia;
    }
    return nullptr;
}

static void ipv4_addr_unlink_locked(struct ipv4_ifaddr* ia)
{
    dlist_remove(&ia->nif->ipv4_addrs, &ia->link);
    dlist_remove(&all_addrs, &ia->all_link);
}

int ipv4_addr_add(struct netif* nif, const struct in_addr addr, const uint8_t prefix_len, const uint64_t valid_until)
{
    if (prefix_len > 32)
        return -EINVAL;

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

    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    if (ipv4_addr_find_locked(addr)) {
        spinlock_release_irqrestore(&addr_lock, irq);
        free(ia);
        return -EEXIST;
    }
    dlist_insert_tail(&nif->ipv4_addrs, &ia->link);
    dlist_insert_tail(&all_addrs, &ia->all_link);
    spinlock_release_irqrestore(&addr_lock, irq);

    const int rc = route_add4(addr, prefix_len, (struct in_addr){ INADDR_ANY }, nif, 0);
    if (rc < 0) {
        spinlock_acquire_irqsave(&addr_lock, &irq);
        ipv4_addr_unlink_locked(ia);
        spinlock_release_irqrestore(&addr_lock, irq);
        free(ia);
    }
    return rc;
}

int ipv4_addr_del(struct netif* nif, const struct in_addr addr)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    struct ipv4_ifaddr* ia = ipv4_addr_find_locked(addr);
    if (!ia || ia->nif != nif) {
        spinlock_release_irqrestore(&addr_lock, irq);
        return -EADDRNOTAVAIL;
    }
    ipv4_addr_unlink_locked(ia);
    spinlock_release_irqrestore(&addr_lock, irq);

    route_del4(addr, ia->prefix_len);
    free(ia);
    return 0;
}

struct in_addr ipv4_addr_primary(const struct netif* nif)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    struct dlist_node* n = dlist_first(&nif->ipv4_addrs);
    const struct in_addr addr = n ? dlist_entry(n, struct ipv4_ifaddr, link)->addr : (struct in_addr){ INADDR_ANY };
    spinlock_release_irqrestore(&addr_lock, irq);
    return addr;
}

bool ipv4_addr_is_on(const struct netif* nif, const struct in_addr addr)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    const struct ipv4_ifaddr* ia = ipv4_addr_find_locked(addr);
    const bool on = ia && ia->nif == nif;
    spinlock_release_irqrestore(&addr_lock, irq);
    return on;
}

int ipv4_addr_set_expiry(const struct in_addr addr, const uint64_t valid_until)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    struct ipv4_ifaddr* ia = ipv4_addr_find_locked(addr);
    if (ia)
        ia->valid_until = valid_until;
    spinlock_release_irqrestore(&addr_lock, irq);
    return ia ? 0 : -EADDRNOTAVAIL;
}

static struct in_addr ipv4_select_source_on(const struct netif* nif, const struct in_addr dst)
{
    struct in_addr src = { INADDR_ANY };
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    for (struct dlist_node* n = dlist_first(&nif->ipv4_addrs); n; n = dlist_next(n)) {
        const auto ia = dlist_entry(n, struct ipv4_ifaddr, link);
        if (src.s_addr == INADDR_ANY)
            src = ia->addr;
        if ((ia->addr.s_addr & ia->netmask.s_addr) == (dst.s_addr & ia->netmask.s_addr)) {
            src = ia->addr;
            break;
        }
    }
    spinlock_release_irqrestore(&addr_lock, irq);
    return src;
}

struct in_addr ipv4_select_source(struct netif* nif, const struct in_addr dst)
{
    if (nif)
        return ipv4_select_source_on(nif, dst);
    struct route_nexthop nh;
    if (!route_lookup4(dst, &nh))
        return (struct in_addr){ INADDR_ANY };
    const struct in_addr src = ipv4_select_source_on(nh.nif, dst);
    netif_rele(nh.nif);
    return src;
}

bool ipv4_is_local(const struct in_addr addr)
{
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    const bool local = ipv4_addr_find_locked(addr) != nullptr;
    spinlock_release_irqrestore(&addr_lock, irq);
    return local;
}

bool ipv4_is_broadcast(const struct netif* nif, const struct in_addr addr)
{
    if (addr.s_addr == INADDR_BROADCAST)
        return true;
    bool bcast = false;
    uint64_t irq;
    spinlock_acquire_irqsave(&addr_lock, &irq);
    for (struct dlist_node* n = dlist_first(&nif->ipv4_addrs); n && !bcast; n = dlist_next(n)) {
        const auto ia = dlist_entry(n, struct ipv4_ifaddr, link);
        bcast = ia->prefix_len < 31 && addr.s_addr == ia->broadcast.s_addr;
    }
    spinlock_release_irqrestore(&addr_lock, irq);
    return bcast;
}

bool ipv4_is_multicast(const struct in_addr addr)
{
    return (ntohl(addr.s_addr) & 0xF0000000u) == 0xE0000000u; // 224.0.0.0/4
}

static void ipv4_reassemble_free(struct ipv4_reassemble* q)
{
    while (q->frags) {
        struct ipv4_frag* f = q->frags;
        q->frags = f->next;
        netbuf_free_chain(f->nb);
        free(f);
    }

    free(q);
}

static struct ipv4_reassemble* ipv4_reassemble_find(const struct ipv4_header* ip)
{
    for (struct dlist_node* n = dlist_first(&reassemble_queues); n; n = dlist_next(n)) {
        const auto q = dlist_entry(n, struct ipv4_reassemble, link);
        if (q->id == ip->id && q->protocol == ip->protocol
            && q->src.s_addr == ip->src.s_addr && q->dst.s_addr == ip->dst.s_addr)
            return q;
    }
    return nullptr;
}

static struct netbuf* ipv4_reassemble_complete(struct ipv4_reassemble* q)
{
    if (!q->total_len || !q->frags || q->frags->offset != 0)
        return nullptr;
    uint32_t end = 0;
    for (const struct ipv4_frag* f = q->frags; f; f = f->next) {
        if (f->offset != end)
            return nullptr;
        end += f->len;
    }
    if (end != q->total_len)
        return nullptr;

    if (q->frags->nb->pkt_len - q->frags->len + end > IPV4_MAX_DATAGRAM)
        return nullptr;

    struct netbuf* head = q->frags->nb;
    for (const struct ipv4_frag* f = q->frags->next; f; f = f->next)
        netbuf_cat(head, f->nb);
    size_t total = 0;
    for (const struct netbuf* b = head; b; b = b->next)
        total += b->len;
    head->pkt_len = total;

    uint8_t hdr[IPV4_MAX_HEADER_LEN];
    const auto ip = (struct ipv4_header*)hdr;
    netbuf_copydata(head, 0, sizeof(*ip), ip);
    const size_t header_len = (ip->ver_ihl & 0x0f) * 4;
    netbuf_copydata(head, 0, header_len, hdr);
    ip->total_len = htons((uint16_t)total);
    ip->frag_off = 0;
    ip->checksum = 0;
    ip->checksum = ipv4_header_checksum(hdr, header_len);
    netbuf_copyback(head, 0, header_len, hdr);

    for (struct ipv4_frag* f = q->frags; f;) {
        struct ipv4_frag* next = f->next;
        free(f);
        f = next;
    }
    q->frags = nullptr;
    return head;
}

struct netbuf* ipv4_reassemble(struct netbuf* nb)
{
    struct ipv4_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    const size_t header_len = (ip.ver_ihl & 0x0f) * 4;
    const uint16_t frag = ntohs(ip.frag_off);
    const uint32_t offset = (uint32_t)(frag & IPV4_OFFMASK) * 8;
    const bool more = frag & IPV4_MF;
    uint32_t len = nb->pkt_len - header_len;

    if ((more && (len % 8 || len == 0)) || offset + len > IPV4_MAX_DATAGRAM - IPV4_MAX_HEADER_LEN) {
        netbuf_free_chain(nb);
        return nullptr;
    }

    uint64_t irq;
    spinlock_acquire_irqsave(&reassemble_lock, &irq);
    struct ipv4_reassemble* q = ipv4_reassemble_find(&ip);
    if (!q) {
        if (reassemble_queues.count >= IPV4_REASSEMBLE_MAX_QUEUES) {
            const auto old = dlist_entry(dlist_first(&reassemble_queues), struct ipv4_reassemble, link);
            dlist_remove(&reassemble_queues, &old->link);
            ipv4_reassemble_free(old);
        }

        q = malloc(sizeof(struct ipv4_reassemble));
        if (!q) {
            spinlock_release_irqrestore(&reassemble_lock, irq);
            netbuf_free_chain(nb);
            return nullptr;
        }
        memset(q, 0, sizeof(struct ipv4_reassemble));
        q->src = ip.src;
        q->dst = ip.dst;
        q->id = ip.id;
        q->protocol = ip.protocol;
        q->expires = ticks_get() + (uint64_t)IPV4_REASSEMBLE_TIMEOUT_S * ticks_hz();
        dlist_insert_tail(&reassemble_queues, &q->link);
    }

    if (!more) {
        if (q->total_len && q->total_len != offset + len)
            goto drop_queue;
        q->total_len = offset + len;
    }
    if (q->n_frags >= IPV4_REASSEMBLE_MAX_FRAGS)
        goto drop_queue;

    if (offset != 0)
        netbuf_adj(nb, (ssize_t)header_len);

    struct ipv4_frag** link = &q->frags;
    const struct ipv4_frag* prev = nullptr;
    while (*link && (*link)->offset < offset) {
        prev = *link;
        link = &(*link)->next;
    }
    uint32_t start = offset;
    if (prev && prev->offset + prev->len > start) {
        const uint32_t overlap = prev->offset + prev->len - start;
        if (overlap >= len || start == 0)
            goto drop_frag;
        netbuf_adj(nb, overlap);
        start += overlap;
        len -= overlap;
    }
    if (*link && (*link)->offset < start + len) {
        const uint32_t overlap = start + len - (*link)->offset;
        if (overlap >= len || (*link)->offset == start)
            goto drop_frag;
        netbuf_adj(nb, -(ssize_t)overlap);
        len -= overlap;
    }

    struct ipv4_frag* f = malloc(sizeof(struct ipv4_frag));
    if (!f)
        goto drop_frag;
    f->nb = nb;
    f->offset = (uint16_t)start;
    f->len = (uint16_t)len;
    f->next = *link;
    *link = f;
    q->n_frags++;

    struct netbuf* whole = ipv4_reassemble_complete(q);
    if (whole) {
        dlist_remove(&reassemble_queues, &q->link);
        ipv4_reassemble_free(q);
    }
    spinlock_release_irqrestore(&reassemble_lock, irq);
    return whole;

drop_frag:
    spinlock_release_irqrestore(&reassemble_lock, irq);
    netbuf_free_chain(nb);
    return nullptr;

drop_queue:
    dlist_remove(&reassemble_queues, &q->link);
    ipv4_reassemble_free(q);
    spinlock_release_irqrestore(&reassemble_lock, irq);
    netbuf_free_chain(nb);
    return nullptr;
}

static bool ipv4_reassemble_from(const struct ipv4_reassemble* q, const struct netif* nif)
{
    for (const struct ipv4_frag* f = q->frags; f; f = f->next)
        if (f->nb->rcvif == nif)
            return true;
    return false;
}

void ipv4_ifdetach(struct netif* nif)
{
    for (;;) {
        const struct in_addr addr = ipv4_addr_primary(nif);
        if (addr.s_addr == INADDR_ANY || ipv4_addr_del(nif, addr) < 0)
            break;
    }

    uint64_t irq;
    spinlock_acquire_irqsave(&reassemble_lock, &irq);
    struct dlist_node* n;
    n = dlist_first(&reassemble_queues);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        const auto q = dlist_entry(n, struct ipv4_reassemble, link);
        if (ipv4_reassemble_from(q, nif)) {
            dlist_remove(&reassemble_queues, n);
            ipv4_reassemble_free(q);
        }
        n = next;
    }
    spinlock_release_irqrestore(&reassemble_lock, irq);
}

void ipv4_reassemble_timer(void* arg)
{
    (void)arg;
    const uint64_t now = ticks_get();
    uint64_t irq;
    spinlock_acquire_irqsave(&reassemble_lock, &irq);
    struct dlist_node* n = dlist_first(&reassemble_queues);
    while (n) {
        struct dlist_node* next = dlist_next(n);
        const auto q = dlist_entry(n, struct ipv4_reassemble, link);
        if (q->expires <= now) {
            dlist_remove(&reassemble_queues, n);
            ipv4_reassemble_free(q);
        }
        n = next;
    }
    spinlock_release_irqrestore(&reassemble_lock, irq);
    callout_reset(&reassemble_callout, ticks_hz(), ipv4_reassemble_timer, nullptr);
}

void ipv4_input(struct netbuf* nb)
{
    struct netif* nif = nb->rcvif;
    if (nb->pkt_len < IPV4_HEADER_LEN)
        goto drop;
    nb = netbuf_pullup(nb, IPV4_HEADER_LEN);
    if (!nb)
        return;

    struct ipv4_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    const size_t header_len = (ip.ver_ihl & 0x0f) * 4;
    if ((ip.ver_ihl >> 4) != IPV4_VERSION || header_len < IPV4_HEADER_LEN || header_len > nb->pkt_len)
        goto drop;
    nb = netbuf_pullup(nb, header_len);
    if (!nb)
        return;
    if (inet_checksum(nb->data, header_len) != 0)
        goto drop;

    const size_t total = ntohs(ip.total_len);
    if (total < header_len || total > nb->pkt_len)
        goto drop;
    if (nb->pkt_len > total)
        netbuf_adj(nb, -(ssize_t)(nb->pkt_len - total));

    if (ipv4_is_multicast(ip.src) || ip.src.s_addr == INADDR_BROADCAST)
        goto drop;

    const bool bcast = nif ? ipv4_is_broadcast(nif, ip.dst) : ip.dst.s_addr == INADDR_BROADCAST;
    if (!ipv4_is_local(ip.dst) && !bcast && !ipv4_is_multicast(ip.dst))
        goto drop;
    if (bcast)
        nb->flags |= NETBUF_BCAST;
    else if (ipv4_is_multicast(ip.dst))
        nb->flags |= NETBUF_MCAST;

    if (ntohs(ip.frag_off) & (IPV4_MF | IPV4_OFFMASK)) {
        nb = ipv4_reassemble(nb);
        if (!nb)
            return;
        nb->rcvif = nif;

        nb = netbuf_pullup(nb, IPV4_HEADER_LEN);
        if (!nb)
            return;
        netbuf_copydata(nb, 0, sizeof(ip), &ip);
    }
    const size_t dhlen = (ip.ver_ihl & 0x0f) * 4;
    if (dhlen < IPV4_HEADER_LEN || dhlen > nb->pkt_len || ntohs(ip.total_len) != nb->pkt_len)
        goto drop;
    nb = netbuf_pullup(nb, dhlen);
    if (!nb)
        return;

    nb->l3_offset = 0;
    nb->l4_offset = (uint16_t)dhlen;
    const ipv4_proto_input_t input = protocols[ip.protocol];
    if (input) {
        input(nb, dhlen);
        return;
    }

    icmp_error(nb, ICMP_DEST_UNREACH, ICMP_UNREACH_PROTOCOL, 0);
    return;

drop:
    netbuf_free_chain(nb);
}

static size_t ipv4_copied_options(const uint8_t* opts, const size_t len, uint8_t* out)
{
    size_t i = 0, n = 0;
    while (i < len) {
        const uint8_t type = opts[i];
        if (type == 0)
            break;
        if (type == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || opts[i + 1] < 2 || i + opts[i + 1] > len)
            break;
        const uint8_t olen = opts[i + 1];
        if (type & 0x80) {
            memcpy(out + n, opts + i, olen);
            n += olen;
        }
        i += olen;
    }
    while (n % 4)
        out[n++] = 0;
    return n;
}

int ipv4_fragment(struct netbuf* nb, struct netif* nif, const uint32_t mtu, struct netbuf** out_list)
{
    (void)nif;
    *out_list = nullptr;

    uint8_t hdr[IPV4_MAX_HEADER_LEN];
    netbuf_copydata(nb, 0, IPV4_HEADER_LEN, hdr);
    const auto ip = (const struct ipv4_header*)hdr;
    const size_t header_len = (ip->ver_ihl & 0x0f) * 4;
    netbuf_copydata(nb, 0, header_len, hdr);

    uint8_t later_hdr[IPV4_MAX_HEADER_LEN];
    memcpy(later_hdr, hdr, IPV4_HEADER_LEN);
    const size_t later_hlen = IPV4_HEADER_LEN
        + ipv4_copied_options(hdr + IPV4_HEADER_LEN, header_len - IPV4_HEADER_LEN, later_hdr + IPV4_HEADER_LEN);

    const size_t data_len = nb->pkt_len - header_len;
    const uint16_t orig_frag = ntohs(ip->frag_off);
    struct netbuf** tail = out_list;
    size_t off = 0;

    while (off < data_len) {
        const bool first = off == 0;
        const size_t frag_header_len = first ? header_len : later_hlen;
        if (mtu < frag_header_len + 8)
            goto fail;
        size_t chunk = (mtu - frag_header_len) & ~(size_t)7;
        const bool last = off + chunk >= data_len;
        if (last)
            chunk = data_len - off;

        struct netbuf* f = netbuf_alloc_cluster(true);
        if (!f)
            goto fail;
        uint8_t* h = first ? hdr : later_hdr;
        struct ipv4_header* fip = (struct ipv4_header*)h;
        fip->ver_ihl = (IPV4_VERSION << 4) | (uint8_t)(frag_header_len / 4);
        fip->total_len = htons((uint16_t)(frag_header_len + chunk));

        const uint16_t mf = !last || orig_frag & IPV4_MF ? IPV4_MF : 0;
        fip->frag_off = htons((uint16_t)((orig_frag & IPV4_OFFMASK) + off / 8 | mf));
        fip->checksum = 0;
        fip->checksum = ipv4_header_checksum(h, frag_header_len);

        bool ok = netbuf_append(f, h, frag_header_len);

        uint8_t piece[256];
        for (size_t done = 0; ok && done < chunk;) {
            const size_t n = chunk - done < sizeof(piece) ? chunk - done : sizeof(piece);
            netbuf_copydata(nb, header_len + off + done, n, piece);
            ok = netbuf_append(f, piece, n);
            done += n;
        }
        if (!ok) {
            netbuf_free_chain(f);
            goto fail;
        }
        f->rcvif = nb->rcvif;
        *tail = f;
        tail = &f->next_pkt;
        off += chunk;
    }
    netbuf_free_chain(nb);
    return 0;

fail:
    while (*out_list) {
        struct netbuf* next = (*out_list)->next_pkt;
        netbuf_free_chain(*out_list);
        *out_list = next;
    }
    netbuf_free_chain(nb);
    return -ENOBUFS;
}

static int ipv4_output_via(struct netbuf* nb, struct in_addr src, const struct in_addr dst, const uint8_t protocol,
                           const struct ipv4_options* opts, const uint32_t flags, const struct route_nexthop* nh,
                           const bool bcast_limited)
{
    struct netif* nif = nh->nif;
    if (!(nif->flags & NETIF_UP)) {
        netbuf_free_chain(nb);
        return -ENETDOWN;
    }

    const bool bcast = bcast_limited || ipv4_is_broadcast(nif, dst);
    if (bcast && !(flags & IPV4_OUTPUT_ALLOW_BCAST)) {
        netbuf_free_chain(nb);
        return -EACCES;
    }

    size_t header_len;
    if (flags & IPV4_OUTPUT_RAW) {
        struct ipv4_header ip;
        if (nb->pkt_len < sizeof(ip)) {
            netbuf_free_chain(nb);
            return -EINVAL;
        }
        netbuf_copydata(nb, 0, sizeof(ip), &ip);
        header_len = (ip.ver_ihl & 0x0f) * 4;
    } else {
        if (src.s_addr == INADDR_ANY)
            src = ipv4_select_source(nif, dst);

        if (src.s_addr == INADDR_ANY && !bcast) {
            netbuf_free_chain(nb);
            return -EADDRNOTAVAIL;
        }

        const size_t opt_len = opts ? (opts->options_len + 3u) & ~3u : 0;
        header_len = IPV4_HEADER_LEN + opt_len;
        if (opt_len > IPV4_MAX_HEADER_LEN - IPV4_HEADER_LEN || nb->pkt_len + header_len > IPV4_MAX_DATAGRAM) {
            netbuf_free_chain(nb);
            return -EMSGSIZE;
        }

        uint8_t hdr[IPV4_MAX_HEADER_LEN];
        memset(hdr, 0, header_len);
        const auto ip = (struct ipv4_header*)hdr;
        ip->ver_ihl = (IPV4_VERSION << 4) | (uint8_t)(header_len / 4);
        ip->tos = opts ? opts->tos : 0;
        ip->total_len = htons((uint16_t)(nb->pkt_len + header_len));
        ip->id = htons(next_id++);
        ip->frag_off = 0;
        ip->ttl = opts && opts->ttl ? opts->ttl : IPV4_DEFAULT_TTL;
        ip->protocol = protocol;
        ip->src = src;
        ip->dst = dst;
        if (opt_len)
            memcpy(hdr + IPV4_HEADER_LEN, opts->options, opts->options_len);
        ip->checksum = ipv4_header_checksum(hdr, header_len);

        nb = netbuf_prepend(nb, header_len);
        if (!nb)
            return -ENOBUFS;
        memcpy(nb->data, hdr, header_len);
    }
    nb->l3_offset = 0;
    nb->l4_offset = (uint16_t)header_len;

    if (ipv4_is_local(dst)) {
        nb->rcvif = nif;
        return netisr_queue(NETISR_IP, nb);
    }

    const struct in_addr next_hop = nh->gw4.s_addr != INADDR_ANY ? nh->gw4 : dst;
    const uint32_t mtu = nh->mtu && nh->mtu < nif->mtu ? nh->mtu : nif->mtu;

    if (nb->pkt_len <= mtu)
        return nif->output(nif, nb, &next_hop, AF_INET);

    struct ipv4_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    if (ntohs(ip.frag_off) & IPV4_DF) {
        netbuf_free_chain(nb);
        return -EMSGSIZE;
    }
    struct netbuf* frags;
    const int rc = ipv4_fragment(nb, nif, mtu, &frags);
    if (rc < 0)
        return rc;
    int result = 0;
    while (frags) {
        struct netbuf* next = frags->next_pkt;
        frags->next_pkt = nullptr;
        const int r = nif->output(nif, frags, &next_hop, AF_INET);
        if (r < 0 && result == 0)
            result = r;
        frags = next;
    }
    return result;
}

int ipv4_output(struct netbuf* nb, const struct in_addr src, const struct in_addr dst, const uint8_t protocol,
                const struct ipv4_options* opts, const uint32_t flags)
{
    const bool bcast_limited = dst.s_addr == INADDR_BROADCAST;
    struct route_nexthop nh;
    if (!route_lookup4(dst, &nh)) {
        struct netif* first = bcast_limited ? netif_first() : nullptr;
        if (!first) {
            netbuf_free_chain(nb);
            return -ENETUNREACH;
        }
        nh = (struct route_nexthop){ .nif = first, .family = AF_INET, .gw4 = { INADDR_ANY } };
    }
    const int rc = ipv4_output_via(nb, src, dst, protocol, opts, flags, &nh, bcast_limited);
    netif_rele(nh.nif);
    return rc;
}

int ipv4_pton(const char* str, struct in_addr* out)
{
    uint8_t parts[4];
    for (int i = 0; i < 4; i++) {
        if (*str < '0' || *str > '9')
            return -EINVAL;
        uint32_t v = 0;
        int digits = 0;
        while (*str >= '0' && *str <= '9') {
            v = v * 10 + (uint32_t)(*str++ - '0');
            if (++digits > 3 || v > 255)
                return -EINVAL;
        }
        parts[i] = (uint8_t)v;
        if (i < 3 && *str++ != '.')
            return -EINVAL;
    }
    if (*str)
        return -EINVAL;
    memcpy(out, parts, sizeof(parts));
    return 0;
}

void ipv4_ntop(const struct in_addr addr, char* out)
{
    const uint8_t* b = (const uint8_t*)&addr;
    for (int i = 0; i < 4; i++) {
        uint8_t v = b[i];
        char digits[3];
        int n = 0;
        do {
            digits[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (n)
            *out++ = digits[--n];
        if (i < 3)
            *out++ = '.';
    }
    *out = '\0';
}
