#ifndef NET_CORE_NETBUF_H
#define NET_CORE_NETBUF_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <os/types.h>

#define NETBUF_SIZE         256
#define NETBUF_CLUSTER_SIZE 2048
#define NETBUF_HEADROOM     64

enum netbuf_flags
{
    NETBUF_PKTHDR = 0x0001,
    NETBUF_EXT = 0x0002,
    NETBUF_BCAST = 0x0004,
    NETBUF_MCAST = 0x0008
};

enum netbuf_csum_flags
{
    NETBUF_CSUM_IP_VALID = 0x0001,
    NETBUF_CSUM_L4_VALID = 0x0002,
    NETBUF_CSUM_IP_TX = 0x0100,
    NETBUF_CSUM_L4_TX = 0x0200
};

struct netif;

struct netbuf {
    struct netbuf* next;
    struct netbuf* next_pkt;
    uint8_t* data;
    size_t len;
    uint8_t* buf;
    size_t buf_size;
    uint32_t flags;

    size_t pkt_len;
    struct netif* rcvif;
    uint32_t csum_flags;
    uint16_t l3_offset;
    uint16_t l4_offset;
};

void netbuf_initialize();

struct netbuf* netbuf_alloc(bool pkthdr);
struct netbuf* netbuf_alloc_cluster(bool pkthdr);

struct netbuf* netbuf_alloc_packet(size_t len);
void netbuf_free(struct netbuf* nb);
void netbuf_free_chain(struct netbuf* nb);

struct netbuf* netbuf_prepend(struct netbuf* nb, size_t len);
void netbuf_adj(struct netbuf* nb, ssize_t len);
struct netbuf* netbuf_pullup(struct netbuf* nb, size_t len);
bool netbuf_append(struct netbuf* nb, const void* data, size_t len);
void netbuf_copydata(const struct netbuf* nb, size_t offset, size_t len, void* out);
bool netbuf_copyback(struct netbuf* nb, size_t offset, size_t len, const void* data);
struct netbuf* netbuf_dup(const struct netbuf* nb);
struct netbuf* netbuf_split(struct netbuf* nb, size_t offset);
void netbuf_cat(struct netbuf* nb, struct netbuf* tail);

#endif //NET_CORE_NETBUF_H
