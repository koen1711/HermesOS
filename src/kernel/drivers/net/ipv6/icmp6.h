#ifndef NET_IPV6_ICMP6_H
#define NET_IPV6_ICMP6_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <drivers/net/ipv6/ipv6.h>

enum icmp6_type
{
    ICMP6_DST_UNREACH = 1,
    ICMP6_PACKET_TOO_BIG = 2,
    ICMP6_TIME_EXCEEDED = 3,
    ICMP6_PARAM_PROB = 4,
    ICMP6_ECHO_REQUEST = 128,
    ICMP6_ECHO_REPLY = 129,
    ICMP6_MLD_QUERY = 130,
    ICMP6_MLD_REPORT = 131,
    ICMP6_MLD_DONE = 132,
    ICMP6_ND_ROUTER_SOLICIT = 133,
    ICMP6_ND_ROUTER_ADVERT = 134,
    ICMP6_ND_NEIGHBOR_SOLICIT = 135,
    ICMP6_ND_NEIGHBOR_ADVERT = 136,
    ICMP6_ND_REDIRECT = 137,
    ICMP6_MLDV2_REPORT = 143
};

enum icmp6_code
{
    ICMP6_DST_UNREACH_NOPORT = 4
};

struct icmp6_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    union {
        uint32_t data32;
        uint16_t data16[2];
        uint8_t data8[4];
    };
} __attribute__((packed));

struct netbuf;
struct netif;

void icmp6_initialize();
void icmp6_input(struct netbuf* nb, size_t offset);
void icmp6_error(struct netbuf* orig, uint8_t type, uint8_t code, uint32_t param);
void icmp6_echo_reply(struct netbuf* nb, size_t offset);
int icmp6_send_echo(const struct in6_addr* dst, uint16_t id, uint16_t seq, const void* payload, size_t len);

int icmp6_output(struct netif* nif, struct netbuf* nb, const struct in6_addr* src, const struct in6_addr* dst,
                 uint8_t hop_limit);

uint16_t icmp6_checksum(const struct netbuf* nb, size_t offset, size_t len,
                        const struct in6_addr* src, const struct in6_addr* dst);

#endif //NET_IPV6_ICMP6_H
