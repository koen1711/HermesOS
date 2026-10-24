#ifndef NET_IPV6_IPV6_H
#define NET_IPV6_IPV6_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <utils/queue/queue.h>
#include <drivers/net/socket/sockaddr.h>
#include <drivers/net/core/inet_addr.h>

#define IPV6_VERSION        6
#define IPV6_HEADER_LEN     40
#define IPV6_MIN_MTU        1280
#define IPV6_DEFAULT_HLIM   64

enum ipv6_next_header
{
    IPV6_NH_HOPOPTS = 0,
    IPV6_NH_ROUTING = 43,
    IPV6_NH_FRAGMENT = 44,
    IPV6_NH_ESP = 50,
    IPV6_NH_AH = 51,
    IPV6_NH_NONE = 59,
    IPV6_NH_DSTOPTS = 60
};

enum ipv6_addr_flags
{
    IPV6_ADDR_TENTATIVE = 0x01,
    IPV6_ADDR_DUPLICATED = 0x02,
    IPV6_ADDR_DEPRECATED = 0x04,
    IPV6_ADDR_AUTOCONF = 0x08,
    IPV6_ADDR_DHCP6 = 0x10,
    IPV6_ADDR_LINKLOCAL = 0x20
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

struct ipv6_header {
    uint32_t ver_tc_flow;
    uint16_t payload_len;
    uint8_t next_header;
    uint8_t hop_limit;
    struct in6_addr src;
    struct in6_addr dst;
} __attribute__((packed, aligned(4)));

struct ipv6_frag_header {
    uint8_t next_header;
    uint8_t reserved;
    uint16_t offset_flags;
    uint32_t id;
} __attribute__((packed));

struct netif;
struct netbuf;

struct ipv6_ifaddr {
    struct dlist_node link;
    struct dlist_node all_link;
    struct netif* nif;
    struct in6_addr addr;
    uint8_t prefix_len;
    uint32_t flags;
    uint64_t preferred_until;
    uint64_t valid_until;
};

struct ipv6_options {
    uint8_t hop_limit;
    uint8_t traffic_class;
    uint32_t flow_label;
    struct netif* nif;
};

enum ipv6_output_flags
{
    IPV6_OUTPUT_UNSPEC_SRC = 0x01,
    IPV6_OUTPUT_DONTFRAG = 0x02
};

void ipv6_initialize();
void ipv6_ifattach(struct netif* nif);
void ipv6_ifdetach(struct netif* nif);

void ipv6_input(struct netbuf* nb);
int ipv6_output(struct netbuf* nb, const struct in6_addr* src, const struct in6_addr* dst,
                uint8_t next_header, const struct ipv6_options* opts, uint32_t flags);

int ipv6_process_ext_headers(struct netbuf* nb, uint8_t* out_proto, size_t* out_offset);
int ipv6_fragment(struct netbuf* nb, struct netif* nif, uint32_t mtu, struct netbuf** out_list);
struct netbuf* ipv6_reassemble(struct netbuf* nb, size_t frag_offset);
void ipv6_reassemble_timer(void* arg);

typedef void (*ipv6_proto_input_t)(struct netbuf* nb, size_t offset);
int ipv6_register_protocol(uint8_t next_header, ipv6_proto_input_t input);

int ipv6_addr_add(struct netif* nif, const struct in6_addr* addr, uint8_t prefix_len,
                  uint32_t flags, uint64_t preferred_until, uint64_t valid_until);
int ipv6_addr_del(struct netif* nif, const struct in6_addr* addr);

int ipv6_addr_flags(const struct in6_addr* addr, const struct netif* nif);
int ipv6_addr_update(const struct in6_addr* addr, uint32_t set, uint32_t clear);

int ipv6_addr_modify(const struct in6_addr* addr, int (*fn)(struct ipv6_ifaddr* ifa, void* arg), void* arg);
bool ipv6_addr_linklocal(const struct netif* nif, struct in6_addr* out);
void ipv6_addr_expire(struct netif* nif, uint64_t now);

int ipv6_select_source(struct netif* nif, const struct in6_addr* dst, struct in6_addr* out);
bool ipv6_is_local(const struct in6_addr* addr);

void ipv6_linklocal_from_hw(const uint8_t* hw_addr, struct in6_addr* out);

void ipv6_solicited_node(const struct in6_addr* addr, struct in6_addr* out);

bool ipv6_addr_equal(const struct in6_addr* a, const struct in6_addr* b);
bool ipv6_addr_is_unspecified(const struct in6_addr* addr);
bool ipv6_addr_is_loopback(const struct in6_addr* addr);
bool ipv6_addr_is_linklocal(const struct in6_addr* addr);
bool ipv6_addr_is_multicast(const struct in6_addr* addr);
bool ipv6_prefix_match(const struct in6_addr* a, const struct in6_addr* b, uint8_t prefix_len);

int ipv6_pton(const char* str, struct in6_addr* out);

void ipv6_ntop(const struct in6_addr* addr, char* out);

#endif //NET_IPV6_IPV6_H
