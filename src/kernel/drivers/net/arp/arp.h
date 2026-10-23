#ifndef NET_ARP_ARP_H
#define NET_ARP_ARP_H

#include <os/stdint.h>
#include <utils/queue/queue.h>
#include <drivers/net/ethernet/ethernet.h>
#include <drivers/net/ipv4/ipv4.h>

#define ARP_HTYPE_ETHERNET 1
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2

#define ARP_MAX_PENDING    4
#define ARP_MAX_TRIES      5
#define ARP_REACHABLE_MS   (20 * 60 * 1000)

struct arp_header {
    uint16_t hardware;
    uint16_t protocol;
    uint8_t hardware_len;
    uint8_t protocol_len;
    uint16_t op;
    uint8_t sha[ETH_ADDR_LEN];
    struct in_addr spa;
    uint8_t tha[ETH_ADDR_LEN];
    struct in_addr tpa;
} __attribute__((packed));

enum arp_state {
    ARP_INCOMPLETE,
    ARP_REACHABLE,
    ARP_STALE,
    ARP_STATIC,
};

struct netif;
struct netbuf;

struct arp_entry {
    struct dlist_node link;
    struct in_addr ip;
    uint8_t hw[ETH_ADDR_LEN];
    enum arp_state state;
    uint64_t expires;
    uint8_t tries;
    struct netbuf* pending;
    uint8_t pending_count;
};

void arp_initialize();
void arp_ifattach(struct netif* nif);
void arp_ifdetach(struct netif* nif);

void arp_input(struct netbuf* nb);

int arp_resolve(struct netif* nif, struct in_addr ip, struct netbuf* nb, uint8_t* out_hw);
int arp_request(struct netif* nif, struct in_addr src, struct in_addr target);

int arp_announce(struct netif* nif, struct in_addr ip);
int arp_probe(struct netif* nif, struct in_addr ip);

struct arp_entry* arp_cache_add(struct netif* nif, struct in_addr ip, const uint8_t* hw, enum arp_state state);
struct arp_entry* arp_cache_lookup(struct netif* nif, struct in_addr ip);
void arp_cache_remove(struct netif* nif, struct in_addr ip);
void arp_cache_flush(const struct netif* nif);

void arp_timer(const void* arg);

#endif //NET_ARP_ARP_H
