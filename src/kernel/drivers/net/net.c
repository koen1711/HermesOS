#include <drivers/net/net.h>

#include <drivers/net/arp/arp.h>
#include <drivers/net/core/loopback.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/netisr.h>
#include <drivers/net/core/route.h>
#include <drivers/net/dhcp/dhcp.h>
#include <drivers/net/dhcp6/dhcp6.h>
#include <drivers/net/dns/dns.h>
#include <drivers/net/ipv4/icmp.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/icmp6.h>
#include <drivers/net/ipv6/ipv6.h>
#include <drivers/net/ipv6/mld6.h>
#include <drivers/net/ipv6/nd6.h>
#include <drivers/net/socket/protosw.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/tcp/tcp_subr.h>
#include <drivers/net/udp/udp.h>
#include <hardware/network/e1000/e1000.h>

void net_initialize()
{
    netbuf_initialize();
    netif_initialize();
    netisr_initialize();
    route_initialize();
    arp_initialize();
    ipv4_initialize();
    icmp_initialize();
    ipv6_initialize();
    icmp6_initialize();
    nd6_initialize();
    mld6_initialize();
    udp_initialize();
    tcp_initialize();
    socket_initialize();
    protosw_initialize();
    dns_initialize();
    dhcp_initialize();
    dhcp6_initialize();

    loopback_initialize();
    e1000_initialize();

    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        if (nif->flags & NETIF_LOOPBACK)
            continue;
        netif_up(nif);
        if (nif->link_state == NETIF_LINK_UP)
            dhcp_start(nif);
    }
}
