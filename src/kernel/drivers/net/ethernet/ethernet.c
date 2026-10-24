#include "ethernet.h"

#include <hardware/memory/alloc.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netisr.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/arp/arp.h>
#include <drivers/net/socket/sockaddr.h>
#include <os/errno.h>

const uint8_t eth_broadcast_addr[ETH_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static bool eth_addr_eq(const uint8_t* a, const uint8_t* b)
{
    for (int i = 0; i < ETH_ADDR_LEN; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

void eth_ifattach(struct netif* nif, const uint8_t* hw_addr)
{
    if (hw_addr)
        memcpy(nif->hw_addr, hw_addr, ETH_ADDR_LEN);
    else
        memset(nif->hw_addr, 0, ETH_ADDR_LEN);

    nif->mtu = ETH_MTU;
    nif->hw_addr_len = ETH_ADDR_LEN;
    nif->flags |= NETIF_BROADCAST | NETIF_MULTICAST;
    nif->input = eth_input;
    nif->output = eth_output;
    arp_ifattach(nif);

    netif_attach(nif);
}

void eth_ifdetach(struct netif* nif)
{
    netif_detach(nif);
    arp_ifdetach(nif);
}

void eth_input(struct netif* nif, struct netbuf* nb)
{
    nb = netbuf_pullup(nb, ETH_HEADER_LEN);
    if (!nb) {
        nif->stats.rx_errors++;
        return;
    }
    const auto eh = (const struct eth_header*)nb->data;

    if (eth_addr_is_broadcast(eh->dst)) {
        nb->flags |= NETBUF_BCAST;
    } else if (eth_addr_is_multicast(eh->dst)) {
        nb->flags |= NETBUF_MCAST;
    } else if (!eth_addr_eq(eh->dst, nif->hw_addr) && !(nif->flags & NETIF_PROMISC)) {
        nif->stats.rx_dropped++;
        netbuf_free_chain(nb);
        return;
    }

    const uint16_t type = ntohs(eh->ethertype);
    netbuf_adj(nb, ETH_HEADER_LEN);
    nb->l3_offset = 0;

    switch (type) {
    case ETHERTYPE_IPV4:
        netisr_queue(NETISR_IP, nb);
        break;
    case ETHERTYPE_ARP:
        netisr_queue(NETISR_ARP, nb);
        break;
    case ETHERTYPE_IPV6:
        netisr_queue(NETISR_IPV6, nb);
        break;
    default:
        nif->stats.rx_dropped++;
        netbuf_free_chain(nb);
        break;
    }
}

int eth_output_frame(struct netif* nif, struct netbuf* nb, const uint8_t* dst_hw, const uint16_t type)
{
    nb = netbuf_prepend(nb, ETH_HEADER_LEN);
    if (!nb) {
        nif->stats.tx_errors++;
        return -ENOBUFS;
    }
    const auto eh = (struct eth_header*)nb->data;
    memcpy(eh->dst, dst_hw, ETH_ADDR_LEN);
    memcpy(eh->src, nif->hw_addr, ETH_ADDR_LEN);
    eh->ethertype = htons(type);
    return netif_output(nif, nb);
}

int eth_output(struct netif* nif, struct netbuf* nb, const void* dst, const uint16_t family)
{
    uint8_t hw[ETH_ADDR_LEN];

    switch (family) {
    case AF_INET: {
        const int rc = arp_resolve(nif, *(const struct in_addr*)dst, nb, hw);
        if (rc == -EINPROGRESS)
            return 0;
        if (rc < 0)
            return rc;
        return eth_output_frame(nif, nb, hw, ETHERTYPE_IPV4);
    }
    default:
        // TODO: AF_INET6 through nd6_resolve
        netbuf_free_chain(nb);
        return -EAFNOSUPPORT;
    }
}

bool eth_addr_is_broadcast(const uint8_t* addr)
{
    return eth_addr_eq(addr, eth_broadcast_addr);
}

bool eth_addr_is_multicast(const uint8_t* addr)
{
    return addr[0] & 0x01;
}

void eth_map_multicast4(const void* in_addr, uint8_t* out_hw)
{
    const uint8_t* ip = in_addr;
    out_hw[0] = 0x01;
    out_hw[1] = 0x00;
    out_hw[2] = 0x5e;
    out_hw[3] = ip[1] & 0x7f;
    out_hw[4] = ip[2];
    out_hw[5] = ip[3];
}

void eth_map_multicast6(const void* in6_addr, uint8_t* out_hw)
{
    const uint8_t* ip = in6_addr;
    out_hw[0] = 0x33;
    out_hw[1] = 0x33;
    memcpy(out_hw + 2, ip + 12, 4);
}

void eth_addr_to_str(const uint8_t* addr, char* out)
{
    static constexpr char hex[] = "0123456789abcdef";
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        *out++ = hex[addr[i] >> 4];
        *out++ = hex[addr[i] & 0xf];
        *out++ = i < ETH_ADDR_LEN - 1 ? ':' : '\0';
    }
}
