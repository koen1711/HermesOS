#include <drivers/net/ipv6/icmp6.h>

#include <drivers/net/core/netif.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/checksum.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/mld6.h>
#include <drivers/net/ipv6/nd6.h>
#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

#define ICMP6_HEADER_LEN   8
#define ICMP6_ERRORS_PER_S 100

#define ICMP6_QUOTE_MAX    (IPV6_MIN_MTU - IPV6_HEADER_LEN - ICMP6_HEADER_LEN)

void icmp6_initialize()
{
    ipv6_register_protocol(IPPROTO_ICMPV6, icmp6_input);
}

uint16_t icmp6_checksum(const struct netbuf* nb, const size_t offset, const size_t len,
                        const struct in6_addr* src, const struct in6_addr* dst)
{
    uint32_t sum = (uint16_t)~inet_checksum_netbuf(nb, offset, len)
                   + inet_pseudo_checksum6(src, dst, IPPROTO_ICMPV6, (uint32_t)len);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum & 0xFFFF;
}

static bool icmp6_is_error(const uint8_t type)
{
    return type < 128;
}

int icmp6_output(struct netif* nif, struct netbuf* nb, const struct in6_addr* src, const struct in6_addr* dst,
                 const uint8_t hop_limit)
{
    struct in6_addr source;
    if (!src) {
        if (ipv6_select_source(nif, dst, &source) < 0) {
            netbuf_free_chain(nb);
            return -EADDRNOTAVAIL;
        }
        src = &source;
    }
    nb = netbuf_pullup(nb, 4);
    if (!nb)
        return -ENOBUFS;
    const auto h = (struct icmp6_header*)nb->data;
    h->checksum = 0;
    h->checksum = htons(icmp6_checksum(nb, 0, nb->pkt_len, src, dst));

    const struct ipv6_options opts = { .hop_limit = hop_limit, .nif = nif };
    const uint32_t flags = ipv6_addr_is_unspecified(src) ? IPV6_OUTPUT_UNSPEC_SRC : 0;
    return ipv6_output(nb, src, dst, IPPROTO_ICMPV6, &opts, flags);
}

void icmp6_input(struct netbuf* nb, const size_t offset)
{
    struct ipv6_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    const size_t len = nb->pkt_len - offset;
    if (len < 4 || icmp6_checksum(nb, offset, len, &ip.src, &ip.dst) != 0) {
        netbuf_free_chain(nb);
        return;
    }
    uint8_t type;
    netbuf_copydata(nb, offset, 1, &type);

    switch (type) {
    case ICMP6_ECHO_REQUEST:
        icmp6_echo_reply(nb, offset);
        return;
    case ICMP6_ND_ROUTER_ADVERT:
        nd6_ra_input(nb, offset);
        return;
    case ICMP6_ND_NEIGHBOR_SOLICIT:
        nd6_ns_input(nb, offset);
        return;
    case ICMP6_ND_NEIGHBOR_ADVERT:
        nd6_na_input(nb, offset);
        return;
    case ICMP6_ND_REDIRECT:
        nd6_redirect_input(nb, offset);
        return;
    case ICMP6_MLD_QUERY:
        mld6_input(nb, offset);
        return;
    default:
        // TODO: hand errors (type < 128) to the transport's ctlinput, Packet Too Big to PMTU
        netbuf_free_chain(nb);
        return;
    }
}

void icmp6_echo_reply(struct netbuf* nb, const size_t offset)
{
    struct ipv6_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);
    if (nb->pkt_len - offset < ICMP6_HEADER_LEN || ipv6_addr_is_unspecified(&ip.src)) {
        netbuf_free_chain(nb);
        return;
    }
    struct netif* nif = nb->rcvif;

    const bool multicast = ipv6_addr_is_multicast(&ip.dst);
    const struct in6_addr src = ip.dst;
    const struct in6_addr dst = ip.src;

    netbuf_adj(nb, (ssize_t)offset);
    nb = netbuf_pullup(nb, ICMP6_HEADER_LEN);
    if (!nb)
        return;
    const auto h = (struct icmp6_header*)nb->data;
    h->type = ICMP6_ECHO_REPLY;
    h->code = 0;
    nb->flags &= ~(NETBUF_BCAST | NETBUF_MCAST);
    icmp6_output(nif, nb, multicast ? nullptr : &src, &dst, 0);
}

int icmp6_send_echo(const struct in6_addr* dst, const uint16_t id, const uint16_t seq, const void* payload,
                    const size_t len)
{
    struct netbuf* nb = netbuf_alloc_packet(ICMP6_HEADER_LEN + len);
    if (!nb)
        return -ENOBUFS;
    const struct icmp6_header h = {
        .type = ICMP6_ECHO_REQUEST,
        .data16 = { htons(id), htons(seq) },
    };
    if (!netbuf_append(nb, &h, sizeof(h)) || (len && !netbuf_append(nb, payload, len))) {
        netbuf_free_chain(nb);
        return -ENOBUFS;
    }
    return icmp6_output(nullptr, nb, nullptr, dst, 0);
}

static bool icmp6_rate_ok()
{
    static uint64_t window_start;
    static uint32_t sent;
    const uint64_t now = ticks_get();
    if (now - window_start >= ticks_hz()) {
        window_start = now;
        sent = 0;
    }
    return sent++ < ICMP6_ERRORS_PER_S;
}

static bool icmp6_error_allowed(struct netbuf* orig, const struct ipv6_header* ip, const uint8_t type,
                                const uint8_t code)
{
    if (ipv6_addr_is_unspecified(&ip->src) || ipv6_addr_is_multicast(&ip->src))
        return false;
    const bool exempt = type == ICMP6_PACKET_TOO_BIG || (type == ICMP6_PARAM_PROB && code == 2);
    if ((ipv6_addr_is_multicast(&ip->dst) || (orig->flags & (NETBUF_MCAST | NETBUF_BCAST))) && !exempt)
        return false;
    uint8_t proto;
    size_t off;
    if (ipv6_process_ext_headers(orig, &proto, &off) == 0 && proto == IPPROTO_ICMPV6 && off < orig->pkt_len) {
        uint8_t t;
        netbuf_copydata(orig, off, 1, &t);
        if (icmp6_is_error(t))
            return false;
    }
    return icmp6_rate_ok();
}

void icmp6_error(struct netbuf* orig, const uint8_t type, const uint8_t code, const uint32_t param)
{
    if (!orig)
        return;
    struct ipv6_header ip;
    if (orig->pkt_len < sizeof(ip))
        goto drop;
    netbuf_copydata(orig, 0, sizeof(ip), &ip);
    if (!icmp6_error_allowed(orig, &ip, type, code))
        goto drop;

    const size_t quote = orig->pkt_len < ICMP6_QUOTE_MAX ? orig->pkt_len : ICMP6_QUOTE_MAX;
    struct netbuf* nb = netbuf_alloc_cluster(true);
    if (!nb)
        goto drop;
    const struct icmp6_header h = { .type = type, .code = code, .data32 = htonl(param) };
    memcpy(nb->data, &h, sizeof(h));
    netbuf_copydata(orig, 0, quote, nb->data + sizeof(h));
    nb->len = nb->pkt_len = sizeof(h) + quote;

    struct netif* nif = orig->rcvif;
    const bool ours = ipv6_addr_flags(&ip.dst, nullptr) >= 0;
    const struct in6_addr src = ip.dst;
    netbuf_free_chain(orig);
    icmp6_output(nif, nb, ours ? &src : nullptr, &ip.src, 0);
    return;

drop:
    netbuf_free_chain(orig);
}
