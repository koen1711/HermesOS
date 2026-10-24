#include <drivers/net/core/loopback.h>

#include <drivers/net/core/netif.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netisr.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/ipv6.h>
#include <drivers/net/socket/sockaddr.h>
#include <os/errno.h>

static struct netif* lo;

static int loopback_queue(struct netif* nif, struct netbuf* nb, const enum netisr_proto proto)
{
    nif->stats.tx_packets++;
    nif->stats.tx_bytes += nb->pkt_len;
    nif->stats.rx_packets++;
    nif->stats.rx_bytes += nb->pkt_len;
    nb->rcvif = nif;
    nb->flags &= ~(NETBUF_BCAST | NETBUF_MCAST);
    return netisr_queue(proto, nb);
}

int loopback_output(struct netif* nif, struct netbuf* nb, const void* dst, const uint16_t family)
{
    (void)dst;
    if (!(nif->flags & NETIF_UP)) {
        nif->stats.tx_dropped++;
        netbuf_free_chain(nb);
        return -ENETDOWN;
    }
    switch (family) {
    case AF_INET:
        return loopback_queue(nif, nb, NETISR_IP);
    case AF_INET6:
        return loopback_queue(nif, nb, NETISR_IPV6);
    default:
        nif->stats.tx_dropped++;
        netbuf_free_chain(nb);
        return -EAFNOSUPPORT;
    }
}

int loopback_transmit(struct netif* nif, struct netbuf* nb)
{
    uint8_t version = 0;
    if (nb->pkt_len)
        netbuf_copydata(nb, 0, 1, &version);
    switch (version >> 4) {
    case IPV4_VERSION:
        return loopback_output(nif, nb, nullptr, AF_INET);
    case IPV6_VERSION:
        return loopback_output(nif, nb, nullptr, AF_INET6);
    default:
        nif->stats.tx_dropped++;
        netbuf_free_chain(nb);
        return -EAFNOSUPPORT;
    }
}

static const struct netif_ops loopback_ops = {
    .transmit = loopback_transmit,
};

void loopback_initialize()
{
    if (lo)
        return;
    struct netif* nif = netif_alloc(NETIF_TYPE_LOOPBACK, "lo");
    if (!nif)
        return;
    nif->ops = &loopback_ops;
    nif->output = loopback_output;
    nif->mtu = LOOPBACK_MTU;
    nif->flags |= NETIF_LOOPBACK | NETIF_MULTICAST;
    netif_attach(nif);
    netif_up(nif);
    nif->link_state = NETIF_LINK_UP;

    ipv4_addr_add(nif, (struct in_addr){ htonl(INADDR_LOOPBACK) }, 8, 0);
    ipv6_ifattach(nif);     // ::1/128
    lo = nif;
}

struct netif* loopback_netif()
{
    return lo;
}
