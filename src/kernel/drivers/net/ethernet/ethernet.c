#include "ethernet.h"

#include <hardware/memory/alloc.h>
#include <drivers/net/core/netif.h>

void eth_ifattach(struct netif* nif, const uint8_t* hwaddr)
{
    if (hwaddr)
        memcpy(nif->hwaddr, hwaddr, ETH_ADDR_LEN);
    else
        memset(nif->hwaddr, 0, ETH_ADDR_LEN);

    netif_attach(nif);

    nif->mtu = ETH_MTU;
    nif->hwaddr_len = ETH_ADDR_LEN;
    nif->input = eth_input;
    nif->output = eth_output;
}
