#ifndef NET_CORE_NETIF_H
#define NET_CORE_NETIF_H

#include <os/stdint.h>
#include <utils/queue/queue.h>
#include <utils/sync/spinlock.h>
#include <drivers/net/core/netqueue.h>

#define NETIF_NAME_LEN   16
#define NETIF_HWADDR_LEN 6

enum netif_flags
{
    NETIF_UP = 0x0001,
    NETIF_RUNNING = 0x0002,
    NETIF_BROADCAST = 0x0004,
    NETIF_LOOPBACK = 0x0008,
    NETIF_MULTICAST = 0x0010,
    NETIF_PROMISC = 0x0020
};

enum netif_link_state
{
    NETIF_LINK_UNKNOWN = 0,
    NETIF_LINK_DOWN = 1,
    NETIF_LINK_UP = 2
};

enum netif_type
{
    NETIF_TYPE_ETHERNET = 1,
    NETIF_TYPE_LOOPBACK = 2
};

enum netif_ioctl_cmd
{
    NETIF_IOCTL_SET_FLAGS = 1,
    NETIF_IOCTL_SET_MTU = 2,
    NETIF_IOCTL_ADD_MULTI = 3,
    NETIF_IOCTL_DEL_MULTI = 4,
    NETIF_IOCTL_GET_MEDIA = 5
};

struct netif;
struct netbuf;

struct netif_ops {
    int (*init)(struct netif* nif);
    int (*transmit)(struct netif* nif, struct netbuf* nb);
    int (*ioctl)(struct netif* nif, uint32_t cmd, void* arg);
    void (*qflush)(struct netif* nif);
    void (*poll)(struct netif* nif);
};

typedef int (*netif_output_t)(struct netif* nif, struct netbuf* nb, const void* dst, uint16_t family);
typedef void (*netif_input_t)(struct netif* nif, struct netbuf* nb);

struct netif_stats {
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_errors, tx_dropped;
};

struct netif {
    struct dlist_node link;
    char name[NETIF_NAME_LEN];
    uint32_t index;
    uint32_t refcount;
    bool gone;
    uint32_t type;
    uint32_t flags;
    uint32_t link_state;
    uint32_t mtu;
    uint8_t hw_addr[NETIF_HWADDR_LEN];
    uint8_t hw_addr_len;

    const struct netif_ops* ops;
    void* softc;

    netif_output_t output;
    netif_input_t input;

    struct dlist ipv4_addrs;
    struct dlist ipv6_addrs;
    void* arp_cache;
    void* nd6_info;
    void* dhcp_state;
    void* dhcp6_state;

    struct netqueue send_queue;
    struct netif_stats stats;
    spinlock_t lock;
};

void netif_initialize();

struct netif* netif_alloc(uint32_t type, const char* name_prefix);
void netif_free(struct netif* nif);
void netif_ref(struct netif* nif);
void netif_rele(struct netif* nif);
int netif_attach(struct netif* nif);
void netif_detach(struct netif* nif);

void netif_input(struct netif* nif, struct netbuf* nb);
int netif_output(struct netif* nif, struct netbuf* nb);

struct netif* netif_find_by_name(const char* name);
struct netif* netif_find_by_index(uint32_t index);
struct netif* netif_first();
struct netif* netif_next(struct netif* nif);

int netif_up(struct netif* nif);
int netif_down(struct netif* nif);
int netif_set_mtu(struct netif* nif, uint32_t mtu);
int netif_set_hw_addr(struct netif* nif, const uint8_t* hwaddr, uint8_t len);
int netif_ioctl(struct netif* nif, uint32_t cmd, void* arg);

void netif_poll_all();
void netif_link_state_change(struct netif* nif, uint32_t link_state);

#endif //NET_CORE_NETIF_H
