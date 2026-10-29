#ifndef NET_INPCB_INPCB_H
#define NET_INPCB_INPCB_H

#include <os/stdint.h>
#include <os/stdbool.h>
#include <utils/queue/queue.h>
#include <utils/sync/spinlock.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/ipv6.h>

#define INPCB_HASH_SIZE      128
#define INPCB_PORT_FIRST     49152
#define INPCB_PORT_LAST      65535

// flags
enum inpcb_flags
{
    INPCB_IPV6 = 0x01,
    INPCB_IPV6_ONLY = 0x02,
    INPCB_REUSEADDR = 0x04,
    INPCB_REUSEPORT = 0x08,
    INPCB_BROADCAST = 0x10,
    INPCB_INHASH = 0x20,
    INPCB_DETACHED = 0x40
};

struct socket;
struct netbuf;

struct inpcb {
    struct dlist_node hash_link;
    struct dlist_node port_link;
    struct dlist_node all_link;
    struct in6_addr laddr;
    struct in6_addr faddr;
    uint16_t lport;
    uint16_t fport;
    uint32_t flags;
    uint8_t ttl;
    uint8_t tos;
    struct socket* socket;
    void* ppcb;
    struct inpcb_table* table;
};

struct inpcb_table {
    struct dlist all;
    struct dlist hash[INPCB_HASH_SIZE];
    struct dlist port_hash[INPCB_HASH_SIZE];
    uint16_t last_port;
    spinlock_t lock;
    const char* name;
};


void inpcb_table_init(struct inpcb_table* table, const char* name);

struct inpcb* inpcb_alloc(struct inpcb_table* table, struct socket* so);
void inpcb_free(struct inpcb* inp);

int inpcb_bind(struct inpcb* inp, const struct sockaddr* addr, socklen_t len);

int inpcb_connect(struct inpcb* inp, const struct sockaddr* addr, socklen_t len);
void inpcb_disconnect(struct inpcb* inp);

struct inpcb* inpcb_lookup(struct inpcb_table* table,
                           const struct in6_addr* faddr, uint16_t fport,
                           const struct in6_addr* laddr, uint16_t lport,
                           bool wildcard);
void inpcb_hash_insert(struct inpcb* inp);
void inpcb_hash_remove(struct inpcb* inp);
void inpcb_rehash(struct inpcb* inp);

int inpcb_alloc_ephemeral_port(struct inpcb* inp, uint16_t* out_port);

void inpcb_get_sockname(const struct inpcb* inp, struct sockaddr* out, socklen_t* len);
void inpcb_get_peername(const struct inpcb* inp, struct sockaddr* out, socklen_t* len);

int inpcb_pr_sockaddr(struct socket* so, struct sockaddr* addr, socklen_t* len);
int inpcb_pr_peeraddr(struct socket* so, struct sockaddr* addr, socklen_t* len);

int inpcb_sockaddr_to_in6(const struct inpcb* inp, const struct sockaddr* addr, socklen_t len,
                          struct in6_addr* out, uint16_t* port);
int inpcb_select_source(const struct in6_addr* dst, struct in6_addr* out);

uint16_t inpcb_l4_checksum(const struct netbuf* nb, size_t offset, size_t len, uint8_t proto,
                           const struct in6_addr* src, const struct in6_addr* dst);

int inpcb_ip_output(const struct inpcb* inp, struct netbuf* nb, uint8_t proto,
                    const struct in6_addr* src, const struct in6_addr* dst);

bool inpcb_addr_equal(const struct in6_addr* a, const struct in6_addr* b);
bool inpcb_addr_is_any(const struct in6_addr* addr);

void inpcb_map_v4(struct in_addr v4, struct in6_addr* out);
bool inpcb_is_v4_mapped(const struct in6_addr* addr);
struct in_addr inpcb_unmap_v4(const struct in6_addr* addr);

#endif //NET_INPCB_INPCB_H
