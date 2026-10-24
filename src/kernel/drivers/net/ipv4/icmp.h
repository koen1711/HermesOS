#ifndef NET_IPV4_ICMP_H
#define NET_IPV4_ICMP_H

#include <os/stdint.h>
#include <drivers/net/ipv4/ipv4.h>

enum icmp_type {
    ICMP_ECHO_REPLY      = 0,
    ICMP_DEST_UNREACH    = 3,
    ICMP_REDIRECT        = 5,
    ICMP_ECHO_REQUEST    = 8,
    ICMP_TIME_EXCEEDED   = 11,
    ICMP_PARAM_PROBLEM   = 12
};

enum icmp_code
{
    ICMP_UNREACH_NET_UNKNOWN = 0,
    ICMP_UNREACH_HOST_UNKNOWN = 1,
    ICMP_UNREACH_PROTOCOL = 2,
    ICMP_UNREACH_PORT = 3,
    ICMP_UNREACH_NEEDFRAG = 4,
};

struct icmp_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    union {
        struct { uint16_t id; uint16_t seq; } echo;
        struct { uint16_t unused; uint16_t mtu; } frag;
        struct in_addr gateway;
        uint32_t unused;
    };
} __attribute__((packed));

struct netbuf;

void icmp_initialize();
void icmp_input(struct netbuf* nb, size_t header_len);

void icmp_error(struct netbuf* orig, uint8_t type, uint8_t code, uint32_t info);
void icmp_echo_reply(struct netbuf* nb, size_t header_len);
int icmp_send_echo(struct in_addr dst, uint16_t id, uint16_t seq, const void* payload, size_t len);

#endif //NET_IPV4_ICMP_H
