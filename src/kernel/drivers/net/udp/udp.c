#include <drivers/net/udp/udp.h>

#include <drivers/net/inpcb/inpcb.h>
#include <drivers/net/ipv4/icmp.h>
#include <drivers/net/ipv6/icmp6.h>
#include <drivers/net/socket/protosw.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/netbuf.h>
#include <os/errno.h>

#define UDP_PROTOSW(dom) {                      \
    .type = SOCK_DGRAM,                         \
    .domain = (dom),                            \
    .protocol = IPPROTO_UDP,                    \
    .flags = PR_ATOMIC | PR_ADDR,               \
    .pr_attach = udp_attach,                    \
    .pr_detach = udp_detach,                    \
    .pr_bind = udp_bind,                        \
    .pr_connect = udp_connect,                  \
    .pr_disconnect = udp_disconnect,            \
    .pr_send = udp_send,                        \
    .pr_shutdown = udp_shutdown,                \
    .pr_sockaddr = inpcb_pr_sockaddr,           \
    .pr_peeraddr = inpcb_pr_peeraddr,           \
}

struct inpcb_table udp_pcbs;
struct protosw udp_protosw4 = UDP_PROTOSW(AF_INET);
struct protosw udp_protosw6 = UDP_PROTOSW(AF_INET6);

void udp_initialize()
{
    inpcb_table_init(&udp_pcbs, "udp");
    ipv4_register_protocol(IPPROTO_UDP, udp_input4);
    ipv6_register_protocol(IPPROTO_UDP, udp_input6);
}

static void udp_lock(uint64_t* flags)
{
    spinlock_acquire_irqsave(&udp_pcbs.lock, flags);
}

static void udp_unlock(const uint64_t flags)
{
    spinlock_release_irqrestore(&udp_pcbs.lock, flags);
}

static bool udp_validate(struct netbuf* nb, const size_t off, const struct in6_addr* src, const struct in6_addr* dst)
{
    const auto uh = (const struct udp_header*)(nb->data + off);
    const size_t ulen = ntohs(uh->length);
    const size_t avail = nb->pkt_len - off;
    if (ulen < UDP_HEADER_LEN || ulen > avail)
        return false;
    if (ulen < avail)
        netbuf_adj(nb, -(ssize_t)(avail - ulen));

    const bool v4 = inpcb_is_v4_mapped(dst);
    if (v4 && !uh->checksum)
        return true;
    if (!v4 && !uh->checksum)
        return false;
    if (nb->csum_flags & NETBUF_CSUM_L4_VALID)
        return true;
    return inpcb_l4_checksum(nb, off, ulen, IPPROTO_UDP, src, dst) == 0;
}

static bool udp_is_multicast(const struct netbuf* nb, const struct in6_addr* dst)
{
    if (nb->flags & (NETBUF_BCAST | NETBUF_MCAST))
        return true;
    if (!inpcb_is_v4_mapped(dst))
        return dst->s6_addr[0] == 0xFF;
    const struct in_addr v4 = inpcb_unmap_v4(dst);
    return v4.s_addr == INADDR_BROADCAST || ipv4_is_multicast(v4);
}

static void udp_sender_sockaddr(const struct inpcb* inp, const struct in6_addr* src, const uint16_t sport,
                                struct sockaddr_storage* out, socklen_t* len)
{
    const struct inpcb peer = { .faddr = *src, .fport = sport, .flags = inp->flags };
    *len = sizeof(struct sockaddr_storage);
    inpcb_get_peername(&peer, (struct sockaddr*)out, len);
}

static void udp_deliver(const struct inpcb* inp, struct netbuf* nb, const struct in6_addr* src, const uint16_t sport)
{
    struct socket* so = inp->socket;
    if (!so || (so->state & SS_CANTRCVMORE)) {
        netbuf_free_chain(nb);
        return;
    }
    struct sockaddr_storage from;
    socklen_t from_len;
    udp_sender_sockaddr(inp, src, sport, &from, &from_len);
    if (!sockbuf_append_addr(&so->rcv, (struct sockaddr*)&from, from_len, nb)) {
        netbuf_free_chain(nb);
        return;
    }
    sock_wakeup(so);
}

static bool udp_pcb_matches(const struct inpcb* inp, const struct in6_addr* src, const uint16_t sport,
                            const struct in6_addr* dst, const uint16_t dport)
{
    if (inp->lport != dport)
        return false;
    if (!inpcb_addr_is_any(&inp->laddr) && !inpcb_addr_equal(&inp->laddr, dst))
        return false;
    if (inp->fport && (inp->fport != sport || !inpcb_addr_equal(&inp->faddr, src)))
        return false;

    const bool v4 = inpcb_is_v4_mapped(dst);
    return v4 ? !(inp->flags & INPCB_IPV6_ONLY) : (inp->flags & INPCB_IPV6);
}

static bool udp_deliver_all(struct netbuf* nb, const struct in6_addr* src, const uint16_t sport,
                            const struct in6_addr* dst, const uint16_t dport)
{
    bool delivered = false;
    for (struct dlist_node* n = dlist_first(&udp_pcbs.all); n; n = dlist_next(n)) {
        const auto inp = dlist_entry(n, struct inpcb, all_link);
        if (!udp_pcb_matches(inp, src, sport, dst, dport))
            continue;
        struct netbuf* copy = netbuf_dup(nb);
        if (copy) {
            udp_deliver(inp, copy, src, sport);
            delivered = true;
        }
    }
    netbuf_free_chain(nb);
    return delivered;
}

static void udp_port_unreachable(struct netbuf* nb, const struct in6_addr* dst)
{
    if (inpcb_is_v4_mapped(dst))
        icmp_error(nb, ICMP_DEST_UNREACH, ICMP_UNREACH_PORT, 0);
    else
        icmp6_error(nb, ICMP6_DST_UNREACH, ICMP6_DST_UNREACH_NOPORT, 0);
}

static void udp_input_packet(struct netbuf* nb, const size_t off, const struct in6_addr* src, const struct in6_addr* dst)
{
    if (!udp_validate(nb, off, src, dst)) {
        netbuf_free_chain(nb);
        return;
    }
    const auto uh = (const struct udp_header*)(nb->data + off);
    const uint16_t sport = uh->src_port;
    const uint16_t dport = uh->dst_port;
    const bool multicast = udp_is_multicast(nb, dst);

    uint64_t flags;
    udp_lock(&flags);
    struct inpcb* inp = multicast ? nullptr : inpcb_lookup(&udp_pcbs, src, sport, dst, dport, true);
    if (!multicast && !inp) {
        udp_unlock(flags);
        udp_port_unreachable(nb, dst);
        return;
    }
    netbuf_adj(nb, off + UDP_HEADER_LEN);
    if (multicast)
        udp_deliver_all(nb, src, sport, dst, dport);
    else
        udp_deliver(inp, nb, src, sport);
    udp_unlock(flags);
}

void udp_input4(struct netbuf* nb, const size_t header_len)
{
    nb = netbuf_pullup(nb, header_len + UDP_HEADER_LEN);
    if (!nb)
        return;
    const auto ip = (const struct ipv4_header*)nb->data;
    struct in6_addr src, dst;
    inpcb_map_v4(ip->src, &src);
    inpcb_map_v4(ip->dst, &dst);
    udp_input_packet(nb, header_len, &src, &dst);
}

void udp_input6(struct netbuf* nb, const size_t offset)
{
    nb = netbuf_pullup(nb, offset + UDP_HEADER_LEN);
    if (!nb)
        return;
    const auto ip6 = (const struct ipv6_header*)nb->data;
    const struct in6_addr src = ip6->src;
    const struct in6_addr dst = ip6->dst;
    udp_input_packet(nb, offset, &src, &dst);
}

static int udp_resolve_dst(const struct inpcb* inp, const struct sockaddr* dst, const socklen_t dst_len,
                           struct in6_addr* faddr, uint16_t* fport)
{
    if (!dst) {
        if (!inp->fport)
            return -EDESTADDRREQ;
        *faddr = inp->faddr;
        *fport = inp->fport;
        return 0;
    }
    if (inp->fport)
        return -EISCONN;
    const int err = inpcb_sockaddr_to_in6(inp, dst, dst_len, faddr, fport);
    if (err)
        return err;
    return *fport ? 0 : -EINVAL;
}

static int udp_output_locked(struct inpcb* inp, struct netbuf* nb, const struct sockaddr* dst, const socklen_t dst_len)
{
    struct in6_addr faddr, laddr;
    uint16_t fport;
    int err = udp_resolve_dst(inp, dst, dst_len, &faddr, &fport);
    if (!err && !inp->lport)
        err = inpcb_bind(inp, nullptr, 0);
    laddr = inp->laddr;
    if (!err && inpcb_addr_is_any(&laddr))
        err = inpcb_select_source(&faddr, &laddr);
    if (!err && nb->pkt_len + UDP_HEADER_LEN > 0xFFFF)
        err = -EMSGSIZE;
    if (err) {
        netbuf_free_chain(nb);
        return err;
    }

    nb = netbuf_prepend(nb, UDP_HEADER_LEN);
    if (!nb)
        return -ENOBUFS;
    const auto uh = (struct udp_header*)nb->data;
    uh->src_port = inp->lport;
    uh->dst_port = fport;
    uh->length = htons(nb->pkt_len);
    uh->checksum = 0;
    const uint16_t sum = inpcb_l4_checksum(nb, 0, nb->pkt_len, IPPROTO_UDP, &laddr, &faddr);
    uh->checksum = htons(sum ? sum : 0xFFFF);
    return inpcb_ip_output(inp, nb, IPPROTO_UDP, &laddr, &faddr);
}

int udp_output(struct inpcb* inp, struct netbuf* nb, const struct sockaddr* dst, const socklen_t dst_len)
{
    uint64_t flags;
    udp_lock(&flags);
    const int err = udp_output_locked(inp, nb, dst, dst_len);
    udp_unlock(flags);
    return err;
}

int udp_attach(struct socket* so, int proto)
{
    (void)proto;
    uint64_t flags;
    udp_lock(&flags);
    struct inpcb* inp = inpcb_alloc(&udp_pcbs, so);
    so->pcb = inp;
    udp_unlock(flags);
    return inp ? 0 : -ENOMEM;
}

int udp_bind(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    uint64_t flags;
    udp_lock(&flags);
    const int err = inpcb_bind(so->pcb, addr, len);
    udp_unlock(flags);
    return err;
}

int udp_connect(struct socket* so, const struct sockaddr* addr, const socklen_t len)
{
    struct inpcb* inp = so->pcb;
    uint64_t flags;
    udp_lock(&flags);
    if (inp->fport)
        inpcb_disconnect(inp);
    const int err = inpcb_connect(inp, addr, len);
    udp_unlock(flags);
    if (!err)
        sock_is_connected(so);
    return err;
}

int udp_disconnect(struct socket* so)
{
    struct inpcb* inp = so->pcb;
    uint64_t flags;
    udp_lock(&flags);
    const bool connected = inp->fport;
    if (connected)
        inpcb_disconnect(inp);
    udp_unlock(flags);
    if (!connected)
        return -ENOTCONN;
    so->state &= ~SS_ISCONNECTED;
    return 0;
}

int udp_send(struct socket* so, struct netbuf* nb, const struct sockaddr* dst, const socklen_t dst_len, const int flags)
{
    (void)flags;
    return udp_output(so->pcb, nb, dst, dst_len);
}

int udp_shutdown(struct socket* so, const int how)
{
    if (how != SHUT_RD)
        sock_cant_send_more(so);
    return 0;
}

void udp_detach(struct socket* so)
{
    uint64_t flags;
    udp_lock(&flags);
    if (so->pcb)
        inpcb_free(so->pcb);
    so->pcb = nullptr;
    udp_unlock(flags);
}
