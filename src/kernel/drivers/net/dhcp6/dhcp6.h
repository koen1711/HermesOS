#ifndef NET_DHCP6_DHCP6_H
#define NET_DHCP6_DHCP6_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <os/stdbool.h>
#include <drivers/timer/callout.h>
#include <drivers/net/ipv6/ipv6.h>

#define DHCP6_CLIENT_PORT 546
#define DHCP6_SERVER_PORT 547

enum dhcp6_msg_type
{
    DHCP6_SOLICIT = 1,
    DHCP6_ADVERTISE = 2,
    DHCP6_REQUEST = 3,
    DHCP6_CONFIRM = 4,
    DHCP6_RENEW = 5,
    DHCP6_REBIND = 6,
    DHCP6_REPLY = 7,
    DHCP6_RELEASE = 8,
    DHCP6_DECLINE = 9,
    DHCP6_INFO_REQUEST = 11
};

enum dhcp6_option
{
    DHCP6_OPT_CLIENTID = 1,
    DHCP6_OPT_SERVERID = 2,
    DHCP6_OPT_IA_NA = 3,
    DHCP6_OPT_IAADDR = 5,
    DHCP6_OPT_ORO = 6,
    DHCP6_OPT_PREFERENCE = 7,
    DHCP6_OPT_ELAPSED_TIME = 8,
    DHCP6_OPT_STATUS_CODE = 13,
    DHCP6_OPT_DNS_SERVERS = 23,
    DHCP6_OPT_INFO_REFRESH_TIME = 32
};

enum dhcp6_status
{
    DHCP6_STATUS_SUCCESS = 0,
    DHCP6_STATUS_UNSPEC_FAIL = 1,
    DHCP6_STATUS_NO_ADDRS_AVAIL = 2,
    DHCP6_STATUS_NO_BINDING = 3,
    DHCP6_STATUS_NOT_ON_LINK = 4,
    DHCP6_STATUS_USE_MULTICAST = 5
};

#define DHCP6_INFINITE 0xFFFFFFFFu

#define DHCP6_MAX_DNS 3
#define DHCP6_DUID_MAX 130

struct dhcp6_header {
    uint32_t type_xid;
} __attribute__((packed));

enum dhcp6_state {
    DHCP6_STATE_IDLE,
    DHCP6_STATE_SOLICIT,
    DHCP6_STATE_REQUEST,
    DHCP6_STATE_BOUND,
    DHCP6_STATE_RENEW,
    DHCP6_STATE_REBIND,
    DHCP6_STATE_INFO_REQUEST,
};

struct dhcp6_lease {
    struct in6_addr addr;
    uint32_t preferred_lifetime;
    uint32_t valid_lifetime;
    uint32_t t1;
    uint32_t t2;
    struct in6_addr dns[DHCP6_MAX_DNS];
    uint8_t dns_count;
    uint8_t server_duid[DHCP6_DUID_MAX];
    uint8_t server_duid_len;
    uint8_t preference;
    uint32_t refresh_time;
};

struct netif;
struct socket;

struct dhcp6_client {
    struct netif* nif;
    enum dhcp6_state state;
    bool stateful;
    uint32_t xid;
    uint32_t iaid;
    uint8_t duid[DHCP6_DUID_MAX];
    uint8_t duid_len;
    uint32_t rt;
    uint32_t tries;
    uint64_t exchange_start;
    uint64_t deadline;
    bool have_advert;
    struct dhcp6_lease advert;
    struct dhcp6_lease lease;
    bool configured;
    uint64_t t1_tick;
    uint64_t t2_tick;
    uint64_t expire_tick;
    struct socket* socket;
    struct callout timer;
};

void dhcp6_initialize();

int dhcp6_start(struct netif* nif, bool stateful);
void dhcp6_stop(struct netif* nif);
void dhcp6_input(struct dhcp6_client* client, const uint8_t* msg, size_t len);

int dhcp6_send_solicit(struct dhcp6_client* client);
int dhcp6_send_request(struct dhcp6_client* client);
int dhcp6_send_renew(struct dhcp6_client* client);
int dhcp6_send_rebind(struct dhcp6_client* client);
int dhcp6_send_release(struct dhcp6_client* client);
int dhcp6_send_info_request(struct dhcp6_client* client);

int dhcp6_parse_options(struct dhcp6_client* client, const uint8_t* opts, size_t len, struct dhcp6_lease* lease);

void dhcp6_make_duid(const struct netif* nif, uint8_t* out, uint8_t* out_len);
void dhcp6_timer(void* arg);
int dhcp6_apply_lease(struct dhcp6_client* client);

#endif //NET_DHCP6_DHCP6_H
