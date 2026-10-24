#ifndef NET_DNS_DNS_H
#define NET_DNS_DNS_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <drivers/net/socket/sockaddr.h>

#define DNS_PORT        53
#define DNS_MAX_SERVERS 4
#define DNS_MAX_NAME    255
#define DNS_MAX_PACKET  512
#define DNS_CACHE_SIZE  32

enum dns_type
{
    DNS_TYPE_A = 1,
    DNS_TYPE_CNAME = 5,
    DNS_TYPE_AAAA = 28
};
enum dns_class
{
    DNS_CLASS_IN = 1
};

enum dns_header_flags
{
    DNS_FLAG_QR = 0x8000,
    DNS_FLAG_RD = 0x0100,
    DNS_FLAG_RA = 0x0080,
    DNS_RCODE_MASK = 0x000f
};

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

struct dns_result {
    sa_family_t family;
    uint8_t addr[16];
    uint32_t ttl;
};

void dns_initialize();

int dns_set_servers(const struct sockaddr_storage* servers, size_t count);
int dns_resolve(const char* name, sa_family_t family, struct dns_result* out, size_t max_out, uint32_t timeout_ms);

size_t dns_build_query(uint16_t id, const char* name, uint16_t qtype, uint8_t* out, size_t out_len);
int dns_parse_response(const uint8_t* msg, size_t len, uint16_t expected_id,
                       struct dns_result* out, size_t max_out);

int dns_decode_name(const uint8_t* msg, size_t len, size_t offset, char* out, size_t out_len);

bool dns_cache_lookup(const char* name, sa_family_t family, struct dns_result* out);
void dns_cache_insert(const char* name, const struct dns_result* result);
void dns_cache_flush();

#endif //NET_DNS_DNS_H
