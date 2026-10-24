#include <drivers/net/dhcp/dhcp.h>

#include <drivers/net/arp/arp.h>
#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/checksum.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/netif.h>
#include <drivers/net/core/random.h>
#include <drivers/net/core/route.h>
#include <drivers/net/dns/dns.h>
#include <drivers/net/inpcb/inpcb.h>
#include <drivers/net/socket/socket.h>
#include <drivers/net/udp/udp.h>
#include <hardware/memory/alloc.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#define DHCP_REQUEST_TRIES 4
#define DHCP_BACKOFF_MAX_S 64
#define DHCP_RETRY_MIN_S   60
#define DHCP_DECLINE_WAIT_S 10
#define DHCP_MAX_MSG_SIZE  576

static spinlock_t dhcp_lock;
static struct socket* dhcp_socket;
static struct callout dhcp_rx_callout;
static struct dhcp_message dhcp_msg_buf;

static void dhcp_transmit(struct dhcp_client* client);

static uint64_t dhcp_seconds(const uint64_t s)
{
    return ticks_from_ms(s * 1000);
}

static uint8_t dhcp_mask_to_prefix(const struct in_addr mask)
{
    uint32_t m = ntohl(mask.s_addr);
    uint8_t n = 0;
    while (m & 0x80000000u) {
        n++;
        m <<= 1;
    }
    return n;
}

static struct in_addr dhcp_classful_mask(const struct in_addr addr)
{
    const uint32_t a = ntohl(addr.s_addr);
    const uint32_t m = a < 0x80000000u ? 0xFF000000u : a < 0xC0000000u ? 0xFFFF0000u : 0xFFFFFF00u;
    return (struct in_addr){ htonl(m) };
}

static uint32_t dhcp_get32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static struct in_addr dhcp_get_addr(const uint8_t* p)
{
    struct in_addr a;
    memcpy(&a, p, sizeof(a));
    return a;
}

static void dhcp_arm(struct dhcp_client* client, const uint64_t delay)
{
    callout_reset(&client->timer, delay ? delay : 1, dhcp_timer, client);
}

static uint64_t dhcp_backoff(const uint8_t tries)
{
    uint64_t s = 4;
    for (uint8_t i = 0; i < tries && s < DHCP_BACKOFF_MAX_S; i++)
        s *= 2;
    const uint64_t second = dhcp_seconds(1);
    return dhcp_seconds(s) - second + net_random32() % (2 * second + 1);
}

static uint64_t dhcp_retry_before(const uint64_t now, const uint64_t deadline)
{
    if (deadline <= now)
        return 1;
    const uint64_t left = deadline - now;
    uint64_t wait = left / 2;
    if (wait < dhcp_seconds(DHCP_RETRY_MIN_S))
        wait = dhcp_seconds(DHCP_RETRY_MIN_S);
    return wait < left ? wait : left;
}

static int dhcp_parse_area(const uint8_t* p, const size_t len, struct dhcp_lease* lease, int* type, uint8_t* overload)
{
    size_t i = 0;
    while (i < len) {
        const uint8_t code = p[i++];
        if (code == DHCP_OPT_PAD)
            continue;
        if (code == DHCP_OPT_END)
            return 0;
        if (i >= len)
            return -EINVAL;
        const uint8_t olen = p[i++];
        if (olen > len - i)
            return -EINVAL;
        const uint8_t* v = p + i;
        i += olen;

        switch (code) {
        case DHCP_OPT_MSG_TYPE:
            if (olen == 1)
                *type = v[0];
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (olen == 4)
                lease->netmask = dhcp_get_addr(v);
            break;
        case DHCP_OPT_ROUTER:
            if (olen >= 4)
                lease->router = dhcp_get_addr(v);
            break;
        case DHCP_OPT_DNS:
            for (size_t k = 0; k + 4 <= olen && lease->dns_count < DHCP_MAX_DNS; k += 4)
                lease->dns[lease->dns_count++] = dhcp_get_addr(v + k);
            break;
        case DHCP_OPT_SERVER_ID:
            if (olen == 4)
                lease->server_id = dhcp_get_addr(v);
            break;
        case DHCP_OPT_LEASE_TIME:
            if (olen == 4)
                lease->lease_time = dhcp_get32(v);
            break;
        case DHCP_OPT_RENEWAL_T1:
            if (olen == 4)
                lease->t1 = dhcp_get32(v);
            break;
        case DHCP_OPT_REBIND_T2:
            if (olen == 4)
                lease->t2 = dhcp_get32(v);
            break;
        case DHCP_OPT_OVERLOAD:
            if (olen == 1 && overload)
                *overload = v[0];
            break;
        default:
            break;
        }
    }
    return 0;
}

int dhcp_parse_options(const struct dhcp_message* msg, const size_t len, struct dhcp_lease* lease)
{
    if (len < DHCP_FIXED_LEN || len > sizeof(struct dhcp_message) || ntohl(msg->magic) != DHCP_MAGIC_COOKIE)
        return -EINVAL;
    memset(lease, 0, sizeof(struct dhcp_lease));
    lease->addr = msg->yiaddr;

    int type = -1;
    uint8_t overload = 0;
    if (dhcp_parse_area(msg->options, len - DHCP_FIXED_LEN, lease, &type, &overload) < 0)
        return -EINVAL;

    if (overload & 1 && dhcp_parse_area((const uint8_t*)msg->file, sizeof(msg->file), lease, &type, nullptr) < 0)
        return -EINVAL;
    if (overload & 2 && dhcp_parse_area((const uint8_t*)msg->sname, sizeof(msg->sname), lease, &type, nullptr) < 0)
        return -EINVAL;
    if (type < 0)
        return -EINVAL;

    if (!lease->netmask.s_addr && lease->addr.s_addr)
        lease->netmask = dhcp_classful_mask(lease->addr);

    if (lease->lease_time != DHCP_INFINITE &&
        (!lease->t1 || !lease->t2 || lease->t1 >= lease->t2 || lease->t2 >= lease->lease_time)) {
        lease->t1 = lease->lease_time / 2;
        lease->t2 = (uint32_t)((uint64_t)lease->lease_time * 7 / 8);
    }
    return type;
}

static size_t dhcp_put_option(uint8_t* out, size_t pos, const size_t out_len, const uint8_t code,
                              const void* value, const uint8_t len)
{
    if (pos + 2 + len + 1 > out_len)
        return pos;
    out[pos++] = code;
    out[pos++] = len;
    memcpy(out + pos, value, len);
    return pos + len;
}

size_t dhcp_build_options(const struct dhcp_client* client, const uint8_t msg_type, uint8_t* out, const size_t out_len)
{
    const struct netif* nif = client->nif;
    size_t n = 0;
    n = dhcp_put_option(out, n, out_len, DHCP_OPT_MSG_TYPE, &msg_type, 1);

    uint8_t client_id[1 + NETIF_HWADDR_LEN] = { 1 };
    memcpy(client_id + 1, nif->hw_addr, nif->hw_addr_len);
    n = dhcp_put_option(out, n, out_len, DHCP_OPT_CLIENT_ID, client_id, 1 + nif->hw_addr_len);

    const struct dhcp_lease* ask = nullptr;
    if (msg_type == DHCP_MSG_REQUEST && client->state == DHCP_STATE_REQUESTING)
        ask = &client->offer;
    else if (msg_type == DHCP_MSG_DECLINE)
        ask = &client->lease;
    if (ask) {
        n = dhcp_put_option(out, n, out_len, DHCP_OPT_REQUESTED_IP, &ask->addr, 4);
        n = dhcp_put_option(out, n, out_len, DHCP_OPT_SERVER_ID, &ask->server_id, 4);
    }
    if (msg_type == DHCP_MSG_RELEASE)
        n = dhcp_put_option(out, n, out_len, DHCP_OPT_SERVER_ID, &client->lease.server_id, 4);

    if (msg_type == DHCP_MSG_DISCOVER || msg_type == DHCP_MSG_REQUEST) {
        const uint16_t max_size = htons(DHCP_MAX_MSG_SIZE);
        n = dhcp_put_option(out, n, out_len, DHCP_OPT_MAX_MSG_SIZE, &max_size, 2);
        static constexpr uint8_t params[] = {
            DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS, DHCP_OPT_DOMAIN_NAME,
            DHCP_OPT_BROADCAST, DHCP_OPT_LEASE_TIME, DHCP_OPT_RENEWAL_T1, DHCP_OPT_REBIND_T2,
        };
        n = dhcp_put_option(out, n, out_len, DHCP_OPT_PARAM_LIST, params, sizeof(params));
    }
    if (n < out_len)
        out[n++] = DHCP_OPT_END;
    return n;
}

static bool dhcp_is_unicast(const struct dhcp_client* client, const uint8_t msg_type)
{
    return msg_type == DHCP_MSG_RELEASE || (msg_type == DHCP_MSG_REQUEST && client->state == DHCP_STATE_RENEWING);
}

static size_t dhcp_build(const struct dhcp_client* client, const uint8_t msg_type, struct dhcp_message* m)
{
    const struct netif* nif = client->nif;
    memset(m, 0, sizeof(struct dhcp_message));
    m->op = DHCP_OP_BOOTREQUEST;
    m->htype = 1;
    m->hlen = nif->hw_addr_len;
    m->xid = client->xid;
    const uint64_t secs = (ticks_get() - client->start_tick) / dhcp_seconds(1);
    if (msg_type != DHCP_MSG_RELEASE && msg_type != DHCP_MSG_DECLINE)
        m->secs = htons(secs > 0xFFFF ? 0xFFFF : (uint16_t)secs);

    const bool renewing = msg_type == DHCP_MSG_REQUEST &&
        (client->state == DHCP_STATE_RENEWING || client->state == DHCP_STATE_REBINDING);
    if (client->configured && (renewing || msg_type == DHCP_MSG_RELEASE))
        m->ciaddr = client->lease.addr;
    else if (!client->configured)
        m->flags = htons(DHCP_FLAG_BROADCAST);
    memcpy(m->chaddr, nif->hw_addr, nif->hw_addr_len);
    m->magic = htonl(DHCP_MAGIC_COOKIE);

    const size_t len = DHCP_FIXED_LEN + dhcp_build_options(client, msg_type, m->options, sizeof(m->options));
    return len < DHCP_MIN_LEN ? DHCP_MIN_LEN : len;
}

static int dhcp_output_broadcast(const struct dhcp_client* client, const struct dhcp_message* m, const size_t len)
{
    struct netif* nif = client->nif;
    const struct in_addr src = client->configured ? client->lease.addr : (struct in_addr){ INADDR_ANY };
    const struct in_addr dst = { INADDR_BROADCAST };

    struct netbuf* nb = netbuf_alloc_packet(len);
    if (!nb)
        return -ENOBUFS;
    if (!netbuf_append(nb, m, len)) {
        netbuf_free_chain(nb);
        return -ENOBUFS;
    }

    nb = netbuf_prepend(nb, UDP_HEADER_LEN);
    if (!nb)
        return -ENOBUFS;
    const auto uh = (struct udp_header*)nb->data;
    uh->src_port = htons(DHCP_CLIENT_PORT);
    uh->dst_port = htons(DHCP_SERVER_PORT);
    uh->length = htons((uint16_t)nb->pkt_len);
    uh->checksum = 0;
    struct in6_addr src6, dst6;
    inpcb_map_v4(src, &src6);
    inpcb_map_v4(dst, &dst6);
    const uint16_t sum = inpcb_l4_checksum(nb, 0, nb->pkt_len, IPPROTO_UDP, &src6, &dst6);
    uh->checksum = htons(sum ? sum : 0xFFFF);

    nb = netbuf_prepend(nb, IPV4_HEADER_LEN);
    if (!nb)
        return -ENOBUFS;
    const auto ip = (struct ipv4_header*)nb->data;
    memset(ip, 0, IPV4_HEADER_LEN);
    ip->ver_ihl = (IPV4_VERSION << 4) | (IPV4_HEADER_LEN / 4);
    ip->total_len = htons((uint16_t)nb->pkt_len);
    ip->id = htons((uint16_t)net_random32());
    ip->ttl = IPV4_DEFAULT_TTL;
    ip->protocol = IPPROTO_UDP;
    ip->src = src;
    ip->dst = dst;
    ip->checksum = htons(inet_checksum(ip, IPV4_HEADER_LEN));
    nb->l3_offset = 0;
    nb->l4_offset = IPV4_HEADER_LEN;
    return nif->output(nif, nb, &dst, AF_INET);
}

static int dhcp_output_unicast(const struct dhcp_client* client, const struct dhcp_message* m, const size_t len)
{
    const struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(DHCP_SERVER_PORT),
        .sin_addr = client->lease.server_id,
    };
    const ssize_t n = sock_send(client->socket, m, len, MSG_DONTWAIT, (const struct sockaddr*)&to, sizeof(to));
    return n < 0 ? (int)n : 0;
}

static int dhcp_send(const struct dhcp_client* client, const uint8_t msg_type)
{
    struct dhcp_message* m = &dhcp_msg_buf;
    const size_t len = dhcp_build(client, msg_type, m);
    return dhcp_is_unicast(client, msg_type) ? dhcp_output_unicast(client, m, len)
                                             : dhcp_output_broadcast(client, m, len);
}

int dhcp_send_discover(const struct dhcp_client* client)
{
    return dhcp_send(client, DHCP_MSG_DISCOVER);
}

int dhcp_send_request(struct dhcp_client* client)
{
    client->request_tick = ticks_get();
    return dhcp_send(client, DHCP_MSG_REQUEST);
}

int dhcp_send_decline(const struct dhcp_client* client)
{
    return dhcp_send(client, DHCP_MSG_DECLINE);
}

int dhcp_send_release(const struct dhcp_client* client)
{
    return dhcp_send(client, DHCP_MSG_RELEASE);
}

static void dhcp_apply_dns(const struct dhcp_lease* l)
{
    if (!l->dns_count)
        return;
    struct sockaddr_storage list[DHCP_MAX_DNS];
    memset(list, 0, sizeof(list));
    for (uint8_t i = 0; i < l->dns_count; i++) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&list[i];
        sin->sin_family = AF_INET;
        sin->sin_port = htons(DNS_PORT);
        sin->sin_addr = l->dns[i];
    }
    dns_set_servers(list, l->dns_count);
}

int dhcp_apply_lease(struct dhcp_client* client)
{
    struct netif* nif = client->nif;
    const struct dhcp_lease* l = &client->lease;
    const uint64_t base = client->request_tick;
    if (l->lease_time == DHCP_INFINITE) {
        client->t1_tick = client->t2_tick = client->expire_tick = 0;
    } else {
        client->t1_tick = base + dhcp_seconds(l->t1);
        client->t2_tick = base + dhcp_seconds(l->t2);
        client->expire_tick = base + dhcp_seconds(l->lease_time);
    }

    dhcp_apply_dns(l);
    if (client->configured) {
        ipv4_addr_set_expiry(l->addr, client->expire_tick);
        return 0;
    }

    const int rc = ipv4_addr_add(nif, l->addr, dhcp_mask_to_prefix(l->netmask), client->expire_tick);
    if (rc < 0)
        return rc;
    client->configured = true;
    if (l->router.s_addr)
        client->default_route = route_set_default4(l->router, nif) == 0;
    arp_announce(nif, l->addr);
    return 0;
}

void dhcp_remove_lease(struct dhcp_client* client)
{
    if (!client->configured)
        return;
    if (client->default_route)
        route_del4((struct in_addr){ INADDR_ANY }, 0);
    ipv4_addr_del(client->nif, client->lease.addr);
    client->default_route = false;
    client->configured = false;
}

static void dhcp_discover(struct dhcp_client* client)
{
    client->state = DHCP_STATE_SELECTING;
    client->xid = net_random32();
    client->start_tick = ticks_get();
    client->tries = 0;
    memset(&client->offer, 0, sizeof(client->offer));
    dhcp_transmit(client);
}

static void dhcp_new_exchange(struct dhcp_client* client, const enum dhcp_state state)
{
    client->state = state;
    client->xid = net_random32();
    client->start_tick = ticks_get();
    client->tries = 0;
    dhcp_transmit(client);
}

static void dhcp_transmit(struct dhcp_client* client)
{
    const uint64_t now = ticks_get();
    switch (client->state) {
    case DHCP_STATE_SELECTING:
        dhcp_send_discover(client);
        dhcp_arm(client, dhcp_backoff(client->tries++));
        break;
    case DHCP_STATE_REQUESTING:
        if (client->tries >= DHCP_REQUEST_TRIES) {
            dhcp_discover(client);
            break;
        }
        dhcp_send_request(client);
        dhcp_arm(client, dhcp_backoff(client->tries++));
        break;
    case DHCP_STATE_RENEWING:
        dhcp_send_request(client);
        dhcp_arm(client, dhcp_retry_before(now, client->t2_tick));
        break;
    case DHCP_STATE_REBINDING:
        dhcp_send_request(client);
        dhcp_arm(client, dhcp_retry_before(now, client->expire_tick));
        break;
    default:
        break;
    }
}

void dhcp_timer(void* arg)
{
    struct dhcp_client* client = arg;
    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp_lock, &flags);

    if (client->nif->dhcp_state != client) {
        spinlock_release_irqrestore(&dhcp_lock, flags);
        return;
    }
    const uint64_t now = ticks_get();
    switch (client->state) {
    case DHCP_STATE_INIT:
        dhcp_discover(client);
        break;
    case DHCP_STATE_SELECTING:
    case DHCP_STATE_REQUESTING:
        dhcp_transmit(client);
        break;
    case DHCP_STATE_BOUND:
        if (now >= client->t1_tick)
            dhcp_new_exchange(client, DHCP_STATE_RENEWING);
        else
            dhcp_arm(client, client->t1_tick - now);
        break;
    case DHCP_STATE_RENEWING:
        if (now >= client->t2_tick)
            dhcp_new_exchange(client, DHCP_STATE_REBINDING);
        else
            dhcp_transmit(client);
        break;
    case DHCP_STATE_REBINDING:
        if (now >= client->expire_tick) {
            dhcp_remove_lease(client);
            dhcp_discover(client);
        } else {
            dhcp_transmit(client);
        }
        break;
    default:
        break;
    }
    spinlock_release_irqrestore(&dhcp_lock, flags);
}

static void dhcp_bound(struct dhcp_client* client)
{
    client->state = DHCP_STATE_BOUND;
    if (client->lease.lease_time == DHCP_INFINITE)
        return;
    const uint64_t now = ticks_get();
    dhcp_arm(client, client->t1_tick > now ? client->t1_tick - now : 1);
}

static void dhcp_ack(struct dhcp_client* client, struct dhcp_lease* l)
{
    if (!l->addr.s_addr || !l->lease_time)
        return;
    if (client->state == DHCP_STATE_REQUESTING) {
        if (l->server_id.s_addr && l->server_id.s_addr != client->offer.server_id.s_addr)
            return;
        l->server_id = client->offer.server_id;
    } else if (!l->server_id.s_addr) {
        l->server_id = client->lease.server_id;
    }

    const struct dhcp_lease* cur = &client->lease;
    if (client->configured && (l->addr.s_addr != cur->addr.s_addr || l->netmask.s_addr != cur->netmask.s_addr ||
                               l->router.s_addr != cur->router.s_addr))
        dhcp_remove_lease(client);

    client->lease = *l;
    callout_stop(&client->timer);
    if (dhcp_apply_lease(client) < 0) {
        dhcp_send_decline(client);
        memset(&client->lease, 0, sizeof(client->lease));
        client->state = DHCP_STATE_INIT;
        dhcp_arm(client, dhcp_seconds(DHCP_DECLINE_WAIT_S));
        return;
    }
    dhcp_bound(client);
}

void dhcp_input(struct dhcp_client* client, const struct dhcp_message* msg, const size_t len)
{
    struct dhcp_lease l;
    const int type = dhcp_parse_options(msg, len, &l);
    if (type < 0)
        return;

    switch (client->state) {
    case DHCP_STATE_SELECTING:
        if (type != DHCP_MSG_OFFER || !l.server_id.s_addr || !l.addr.s_addr ||
            l.addr.s_addr == INADDR_BROADCAST || ipv4_is_multicast(l.addr))
            return;
        client->offer = l;
        client->state = DHCP_STATE_REQUESTING;
        client->tries = 0;
        dhcp_transmit(client);
        break;
    case DHCP_STATE_REQUESTING:
    case DHCP_STATE_RENEWING:
    case DHCP_STATE_REBINDING:
        if (type == DHCP_MSG_NAK) {
            callout_stop(&client->timer);
            dhcp_remove_lease(client);
            memset(&client->lease, 0, sizeof(client->lease));
            dhcp_discover(client);
        } else if (type == DHCP_MSG_ACK) {
            dhcp_ack(client, &l);
        }
        break;
    default:
        break;
    }
}

static struct dhcp_client* dhcp_find_client(struct dhcp_message* msg)
{
    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        struct dhcp_client* client = nif->dhcp_state;
        if (client && client->xid == msg->xid && msg->hlen == nif->hw_addr_len &&
            !memcmp(msg->chaddr, nif->hw_addr, nif->hw_addr_len)) {
            netif_rele(nif);
            return client;
        }
    }
    return nullptr;
}

static void dhcp_rx(void* arg)
{
    (void)arg;
    struct dhcp_message* msg = &dhcp_msg_buf;
    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp_lock, &flags);
    struct socket* so = dhcp_socket;
    while (so) {
        const ssize_t n = sock_recv(so, msg, sizeof(struct dhcp_message), MSG_DONTWAIT, nullptr, nullptr);
        if (n < 0 || (n == 0 && !so->rcv.head))
            break;
        if ((size_t)n < DHCP_FIXED_LEN || msg->op != DHCP_OP_BOOTREPLY)
            continue;
        struct dhcp_client* client = dhcp_find_client(msg);
        if (client)
            dhcp_input(client, msg, (size_t)n);
    }
    spinlock_release_irqrestore(&dhcp_lock, flags);
}

static void dhcp_upcall(struct socket* so, void* arg)
{
    (void)so;
    (void)arg;
    if (!callout_pending(&dhcp_rx_callout))
        callout_reset(&dhcp_rx_callout, 1, dhcp_rx, nullptr);
}

static int dhcp_open_socket()
{
    if (dhcp_socket)
        return 0;
    struct socket* so;
    int err = sock_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &so);
    if (err)
        return err;
    const struct sockaddr_in any = {
        .sin_family = AF_INET,
        .sin_port = htons(DHCP_CLIENT_PORT),
        .sin_addr = { INADDR_ANY },
    };
    err = sock_bind(so, (const struct sockaddr*)&any, sizeof(any));
    if (err) {
        sock_close(so);
        return err;
    }
    sock_set_upcall(so, dhcp_upcall, nullptr);
    dhcp_socket = so;
    return 0;
}

static bool dhcp_any_client()
{
    for (struct netif* nif = netif_first(); nif; nif = netif_next(nif)) {
        if (nif->dhcp_state) {
            netif_rele(nif);
            return true;
        }
    }
    return false;
}

void dhcp_initialize()
{
    spinlock_init(&dhcp_lock, "dhcp");
    callout_init(&dhcp_rx_callout);
    dhcp_socket = nullptr;
}

int dhcp_start(struct netif* nif)
{
    if (!nif)
        return -EINVAL;
    if (!(nif->flags & NETIF_UP))
        return -ENETDOWN;
    const int err = dhcp_open_socket();
    if (err)
        return err;

    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp_lock, &flags);
    struct dhcp_client* client = nif->dhcp_state;
    if (client && client->state != DHCP_STATE_INIT) {
        spinlock_release_irqrestore(&dhcp_lock, flags);
        return -EALREADY;
    }
    if (!client) {
        client = malloc(sizeof(struct dhcp_client));
        if (!client) {
            spinlock_release_irqrestore(&dhcp_lock, flags);
            return -ENOMEM;
        }
        memset(client, 0, sizeof(struct dhcp_client));
        client->nif = nif;
        client->socket = dhcp_socket;
        callout_init(&client->timer);
        nif->dhcp_state = client;
    }
    callout_stop(&client->timer);
    dhcp_discover(client);
    spinlock_release_irqrestore(&dhcp_lock, flags);
    return 0;
}

void dhcp_stop(struct netif* nif)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp_lock, &flags);
    struct dhcp_client* client = nif->dhcp_state;
    if (!client) {
        spinlock_release_irqrestore(&dhcp_lock, flags);
        return;
    }
    callout_stop(&client->timer);
    dhcp_remove_lease(client);
    nif->dhcp_state = nullptr;
    struct socket* so = nullptr;
    if (!dhcp_any_client()) {
        so = dhcp_socket;
        dhcp_socket = nullptr;
        callout_stop(&dhcp_rx_callout);
    }
    spinlock_release_irqrestore(&dhcp_lock, flags);
    callout_drain(&client->timer);
    free(client);
    if (so)
        sock_close(so);
}

int dhcp_release(const struct netif* nif)
{
    uint64_t flags;
    spinlock_acquire_irqsave(&dhcp_lock, &flags);
    struct dhcp_client* client = nif->dhcp_state;
    int rc = -ENXIO;
    if (client && !client->configured) {
        rc = -EADDRNOTAVAIL;
    } else if (client) {
        callout_stop(&client->timer);
        client->xid = net_random32();
        rc = dhcp_send_release(client);
        dhcp_remove_lease(client);
        memset(&client->lease, 0, sizeof(client->lease));
        client->state = DHCP_STATE_INIT;
    }
    spinlock_release_irqrestore(&dhcp_lock, flags);
    return rc;
}
