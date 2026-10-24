#ifndef NET_DHCP_DHCP_H
#define NET_DHCP_DHCP_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <drivers/timer/callout.h>
#include <drivers/net/ipv4/ipv4.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC_COOKIE 0x63825363
#define DHCP_FLAG_BROADCAST 0x8000
#define DHCP_FIXED_LEN 240
#define DHCP_MIN_LEN 300
#define DHCP_INFINITE 0xFFFFFFFFu

enum dhcp_op
{
    DHCP_OP_BOOTREQUEST = 1,
    DHCP_OP_BOOTREPLY   = 2,
};

enum dhcp_msg_type
{
    DHCP_MSG_DISCOVER = 1,
    DHCP_MSG_OFFER    = 2,
    DHCP_MSG_REQUEST  = 3,
    DHCP_MSG_DECLINE  = 4,
    DHCP_MSG_ACK      = 5,
    DHCP_MSG_NAK      = 6,
    DHCP_MSG_RELEASE  = 7,
    DHCP_MSG_INFORM   = 8,
};

enum dhcp_option
{
    DHCP_OPT_PAD = 0,
    DHCP_OPT_SUBNET_MASK = 1,
    DHCP_OPT_ROUTER = 3,
    DHCP_OPT_DNS = 6,
    DHCP_OPT_HOSTNAME = 12,
    DHCP_OPT_DOMAIN_NAME = 15,
    DHCP_OPT_BROADCAST = 28,
    DHCP_OPT_REQUESTED_IP = 50,
    DHCP_OPT_LEASE_TIME = 51,
    DHCP_OPT_OVERLOAD = 52,
    DHCP_OPT_MSG_TYPE = 53,
    DHCP_OPT_SERVER_ID = 54,
    DHCP_OPT_PARAM_LIST = 55,
    DHCP_OPT_MAX_MSG_SIZE = 57,
    DHCP_OPT_RENEWAL_T1 = 58,
    DHCP_OPT_REBIND_T2 = 59,
    DHCP_OPT_CLIENT_ID = 61,
    DHCP_OPT_END = 255,
};

#define DHCP_MAX_DNS 3

struct dhcp_message {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    struct in_addr ciaddr;
    struct in_addr yiaddr;
    struct in_addr siaddr;
    struct in_addr giaddr;
    uint8_t chaddr[16];
    char sname[64];
    char file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

enum dhcp_state {
    DHCP_STATE_INIT,
    DHCP_STATE_SELECTING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND,
    DHCP_STATE_RENEWING,
    DHCP_STATE_REBINDING,
    DHCP_STATE_INIT_REBOOT,
    DHCP_STATE_REBOOTING,
};

struct dhcp_lease {
    struct in_addr addr;
    struct in_addr netmask;
    struct in_addr router;
    struct in_addr server_id;
    struct in_addr dns[DHCP_MAX_DNS];
    uint8_t dns_count;
    uint32_t lease_time;
    uint32_t t1;
    uint32_t t2;
};

struct netif;
struct netbuf;
struct socket;

struct dhcp_client {
    struct netif* nif;
    enum dhcp_state state;
    uint32_t xid;
    uint8_t tries;
    uint64_t start_tick;
    struct dhcp_lease offer;
    struct dhcp_lease lease;
    struct socket* socket;
    struct callout timer;
    uint64_t request_tick;          // when the REQUEST behind the lease was sent; T1/T2 count from here
    uint64_t t1_tick;
    uint64_t t2_tick;
    uint64_t expire_tick;           // 0: infinite lease
    bool configured;                // lease applied to the interface
    bool default_route;             // and we installed the default route
};

void dhcp_initialize();
int dhcp_start(struct netif* nif);
void dhcp_stop(struct netif* nif);

int dhcp_release(const struct netif* nif);
void dhcp_input(struct dhcp_client* client, const struct dhcp_message* msg, size_t len);

int dhcp_send_discover(const struct dhcp_client* client);
int dhcp_send_request(struct dhcp_client* client);
int dhcp_send_decline(const struct dhcp_client* client);
int dhcp_send_release(const struct dhcp_client* client);

int dhcp_parse_options(const struct dhcp_message* msg, size_t len, struct dhcp_lease* lease);
size_t dhcp_build_options(const struct dhcp_client* client, uint8_t msg_type, uint8_t* out, size_t out_len);

void dhcp_timer(void* arg);

int dhcp_apply_lease(struct dhcp_client* client);
void dhcp_remove_lease(struct dhcp_client* client);

#endif //NET_DHCP_DHCP_H
