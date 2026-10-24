#include <drivers/net/ipv4/icmp.h>

#include <drivers/net/core/checksum.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/timer/callout.h>
#include <os/errno.h>

#define ICMP_MIN_LEN       8
#define ICMP_QUOTE_LEN     8
#define ICMP_ERRORS_PER_S  200

void icmp_initialize()
{
    ipv4_register_protocol(IPPROTO_ICMP, icmp_input);
}

static bool icmp_is_error_type(const uint8_t type)
{
    return type == ICMP_DEST_UNREACH || type == ICMP_REDIRECT || type == ICMP_TIME_EXCEEDED
           || type == ICMP_PARAM_PROBLEM || type == 4;  // 4: source quench (obsolete)
}

static void icmp_set_checksum(const struct netbuf* nb)
{
    const auto ih = (struct icmp_header*)nb->data;
    ih->checksum = 0;
    ih->checksum = htons(inet_checksum_netbuf(nb, 0, nb->pkt_len));
}

void icmp_input(struct netbuf* nb, const size_t header_len)
{
    if (nb->pkt_len < header_len + ICMP_MIN_LEN)
        goto drop;
    nb = netbuf_pullup(nb, header_len + ICMP_MIN_LEN);
    if (!nb)
        return;

    if (inet_checksum_netbuf(nb, header_len, nb->pkt_len - header_len) != 0)
        goto drop;

    const auto ih = (const struct icmp_header*)(nb->data + header_len);
    switch (ih->type) {
    case ICMP_ECHO_REQUEST:
        icmp_echo_reply(nb, header_len);
        return;
    case ICMP_ECHO_REPLY:
        // TODO: hand to a raw socket once those exist, so a userspace ping sees it
        break;
    case ICMP_DEST_UNREACH:
    case ICMP_TIME_EXCEEDED:
    case ICMP_PARAM_PROBLEM:
        // TODO: tell UDP/TCP about the quoted connection
        break;
    default:
        break;
    }

drop:
    netbuf_free_chain(nb);
}

static bool icmp_bad_source(const struct netif* nif, const struct in_addr src)
{
    const uint32_t a = ntohl(src.s_addr);
    return src.s_addr == INADDR_ANY || src.s_addr == INADDR_BROADCAST
           || (a >> 24) == 127 || ipv4_is_multicast(src)
           || (nif && ipv4_is_broadcast(nif, src));
}

static bool icmp_rate_ok()
{
    static uint64_t window_start;
    static uint32_t sent;
    const uint64_t now = ticks_get();
    if (now - window_start >= ticks_hz()) {
        window_start = now;
        sent = 0;
    }
    return sent++ < ICMP_ERRORS_PER_S;
}

void icmp_error(struct netbuf* orig, const uint8_t type, const uint8_t code, const uint32_t info)
{
    struct ipv4_header ip;
    if (orig->pkt_len < sizeof(ip))
        goto drop;
    netbuf_copydata(orig, 0, sizeof(ip), &ip);
    const size_t ihl = (ip.ver_ihl & 0x0f) * 4;
    if (ihl < IPV4_HEADER_LEN || ihl > orig->pkt_len)
        goto drop;

    if (ntohs(ip.frag_off) & IPV4_OFFMASK)
        goto drop;

    if ((orig->flags & (NETBUF_BCAST | NETBUF_MCAST)) || ipv4_is_multicast(ip.dst)
        || ip.dst.s_addr == INADDR_BROADCAST || (orig->rcvif && ipv4_is_broadcast(orig->rcvif, ip.dst)))
        goto drop;
    if (icmp_bad_source(orig->rcvif, ip.src))
        goto drop;

    if (ip.protocol == IPPROTO_ICMP) {
        uint8_t inner_type;
        if (orig->pkt_len < ihl + 1)
            goto drop;
        netbuf_copydata(orig, ihl, 1, &inner_type);
        if (icmp_is_error_type(inner_type))
            goto drop;
    }
    if (!icmp_rate_ok())
        goto drop;

    size_t quote = ihl + ICMP_QUOTE_LEN;
    if (quote > orig->pkt_len)
        quote = orig->pkt_len;
    uint8_t quoted[IPV4_MAX_HEADER_LEN + ICMP_QUOTE_LEN];
    netbuf_copydata(orig, 0, quote, quoted);

    struct icmp_header ih;
    ih.type = type;
    ih.code = code;
    ih.checksum = 0;
    if (type == ICMP_DEST_UNREACH && code == ICMP_UNREACH_NEEDFRAG) {
        ih.frag.unused = 0;
        ih.frag.mtu = htons((uint16_t)info);
    } else {
        ih.unused = htonl(info);
    }

    struct netbuf* nb = netbuf_alloc(true);
    if (!nb)
        goto drop;
    if (!netbuf_append(nb, &ih, sizeof(ih)) || !netbuf_append(nb, quoted, quote)) {
        netbuf_free_chain(nb);
        goto drop;
    }
    icmp_set_checksum(nb);

    const struct in_addr src = ipv4_is_local(ip.dst) ? ip.dst : (struct in_addr){ INADDR_ANY };
    netbuf_free_chain(orig);
    ipv4_output(nb, src, ip.src, IPPROTO_ICMP, nullptr, 0);
    return;

drop:
    netbuf_free_chain(orig);
}

void icmp_echo_reply(struct netbuf* nb, const size_t header_len)
{
    struct ipv4_header ip;
    netbuf_copydata(nb, 0, sizeof(ip), &ip);

    if ((nb->flags & (NETBUF_BCAST | NETBUF_MCAST)) || ipv4_is_multicast(ip.dst)
        || ip.dst.s_addr == INADDR_BROADCAST || (nb->rcvif && ipv4_is_broadcast(nb->rcvif, ip.dst))
        || icmp_bad_source(nb->rcvif, ip.src)) {
        netbuf_free_chain(nb);
        return;
    }

    netbuf_adj(nb, (ssize_t)header_len);
    nb->flags &= ~(NETBUF_BCAST | NETBUF_MCAST);

    const auto ih = (struct icmp_header*)nb->data;
    ih->type = ICMP_ECHO_REPLY;
    ih->code = 0;
    icmp_set_checksum(nb);

    ipv4_output(nb, ip.dst, ip.src, IPPROTO_ICMP, nullptr, 0);
}

int icmp_send_echo(const struct in_addr dst, const uint16_t id, const uint16_t seq, const void* payload, const size_t len)
{
    struct icmp_header ih;
    ih.type = ICMP_ECHO_REQUEST;
    ih.code = 0;
    ih.checksum = 0;
    ih.echo.id = htons(id);
    ih.echo.seq = htons(seq);

    struct netbuf* nb = netbuf_alloc(true);
    if (!nb)
        return -ENOBUFS;
    if (!netbuf_append(nb, &ih, sizeof(ih)) || !netbuf_append(nb, payload, len)) {
        netbuf_free_chain(nb);
        return -ENOBUFS;
    }
    icmp_set_checksum(nb);

    return ipv4_output(nb, (struct in_addr){ INADDR_ANY }, dst, IPPROTO_ICMP, nullptr, 0);
}
