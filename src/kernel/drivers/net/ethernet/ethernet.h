#ifndef NET_ETHERNET_ETHERNET_H
#define NET_ETHERNET_ETHERNET_H

#include <os/stdint.h>
#include <os/stdbool.h>

#define ETH_ADDR_LEN    6
#define ETH_HEADER_LEN  14
#define ETH_MIN_LEN     60      // without FCS
#define ETH_MAX_LEN     1514    // without FCS
#define ETH_MTU         1500

#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_VLAN  0x8100
#define ETHERTYPE_IPV6  0x86DD

struct eth_header {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t ethertype;         // network byte order
} __attribute__((packed));

extern const uint8_t eth_broadcast_addr[ETH_ADDR_LEN];

struct netif;
struct netbuf;

void eth_ifattach(struct netif* nif, const uint8_t* hwaddr);
void eth_ifdetach(struct netif* nif);

void eth_input(struct netif* nif, struct netbuf* nb);

int eth_output_frame(struct netif* nif, struct netbuf* nb, const uint8_t* dst_hw, uint16_t ethertype);
int eth_output(struct netif* nif, struct netbuf* nb, const void* dst, uint16_t family);

bool eth_addr_is_broadcast(const uint8_t* addr);
bool eth_addr_is_multicast(const uint8_t* addr);

void eth_map_multicast4(const void* in_addr, uint8_t* out_hw);
void eth_map_multicast6(const void* in6_addr, uint8_t* out_hw);

void eth_addr_to_str(const uint8_t* addr, char* out);

#endif //NET_ETHERNET_ETHERNET_H
