#include <drivers/net/dhcp6/dhcp6.h>

#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/random.h>
#include <drivers/net/dns/dns.h>
#include <drivers/net/inpcb/inpcb.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/udp/udp.h>
#include <hardware/memory/alloc.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#define DHCP6_MAX_MSG 1500
#define DHCP6_OPT_HDR 4

#define DHCP6_SOL_MAX_DELAY 1000
#define DHCP6_SOL_TIMEOUT   1000
#define DHCP6_SOL_MAX_RT    3600000
#define DHCP6_REQ_TIMEOUT   1000
#define DHCP6_REQ_MAX_RT    30000
#define DHCP6_REQ_MAX_RC    10
#define DHCP6_REN_TIMEOUT   10000
#define DHCP6_REN_MAX_RT    600000
#define DHCP6_REB_TIMEOUT   10000
#define DHCP6_REB_MAX_RT    600000
#define DHCP6_INF_MAX_DELAY 1000
#define DHCP6_INF_TIMEOUT   1000
#define DHCP6_INF_MAX_RT    3600000
#define DHCP6_IRT_DEFAULT_S 86400
#define DHCP6_NOT_READY_MS  1000

static spinlock_t dhcp6_lock;
static struct socket* dhcp6_socket;
static struct callout dhcp6_rx_callout;
static bool dhcp6_ready;
static uint8_t dhcp6_buf[DHCP6_MAX_MSG];

static const struct in6_addr dhcp6_all_servers = {
    .s6_addr = { 0xFF, 0x02, [13] = 0x01, [15] = 0x02 }
};

static void dhcp6_transmit(struct dhcp6_client* client);

static uint64_t dhcp6_ms(const uint64_t ms)
{
    return ticks_from_ms(ms);
}

static uint64_t dhcp6_seconds(const uint32_t s, const uint64_t from)
{
    return s == DHCP6_INFINITE ? 0 : from + dhcp6_ms((uint64_t)s * 1000);
}

static uint16_t dhcp6_get16(const uint8_t* p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t dhcp6_get32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void dhcp6_put16(uint8_t* p, const uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void dhcp6_put32(uint8_t* p, const uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t dhcp6_jitter(const uint32_t x)
{
    const int32_t permille = (int32_t)(net_random32() % 201) - 100;
    return (uint32_t)((int64_t)x + (int64_t)x * permille / 1000);
}

void dhcp6_make_duid(const struct netif* nif, uint8_t* out, uint8_t* out_len)
{
    dhcp6_put16(out, 3);
    dhcp6_put16(out + 2, 1);
    memcpy(out + 4, nif->hw_addr, nif->hw_addr_len);
    *out_len = (uint8_t)(4 + nif->hw_addr_len);
}

struct dhcp6_ia_result {
    bool present;
    int status;
};

static void dhcp6_parse_ia(const struct dhcp6_client* client, const uint8_t* p, const size_t len,
                           struct dhcp6_lease* lease, struct dhcp6_ia_result* ia)
{
    if (len < 12 || dhcp6_get32(p) != client->iaid || ia->present)
        return;
    const uint32_t t1 = dhcp6_get32(p + 4), t2 = dhcp6_get32(p + 8);
    if (t1 && t2 && t1 > t2)
        return;
    ia->present = true;
    ia->status = DHCP6_STATUS_SUCCESS;
    lease->t1 = t1;
    lease->t2 = t2;
    for (size_t i = 12; i + DHCP6_OPT_HDR <= len;) {
        const uint16_t code = dhcp6_get16(p + i), olen = dhcp6_get16(p + i + 2);
        const uint8_t* v = p + i + DHCP6_OPT_HDR;
        if (i + DHCP6_OPT_HDR + olen > len)
            break;
        if (code == DHCP6_OPT_IAADDR && olen >= 24 && ipv6_addr_is_unspecified(&lease->addr)) {
            const uint32_t pref = dhcp6_get32(v + 16), valid = dhcp6_get32(v + 20);
            if (pref <= valid) {
                memcpy(&lease->addr, v, sizeof(struct in6_addr));
                lease->preferred_lifetime = pref;
                lease->valid_lifetime = valid;
            }
        } else if (code == DHCP6_OPT_STATUS_CODE && olen >= 2) {
            ia->status = dhcp6_get16(v);
        }
        i += DHCP6_OPT_HDR + olen;
    }
}

static int dhcp6_parse(struct dhcp6_client* client, const uint8_t* opts, const size_t len,
                       struct dhcp6_lease* lease, struct dhcp6_ia_result* ia)
{
    memset(lease, 0, sizeof(struct dhcp6_lease));
    memset(ia, 0, sizeof(struct dhcp6_ia_result));
    int status = DHCP6_STATUS_SUCCESS;
    bool client_id_ok = false;
    for (size_t i = 0; i + DHCP6_OPT_HDR <= len;) {
        const uint16_t code = dhcp6_get16(opts + i), olen = dhcp6_get16(opts + i + 2);
        const uint8_t* v = opts + i + DHCP6_OPT_HDR;
        if (i + DHCP6_OPT_HDR + olen > len)
            return -EINVAL;
        switch (code) {
        case DHCP6_OPT_CLIENTID:
            client_id_ok = olen == client->duid_len && !memcmp((void*)v, client->duid, olen);
            break;
        case DHCP6_OPT_SERVERID:
            if (olen && olen <= DHCP6_DUID_MAX) {
                memcpy(lease->server_duid, v, olen);
                lease->server_duid_len = (uint8_t)olen;
            }
            break;
        case DHCP6_OPT_IA_NA:
            dhcp6_parse_ia(client, v, olen, lease, ia);
            break;
        case DHCP6_OPT_STATUS_CODE:
            if (olen >= 2)
                status = dhcp6_get16(v);
            break;
        case DHCP6_OPT_DNS_SERVERS:
            for (size_t k = 0; k + 16 <= olen && lease->dns_count < DHCP6_MAX_DNS; k += 16)
                memcpy(&lease->dns[lease->dns_count++], v + k, sizeof(struct in6_addr));
            break;
        case DHCP6_OPT_PREFERENCE:
            if (olen == 1)
                lease->preference = v[0];
            break;
        case DHCP6_OPT_INFO_REFRESH_TIME:
            if (olen == 4)
                lease->refresh_time = dhcp6_get32(v);
            break;
        default:
            break;
        }
        i += DHCP6_OPT_HDR + olen;
    }
    if (!client_id_ok || !lease->server_duid_len)
        return -EINVAL;
    return status;
}

int dhcp6_parse_options(struct dhcp6_client* client, const uint8_t* opts, const size_t len, struct dhcp6_lease* lease)
{
    struct dhcp6_ia_result ia;
    return dhcp6_parse(client, opts, len, lease, &ia);
}

static size_t dhcp6_put_option(uint8_t* out, size_t pos, const uint16_t code, const void* data, const uint16_t len)
{
    dhcp6_put16(out + pos, code);
    dhcp6_put16(out + pos + 2, len);
    if (len)
        memcpy(out + pos + DHCP6_OPT_HDR, data, len);
    return pos + DHCP6_OPT_HDR + len;
}

static size_t dhcp6_build(const struct dhcp6_client* client, const uint8_t type, uint8_t* out)
{
    out[0] = type;
    out[1] = (uint8_t)(client->xid >> 16);
    out[2] = (uint8_t)(client->xid >> 8);
    out[3] = (uint8_t)client->xid;
    size_t n = 4;

    n = dhcp6_put_option(out, n, DHCP6_OPT_CLIENTID, client->duid, client->duid_len);
    const bool with_server = type == DHCP6_REQUEST || type == DHCP6_RENEW || type == DHCP6_RELEASE;
    const struct dhcp6_lease* srv = type == DHCP6_REQUEST ? &client->advert : &client->lease;
    if (with_server)
        n = dhcp6_put_option(out, n, DHCP6_OPT_SERVERID, srv->server_duid, srv->server_duid_len);

    const uint64_t cs = client->tries ? (ticks_get() - client->exchange_start) * 100 / dhcp6_ms(1000) : 0;
    uint8_t elapsed[2];
    dhcp6_put16(elapsed, cs > 0xFFFF ? 0xFFFF : (uint16_t)cs);
    if (type != DHCP6_RELEASE)
        n = dhcp6_put_option(out, n, DHCP6_OPT_ELAPSED_TIME, elapsed, 2);

    if (type != DHCP6_INFO_REQUEST) {
        const struct dhcp6_lease* hint = type == DHCP6_REQUEST ? &client->advert : &client->lease;
        uint8_t ia[12 + DHCP6_OPT_HDR + 24] = {0};
        dhcp6_put32(ia, client->iaid);
        size_t ia_len = 12;
        if (type != DHCP6_SOLICIT && !ipv6_addr_is_unspecified(&hint->addr)) {
            uint8_t addr[24] = { 0 };
            memcpy(addr, &hint->addr, sizeof(struct in6_addr));
            ia_len = dhcp6_put_option(ia, ia_len, DHCP6_OPT_IAADDR, addr, sizeof(addr));
        }
        n = dhcp6_put_option(out, n, DHCP6_OPT_IA_NA, ia, (uint16_t)ia_len);
    }
    if (type != DHCP6_RELEASE) {
        uint8_t oro[4];
        dhcp6_put16(oro, DHCP6_OPT_DNS_SERVERS);
        dhcp6_put16(oro + 2, DHCP6_OPT_INFO_REFRESH_TIME);
        n = dhcp6_put_option(out, n, DHCP6_OPT_ORO, oro, type == DHCP6_INFO_REQUEST ? 4 : 2);
    }
    return n;
}

static int dhcp6_send(const struct dhcp6_client* client, const uint8_t type)
{
    struct netif* nif = client->nif;
    const struct ipv6_ifaddr* ll = ipv6_addr_linklocal(nif);
    if (!ll || (ll->flags & (IPV6_ADDR_TENTATIVE | IPV6_ADDR_DUPLICATED)))
        return -EADDRNOTAVAIL;
    const struct in6_addr src = ll->addr;

    const size_t len = dhcp6_build(client, type, dhcp6_buf);
    struct netbuf* nb = netbuf_alloc_packet(len);
    if (!nb)
        return -ENOBUFS;
    if (!netbuf_append(nb, dhcp6_buf, len)) {
        netbuf_free_chain(nb);
        return -ENOBUFS;
    }
    nb = netbuf_prepend(nb, UDP_HEADER_LEN);
    if (!nb)
        return -ENOBUFS;
    const auto uh = (struct udp_header*)nb->data;
    uh->src_port = htons(DHCP6_CLIENT_PORT);
    uh->dst_port = htons(DHCP6_SERVER_PORT);
    uh->length = htons((uint16_t)nb->pkt_len);
    uh->checksum = 0;
    const uint16_t sum = inpcb_l4_checksum(nb, 0, nb->pkt_len, IPPROTO_UDP, &src, &dhcp6_all_servers);
    uh->checksum = htons(sum ? sum : 0xFFFF);

    const struct ipv6_options opts = { .nif = nif };
    return ipv6_output(nb, &src, &dhcp6_all_servers, IPPROTO_UDP, &opts, 0);
}

int dhcp6_send_solicit(struct dhcp6_client* client)
{
    return dhcp6_send(client, DHCP6_SOLICIT);
}

int dhcp6_send_request(struct dhcp6_client* client)
{
    return dhcp6_send(client, DHCP6_REQUEST);
}

int dhcp6_send_renew(struct dhcp6_client* client)
{
    return dhcp6_send(client, DHCP6_RENEW);
}

int dhcp6_send_rebind(struct dhcp6_client* client)
{
    return dhcp6_send(client, DHCP6_REBIND);
}

int dhcp6_send_release(struct dhcp6_client* client)
{
    return dhcp6_send(client, DHCP6_RELEASE);
}

int dhcp6_send_info_request(struct dhcp6_client* client)
{
    return dhcp6_send(client, DHCP6_INFO_REQUEST);
}

int dhcp6_apply_lease(struct dhcp6_client* client)
{
    struct dhcp6_lease* l = &client->lease;
    const uint64_t now = ticks_get();
    if (!l->t1)
        l->t1 = l->preferred_lifetime == DHCP6_INFINITE ? DHCP6_INFINITE : l->preferred_lifetime / 2;
    if (!l->t2)
        l->t2 = l->preferred_lifetime == DHCP6_INFINITE ? DHCP6_INFINITE
                                                        : (uint32_t)((uint64_t)l->preferred_lifetime * 4 / 5);
    client->t1_tick = dhcp6_seconds(l->t1, now);
    client->t2_tick = dhcp6_seconds(l->t2, now);
    client->expire_tick = dhcp6_seconds(l->valid_lifetime, now);
    const uint64_t preferred_until = dhcp6_seconds(l->preferred_lifetime, now);

    struct ipv6_ifaddr* ifa = ipv6_addr_find(&l->addr);
    if (ifa && ifa->nif == client->nif && (ifa->flags & IPV6_ADDR_DHCP6)) {
        ifa->preferred_until = preferred_until;
        ifa->valid_until = client->expire_tick;
        if (l->preferred_lifetime)
            ifa->flags &= ~IPV6_ADDR_DEPRECATED;
        client->configured = true;
        return 0;
    }

    const int rc = ipv6_addr_add(client->nif, &l->addr, 128, IPV6_ADDR_TENTATIVE | IPV6_ADDR_DHCP6,
                                 preferred_until, client->expire_tick);
    if (rc < 0)
        return rc;
    client->configured = true;

    return 0;
}

static void dhcp6_remove_lease(struct dhcp6_client* client)
{
    if (client->configured)
        ipv6_addr_del(client->nif, &client->lease.addr);
    client->configured = false;
    memset(&client->lease, 0, sizeof(client->lease));
}

static void dhcp6_arm(struct dhcp6_client* client, const uint64_t delay)
{
    callout_reset(&client->timer, delay ? delay : 1, dhcp6_timer, client);
}

static void dhcp6_begin(struct dhcp6_client* client, const enum dhcp6_state state, const uint32_t irt,
                        const uint32_t initial_delay_ms, const uint64_t deadline)
{
    client->state = state;
    client->xid = net_random32() & 0xFFFFFF;
    client->tries = 0;
    client->rt = irt;
    client->deadline = deadline;
    if (state == DHCP6_STATE_SOLICIT)
        client->have_advert = false;
    if (initial_delay_ms)
        dhcp6_arm(client, 1 + dhcp6_ms(net_random32() % initial_delay_ms));
    else
        dhcp6_transmit(client);
}

static void dhcp6_solicit(struct dhcp6_client* client)
{
    dhcp6_begin(client, client->stateful ? DHCP6_STATE_SOLICIT : DHCP6_STATE_INFO_REQUEST,
                client->stateful ? DHCP6_SOL_TIMEOUT : DHCP6_INF_TIMEOUT,
                client->stateful ? DHCP6_SOL_MAX_DELAY : DHCP6_INF_MAX_DELAY, 0);
}

static uint32_t dhcp6_max_rt(const enum dhcp6_state state)
{
    switch (state) {
    case DHCP6_STATE_SOLICIT: return DHCP6_SOL_MAX_RT;
    case DHCP6_STATE_REQUEST: return DHCP6_REQ_MAX_RT;
    case DHCP6_STATE_RENEW: return DHCP6_REN_MAX_RT;
    case DHCP6_STATE_REBIND: return DHCP6_REB_MAX_RT;
    default: return DHCP6_INF_MAX_RT;
    }
}

static uint8_t dhcp6_msg_for(const enum dhcp6_state state)
{
    switch (state) {
    case DHCP6_STATE_SOLICIT: return DHCP6_SOLICIT;
    case DHCP6_STATE_REQUEST: return DHCP6_REQUEST;
    case DHCP6_STATE_RENEW: return DHCP6_RENEW;
    case DHCP6_STATE_REBIND: return DHCP6_REBIND;
    default: return DHCP6_INFO_REQUEST;
    }
}

static void dhcp6_transmit(struct dhcp6_client* client)
{
    const uint64_t now = ticks_get();
    if (dhcp6_send(client, dhcp6_msg_for(client->state)) == -EADDRNOTAVAIL) {
        dhcp6_arm(client, dhcp6_ms(DHCP6_NOT_READY_MS));    // no link-local yet; try again
        return;
    }
    if (client->tries++ == 0) {
        client->exchange_start = now;
        client->rt = client->state == DHCP6_STATE_SOLICIT ? client->rt + client->rt * (net_random32() % 101) / 1000
                                                          : dhcp6_jitter(client->rt);
    } else {
        client->rt = dhcp6_jitter(client->rt * 2);
        const uint32_t mrt = dhcp6_max_rt(client->state);
        if (client->rt > mrt)
            client->rt = dhcp6_jitter(mrt);
    }
    uint64_t wait = dhcp6_ms(client->rt);
    if (client->deadline && now + wait > client->deadline)
        wait = client->deadline > now ? client->deadline - now : 1;
    dhcp6_arm(client, wait);
}

static void dhcp6_apply_dns(const struct dhcp6_client* client)
{
    const struct dhcp6_lease* l = &client->lease;
    if (!l->dns_count)
        return;
    struct sockaddr_storage list[DHCP6_MAX_DNS] = {0};
    for (uint8_t i = 0; i < l->dns_count; i++) {
        const auto sin6 = (struct sockaddr_in6*)&list[i];
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(DNS_PORT);
        sin6->sin6_addr = l->dns[i];
        sin6->sin6_scope_id = client->nif->index;
    }
    dns_set_servers(list, l->dns_count);
}

static void dhcp6_bound(struct dhcp6_client* client)
{
    client->state = DHCP6_STATE_BOUND;
    dhcp6_apply_dns(client);
    const uint64_t now = ticks_get();
    if (!client->stateful) {
        const uint32_t refresh = client->lease.refresh_time ? client->lease.refresh_time : DHCP6_IRT_DEFAULT_S;
        if (refresh != DHCP6_INFINITE)
            dhcp6_arm(client, dhcp6_ms((uint64_t)refresh * 1000));
        return;
    }
    if (client->t1_tick)
        dhcp6_arm(client, client->t1_tick > now ? client->t1_tick - now : 1);
}

void dhcp6_timer(void* arg)
{
    struct dhcp6_client* client = arg;
    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp6_lock, &flags);
    const uint64_t now = ticks_get();
    switch (client->state) {
    case DHCP6_STATE_SOLICIT:
        if (client->have_advert && client->tries) {
            dhcp6_begin(client, DHCP6_STATE_REQUEST, DHCP6_REQ_TIMEOUT, 0, 0);
            break;
        }
        dhcp6_transmit(client);
        break;
    case DHCP6_STATE_REQUEST:
        if (client->tries >= DHCP6_REQ_MAX_RC)
            dhcp6_solicit(client);
        else
            dhcp6_transmit(client);
        break;
    case DHCP6_STATE_BOUND:
        if (!client->stateful)
            dhcp6_solicit(client);
        else if (client->t2_tick && now >= client->t2_tick)
            dhcp6_begin(client, DHCP6_STATE_REBIND, DHCP6_REB_TIMEOUT, 0, client->expire_tick);
        else
            dhcp6_begin(client, DHCP6_STATE_RENEW, DHCP6_REN_TIMEOUT, 0, client->t2_tick);
        break;
    case DHCP6_STATE_RENEW:
        if (client->t2_tick && now >= client->t2_tick)
            dhcp6_begin(client, DHCP6_STATE_REBIND, DHCP6_REB_TIMEOUT, 0, client->expire_tick);
        else
            dhcp6_transmit(client);
        break;
    case DHCP6_STATE_REBIND:
        if (client->expire_tick && now >= client->expire_tick) {
            dhcp6_remove_lease(client);
            dhcp6_solicit(client);
        } else {
            dhcp6_transmit(client);
        }
        break;
    case DHCP6_STATE_INFO_REQUEST:
        dhcp6_transmit(client);
        break;
    default:
        break;
    }
    spinlock_release_irqrestore(&dhcp6_lock, flags);
}

static void dhcp6_advertise(struct dhcp6_client* client, struct dhcp6_lease* l, const struct dhcp6_ia_result* ia,
                            const int status)
{
    if (status != DHCP6_STATUS_SUCCESS || !ia->present || ia->status != DHCP6_STATUS_SUCCESS ||
        ipv6_addr_is_unspecified(&l->addr))
        return;
    if (!client->have_advert || l->preference > client->advert.preference) {
        client->advert = *l;
        client->have_advert = true;
    }

    if (l->preference == 255 || client->tries > 1) {
        callout_stop(&client->timer);
        dhcp6_begin(client, DHCP6_STATE_REQUEST, DHCP6_REQ_TIMEOUT, 0, 0);
    }
}

static void dhcp6_reply(struct dhcp6_client* client, struct dhcp6_lease* l, const struct dhcp6_ia_result* ia,
                        const int status)
{
    if (!client->stateful) {
        if (status != DHCP6_STATUS_SUCCESS)
            return;
        callout_stop(&client->timer);
        client->lease = *l;
        dhcp6_bound(client);
        return;
    }
    if (status == DHCP6_STATUS_USE_MULTICAST || status == DHCP6_STATUS_UNSPEC_FAIL)
        return;
    const bool renewing = client->state == DHCP6_STATE_RENEW || client->state == DHCP6_STATE_REBIND;
    if (!ia->present || ia->status != DHCP6_STATUS_SUCCESS || ipv6_addr_is_unspecified(&l->addr)) {
        if (renewing && ia->present && ia->status == DHCP6_STATUS_NO_BINDING) {
            callout_stop(&client->timer);
            client->advert = client->lease;
            dhcp6_begin(client, DHCP6_STATE_REQUEST, DHCP6_REQ_TIMEOUT, 0, 0);
        } else if (!renewing) {
            callout_stop(&client->timer);
            dhcp6_solicit(client);
        }
        return;
    }
    if (!l->valid_lifetime) {
        callout_stop(&client->timer);
        dhcp6_remove_lease(client);
        dhcp6_solicit(client);
        return;
    }
    if (client->configured && !ipv6_addr_equal(&l->addr, &client->lease.addr))
        dhcp6_remove_lease(client);
    callout_stop(&client->timer);
    client->lease = *l;
    if (dhcp6_apply_lease(client) < 0) {
        dhcp6_solicit(client);
        return;
    }
    dhcp6_bound(client);
}

void dhcp6_input(struct dhcp6_client* client, const uint8_t* msg, const size_t len)
{
    if (len < 4)
        return;
    const uint8_t type = msg[0];
    struct dhcp6_lease l;
    struct dhcp6_ia_result ia;
    const int status = dhcp6_parse(client, msg + 4, len - 4, &l, &ia);
    if (status < 0)
        return;

    if (type == DHCP6_ADVERTISE && client->state == DHCP6_STATE_SOLICIT)
        dhcp6_advertise(client, &l, &ia, status);
    else if (type == DHCP6_REPLY && client->state != DHCP6_STATE_SOLICIT && client->state != DHCP6_STATE_BOUND &&
             client->state != DHCP6_STATE_IDLE)
        dhcp6_reply(client, &l, &ia, status);
}

static struct dhcp6_client* dhcp6_find_client(const uint8_t* msg)
{
    const uint32_t xid = (uint32_t)msg[1] << 16 | (uint32_t)msg[2] << 8 | msg[3];
    for (const struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        struct dhcp6_client* client = nif->dhcp6_state;
        if (client && client->xid == xid)
            return client;
    }
    return nullptr;
}

static void dhcp6_rx(void* arg)
{
    (void)arg;
    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp6_lock, &flags);
    struct socket* so = dhcp6_socket;
    while (so) {
        const ssize_t n = sock_recv(so, dhcp6_buf, sizeof(dhcp6_buf), MSG_DONTWAIT, nullptr, nullptr);
        if (n < 0 || (n == 0 && !so->rcv.head))
            break;
        if (n < 4)
            continue;
        struct dhcp6_client* client = dhcp6_find_client(dhcp6_buf);
        if (client)
            dhcp6_input(client, dhcp6_buf, (size_t)n);
    }
    spinlock_release_irqrestore(&dhcp6_lock, flags);
}

static void dhcp6_upcall(struct socket* so, void* arg)
{
    (void)so;
    (void)arg;
    if (!callout_pending(&dhcp6_rx_callout))
        callout_reset(&dhcp6_rx_callout, 1, dhcp6_rx, nullptr);
}

static int dhcp6_open_socket()
{
    if (dhcp6_socket)
        return 0;
    struct socket* so;
    int err = sock_create(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, &so);
    if (err)
        return err;
    const struct sockaddr_in6 any = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(DHCP6_CLIENT_PORT),
    };
    err = sock_bind(so, (const struct sockaddr*)&any, sizeof(any));
    if (err) {
        sock_close(so);
        return err;
    }
    sock_set_upcall(so, dhcp6_upcall, nullptr);
    dhcp6_socket = so;
    return 0;
}

static bool dhcp6_any_client()
{
    for (const struct netif* nif = netif_first(); nif; nif = netif_next(nif))
        if (nif->dhcp6_state)
            return true;
    return false;
}

void dhcp6_initialize()
{
    spinlock_init(&dhcp6_lock, "dhcp6");
    callout_init(&dhcp6_rx_callout);
    dhcp6_socket = nullptr;
    dhcp6_ready = true;
}

int dhcp6_start(struct netif* nif, const bool stateful)
{
    if (!dhcp6_ready)
        return -ENXIO;
    if (!nif || !(nif->flags & NETIF_UP))
        return -ENETDOWN;
    const int err = dhcp6_open_socket();
    if (err)
        return err;

    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp6_lock, &flags);
    struct dhcp6_client* client = nif->dhcp6_state;
    if (client && client->state != DHCP6_STATE_IDLE && (client->stateful || !stateful)) {
        spinlock_release_irqrestore(&dhcp6_lock, flags);
        return -EALREADY;
    }
    if (!client) {
        client = malloc(sizeof(struct dhcp6_client));
        if (!client) {
            spinlock_release_irqrestore(&dhcp6_lock, flags);
            return -ENOMEM;
        }
        memset(client, 0, sizeof(struct dhcp6_client));
        client->nif = nif;
        client->socket = dhcp6_socket;
        dhcp6_make_duid(nif, client->duid, &client->duid_len);

        const uint8_t* hw = nif->hw_addr + nif->hw_addr_len - 4;
        client->iaid = dhcp6_get32(hw);
        callout_init(&client->timer);
        nif->dhcp6_state = client;
    }
    callout_stop(&client->timer);
    client->stateful = stateful;
    dhcp6_solicit(client);
    spinlock_release_irqrestore(&dhcp6_lock, flags);
    return 0;
}

void dhcp6_stop(struct netif* nif)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp6_lock, &flags);
    struct dhcp6_client* client = nif->dhcp6_state;
    if (!client) {
        spinlock_release_irqrestore(&dhcp6_lock, flags);
        return;
    }
    callout_stop(&client->timer);
    if (client->configured) {
        client->xid = net_random32() & 0xFFFFFF;
        client->tries = 0;
        dhcp6_send_release(client);
    }
    dhcp6_remove_lease(client);
    nif->dhcp6_state = nullptr;
    struct socket* so = nullptr;
    if (!dhcp6_any_client()) {
        so = dhcp6_socket;
        dhcp6_socket = nullptr;
        callout_stop(&dhcp6_rx_callout);
    }
    spinlock_release_irqrestore(&dhcp6_lock, flags);
    free(client);
    if (so)
        sock_close(so);
}
