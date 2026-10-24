#ifndef NET_IPV4_IPV4_H
#define NET_IPV4_IPV4_H

#include <os/stdint.h>
#include <utils/queue/queue.h>
#include <drivers/net/socket/sockaddr.h>
#include <drivers/net/core/inet_addr.h>

#define IPV4_VERSION        4
#define IPV4_HEADER_LEN     20
#define IPV4_MAX_HEADER_LEN 60
#define IPV4_DEFAULT_TTL    64

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_ICMPV6  58
#define IPPROTO_RAW     255

#define IPV4_RF      0x8000
#define IPV4_DF      0x4000
#define IPV4_MF      0x2000
#define IPV4_OFFMASK 0x1fff

#define IPV4_OUTPUT_FORWARDING  0x01
#define IPV4_OUTPUT_RAW         0x02
#define IPV4_OUTPUT_ALLOW_BCAST 0x04

#define INADDR_ANY       0x00000000
#define INADDR_BROADCAST 0xffffffff
#define INADDR_LOOPBACK  0x7f000001

struct sockaddr_in {
    sa_family_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

struct ipv4_header {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    struct in_addr src;
    struct in_addr dst;
} __attribute__((packed));

struct netif;
struct netbuf;
struct route_nexthop;

struct ipv4_ifaddr {
    struct dlist_node link;
    struct netif* nif;
    struct in_addr addr;
    struct in_addr netmask;
    struct in_addr broadcast;
    uint8_t prefix_len;
    uint64_t valid_until;
};

struct ipv4_options {
    uint8_t ttl;
    uint8_t tos;
    uint8_t options[40];
    uint8_t options_len;
};

void ipv4_initialize();

void ipv4_input(struct netbuf* nb);

int ipv4_output(struct netbuf* nb, struct in_addr src, struct in_addr dst, uint8_t protocol,
                const struct ipv4_options* opts, uint32_t flags);
void ipv4_forward(struct netbuf* nb);

int ipv4_fragment(struct netbuf* nb, struct netif* nif, uint32_t mtu, struct netbuf** out_list);

struct netbuf* ipv4_reassemble(struct netbuf* nb);
void ipv4_reassemble_timer(void* arg);

typedef void (*ipv4_proto_input_t)(struct netbuf* nb, size_t header_len);
int ipv4_register_protocol(uint8_t protocol, ipv4_proto_input_t input);

int ipv4_addr_add(struct netif* nif, struct in_addr addr, uint8_t prefix_len, uint64_t valid_until);
int ipv4_addr_del(struct netif* nif, struct in_addr addr);
struct ipv4_ifaddr* ipv4_addr_for_netif(const struct netif* nif);
struct ipv4_ifaddr* ipv4_addr_find(struct in_addr addr);
struct in_addr ipv4_select_source(struct netif* nif, struct in_addr dst);
bool ipv4_is_local(struct in_addr addr);
bool ipv4_is_broadcast(const struct netif* nif, struct in_addr addr);
bool ipv4_is_multicast(struct in_addr addr);

int ipv4_pton(const char* str, struct in_addr* out);
void ipv4_ntop(struct in_addr addr, char* out);

#endif //NET_IPV4_IPV4_H
