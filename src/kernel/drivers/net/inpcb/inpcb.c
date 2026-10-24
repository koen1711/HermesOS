#include <drivers/net/inpcb/inpcb.h>

#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/checksum.h>
#include <drivers/net/core/random.h>
#include <drivers/net/socket/protosw.h>
#include <drivers/net/socket/socket.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

void inpcb_table_init(struct inpcb_table* table, const char* name)
{
    dlist_init(&table->all);
    for (int i = 0; i < INPCB_HASH_SIZE; i++) {
        dlist_init(&table->hash[i]);
        dlist_init(&table->port_hash[i]);
    }
    table->last_port = 0;
    spinlock_init(&table->lock, name);
    table->name = name;
}

bool inpcb_addr_equal(const struct in6_addr* a, const struct in6_addr* b)
{
    for (int i = 0; i < 4; i++)
        if (a->s6_addr32[i] != b->s6_addr32[i])
            return false;
    return true;
}

bool inpcb_addr_is_any(const struct in6_addr* addr)
{
    return !(addr->s6_addr32[0] | addr->s6_addr32[1] | addr->s6_addr32[2] | addr->s6_addr32[3]);
}

void inpcb_map_v4(const struct in_addr v4, struct in6_addr* out)
{
    out->s6_addr32[0] = 0;
    out->s6_addr32[1] = 0;
    out->s6_addr32[2] = v4.s_addr ? htonl(0x0000FFFF) : 0;
    out->s6_addr32[3] = v4.s_addr;
}

bool inpcb_is_v4_mapped(const struct in6_addr* addr)
{
    return addr->s6_addr32[0] == 0 && addr->s6_addr32[1] == 0 && addr->s6_addr32[2] == htonl(0x0000FFFF);
}

struct in_addr inpcb_unmap_v4(const struct in6_addr* addr)
{
    return (struct in_addr){ addr->s6_addr32[3] };
}

static uint32_t inpcb_port_hash(const uint16_t lport)
{
    return ntohs(lport) % INPCB_HASH_SIZE;
}

static uint32_t inpcb_hash(const struct in6_addr* faddr, const uint16_t fport,
                           const struct in6_addr* laddr, const uint16_t lport)
{
    uint32_t h = (uint32_t)fport << 16 | lport;
    for (int i = 0; i < 4; i++)
        h = (h ^ faddr->s6_addr32[i] ^ laddr->s6_addr32[i]) * 0x9E3779B1u;
    return (h >> 16) % INPCB_HASH_SIZE;
}

// Can an endpoint of `inp` receive traffic for `addr`?
static bool inpcb_family_ok(const struct inpcb* inp, const struct in6_addr* addr)
{
    if (inpcb_addr_is_any(addr))
        return true;
    if (inpcb_is_v4_mapped(addr))
        return !(inp->flags & INPCB_IPV6_ONLY);
    return inp->flags & INPCB_IPV6;
}

struct inpcb* inpcb_alloc(struct inpcb_table* table, struct socket* so)
{
    struct inpcb* inp = calloc(1, sizeof(struct inpcb));
    if (!inp)
        return nullptr;
    inp->table = table;
    inp->socket = so;
    inp->ttl = IPV4_DEFAULT_TTL;
    if (so && so->proto && so->proto->domain == AF_INET6)
        inp->flags |= INPCB_IPV6;
    dlist_insert_tail(&table->all, &inp->all_link);
    return inp;
}

void inpcb_free(struct inpcb* inp)
{
    inpcb_hash_remove(inp);
    dlist_remove(&inp->table->all, &inp->all_link);
    free(inp);
}

void inpcb_hash_insert(struct inpcb* inp)
{
    struct inpcb_table* t = inp->table;
    dlist_insert_tail(&t->hash[inpcb_hash(&inp->faddr, inp->fport, &inp->laddr, inp->lport)], &inp->hash_link);
    dlist_insert_tail(&t->port_hash[inpcb_port_hash(inp->lport)], &inp->port_link);
    inp->flags |= INPCB_INHASH;
}

void inpcb_hash_remove(struct inpcb* inp)
{
    if (!(inp->flags & INPCB_INHASH))
        return;
    struct inpcb_table* t = inp->table;
    dlist_remove(&t->hash[inpcb_hash(&inp->faddr, inp->fport, &inp->laddr, inp->lport)], &inp->hash_link);
    dlist_remove(&t->port_hash[inpcb_port_hash(inp->lport)], &inp->port_link);
    inp->flags &= ~INPCB_INHASH;
}

void inpcb_rehash(struct inpcb* inp)
{
    inpcb_hash_remove(inp);
    inpcb_hash_insert(inp);
}

static bool inpcb_families_collide(const struct inpcb* a, const struct inpcb* b)
{
    const bool a4 = !(a->flags & INPCB_IPV6_ONLY);
    const bool b4 = !(b->flags & INPCB_IPV6_ONLY);
    const bool a6 = a->flags & INPCB_IPV6;
    const bool b6 = b->flags & INPCB_IPV6;
    return (a4 && b4) || (a6 && b6);
}

// Could `other` and `inp` bound to `laddr` receive the same packet?
static bool inpcb_bindings_overlap(const struct inpcb* other, const struct inpcb* inp, const struct in6_addr* laddr)
{
    if (!inpcb_families_collide(other, inp))
        return false;
    return inpcb_addr_is_any(&other->laddr) || inpcb_addr_is_any(laddr) || inpcb_addr_equal(&other->laddr, laddr);
}

static bool inpcb_port_in_use(const struct inpcb* inp, const struct in6_addr* laddr, const uint16_t lport)
{
    const struct dlist* bucket = &inp->table->port_hash[inpcb_port_hash(lport)];
    for (struct dlist_node* n = dlist_first(bucket); n; n = dlist_next(n)) {
        const auto other = dlist_entry(n, struct inpcb, port_link);
        if (other == inp || other->lport != lport)
            continue;
        if ((inp->flags & INPCB_REUSEPORT) && (other->flags & INPCB_REUSEPORT))
            continue;
        if ((inp->flags & INPCB_REUSEADDR) && !inpcb_addr_is_any(&other->faddr))
            continue;
        if (inpcb_bindings_overlap(other, inp, laddr))
            return true;
    }
    return false;
}

int inpcb_alloc_ephemeral_port(struct inpcb* inp, uint16_t* out_port)
{
    constexpr uint32_t range = INPCB_PORT_LAST - INPCB_PORT_FIRST + 1;
    const uint32_t offset = net_random32();

    for (uint32_t i = 0; i < range; i++) {
        const uint16_t port = htons(INPCB_PORT_FIRST + (offset + i) % range);
        if (!inpcb_port_in_use(inp, &inp->laddr, port)) {
            inp->table->last_port = port;
            *out_port = port;
            return 0;
        }
    }
    return -EADDRINUSE;
}

int inpcb_sockaddr_to_in6(const struct inpcb* inp, const struct sockaddr* addr, const socklen_t len,
                          struct in6_addr* out, uint16_t* port)
{
    if (addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        if (inp->flags & INPCB_IPV6)
            return -EAFNOSUPPORT;
        const auto sin = (const struct sockaddr_in*)addr;
        inpcb_map_v4(sin->sin_addr, out);
        *port = sin->sin_port;
        return 0;
    }
    if (addr->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        if (!(inp->flags & INPCB_IPV6))
            return -EAFNOSUPPORT;
        const auto sin6 = (const struct sockaddr_in6*)addr;
        *out = sin6->sin6_addr;
        *port = sin6->sin6_port;
        if (inpcb_is_v4_mapped(out) && (inp->flags & INPCB_IPV6_ONLY))
            return -EAFNOSUPPORT;
        return 0;
    }
    return -EINVAL;
}

static bool inpcb_addr_is_local(const struct in6_addr* addr)
{
    if (inpcb_addr_is_any(addr))
        return true;
    if (inpcb_is_v4_mapped(addr))
        return ipv4_is_local(inpcb_unmap_v4(addr));
    return ipv6_is_local(addr);
}

int inpcb_bind(struct inpcb* inp, const struct sockaddr* addr, const socklen_t len)
{
    if (inp->lport)
        return -EINVAL;

    struct in6_addr laddr = {};
    uint16_t lport = 0;
    if (addr) {
        const int err = inpcb_sockaddr_to_in6(inp, addr, len, &laddr, &lport);
        if (err)
            return err;
        if (!inpcb_addr_is_local(&laddr))
            return -EADDRNOTAVAIL;
    }

    if (lport && inpcb_port_in_use(inp, &laddr, lport))
        return -EADDRINUSE;

    inp->laddr = laddr;
    if (!lport) {
        const int err = inpcb_alloc_ephemeral_port(inp, &lport);
        if (err) {
            inp->laddr = (struct in6_addr){};
            return err;
        }
    }
    inp->lport = lport;
    inpcb_hash_insert(inp);
    return 0;
}

int inpcb_select_source(const struct in6_addr* dst, struct in6_addr* out)
{
    if (inpcb_is_v4_mapped(dst)) {
        const struct in_addr src = ipv4_select_source(nullptr, inpcb_unmap_v4(dst));
        if (src.s_addr == INADDR_ANY)
            return -EHOSTUNREACH;
        inpcb_map_v4(src, out);
        return 0;
    }
    return ipv6_select_source(nullptr, dst, out) < 0 ? -EHOSTUNREACH : 0;
}

static void inpcb_default_faddr(const struct inpcb* inp, struct in6_addr* faddr)
{
    if (!inpcb_addr_is_any(faddr))
        return;
    if (inp->flags & INPCB_IPV6)
        *faddr = in6addr_loopback;
    else
        inpcb_map_v4((struct in_addr){ htonl(INADDR_LOOPBACK) }, faddr);
}

int inpcb_connect(struct inpcb* inp, const struct sockaddr* addr, const socklen_t len)
{
    struct in6_addr faddr;
    uint16_t fport;
    int err = inpcb_sockaddr_to_in6(inp, addr, len, &faddr, &fport);
    if (err)
        return err;
    if (!fport)
        return -EADDRNOTAVAIL;
    inpcb_default_faddr(inp, &faddr);

    struct in6_addr laddr = inp->laddr;
    if (inpcb_addr_is_any(&laddr) && ((err = inpcb_select_source(&faddr, &laddr))))
        return err;

    const struct inpcb* dup = inpcb_lookup(inp->table, &faddr, fport, &laddr, inp->lport, false);
    if (inp->lport && dup && dup != inp)
        return -EADDRINUSE;

    inpcb_hash_remove(inp);
    inp->laddr = laddr;
    if (!inp->lport && ((err = inpcb_alloc_ephemeral_port(inp, &inp->lport))))
        return err;
    inp->faddr = faddr;
    inp->fport = fport;
    inpcb_hash_insert(inp);
    return 0;
}

void inpcb_disconnect(struct inpcb* inp)
{
    inpcb_hash_remove(inp);
    inp->faddr = (struct in6_addr){};
    inp->fport = 0;
    if (inp->lport)
        inpcb_hash_insert(inp);
}

static struct inpcb* inpcb_lookup_exact(const struct inpcb_table* table,
                                        const struct in6_addr* faddr, const uint16_t fport,
                                        const struct in6_addr* laddr, const uint16_t lport)
{
    const struct dlist* bucket = &table->hash[inpcb_hash(faddr, fport, laddr, lport)];
    for (struct dlist_node* n = dlist_first(bucket); n; n = dlist_next(n)) {
        const auto inp = dlist_entry(n, struct inpcb, hash_link);
        if (inp->fport == fport && inp->lport == lport &&
            inpcb_addr_equal(&inp->faddr, faddr) && inpcb_addr_equal(&inp->laddr, laddr))
            return inp;
    }
    return nullptr;
}

static struct inpcb* inpcb_lookup_wild(const struct inpcb_table* table,
                                       const struct in6_addr* laddr, const uint16_t lport)
{
    struct inpcb* wild = nullptr;
    const struct dlist* bucket = &table->port_hash[inpcb_port_hash(lport)];
    for (struct dlist_node* n = dlist_first(bucket); n; n = dlist_next(n)) {
        const auto inp = dlist_entry(n, struct inpcb, port_link);
        if (inp->lport != lport || inp->fport || !inpcb_family_ok(inp, laddr))
            continue;
        if (inpcb_addr_equal(&inp->laddr, laddr))
            return inp;
        if (inpcb_addr_is_any(&inp->laddr) && !wild)
            wild = inp;
    }
    return wild;
}

struct inpcb* inpcb_lookup(struct inpcb_table* table,
                           const struct in6_addr* faddr, const uint16_t fport,
                           const struct in6_addr* laddr, const uint16_t lport,
                           const bool wildcard)
{
    struct inpcb* inp = inpcb_lookup_exact(table, faddr, fport, laddr, lport);
    if (!inp && wildcard)
        inp = inpcb_lookup_wild(table, laddr, lport);
    return inp;
}

static void inpcb_fill_sockaddr(const struct inpcb* inp, const struct in6_addr* addr, const uint16_t port,
                                struct sockaddr* out, socklen_t* len)
{
    struct sockaddr_storage ss = {};
    socklen_t n;
    if (inp->flags & INPCB_IPV6) {
        const auto sin6 = (struct sockaddr_in6*)&ss;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = port;
        sin6->sin6_addr = *addr;
        n = sizeof(struct sockaddr_in6);
    } else {
        const auto sin = (struct sockaddr_in*)&ss;
        sin->sin_family = AF_INET;
        sin->sin_port = port;
        sin->sin_addr = inpcb_unmap_v4(addr);
        n = sizeof(struct sockaddr_in);
    }
    memcpy(out, &ss, *len < n ? *len : n);
    *len = n;
}

void inpcb_get_sockname(const struct inpcb* inp, struct sockaddr* out, socklen_t* len)
{
    inpcb_fill_sockaddr(inp, &inp->laddr, inp->lport, out, len);
}

void inpcb_get_peername(const struct inpcb* inp, struct sockaddr* out, socklen_t* len)
{
    inpcb_fill_sockaddr(inp, &inp->faddr, inp->fport, out, len);
}

static int inpcb_pr_name(struct socket* so, struct sockaddr* addr, socklen_t* len, const bool peer)
{
    struct inpcb* inp = so->pcb;
    if (!inp)
        return -EINVAL;
    if (peer && !inp->fport)
        return -ENOTCONN;

    uint64_t flags;
    spinlock_acquire_irqsave(&inp->table->lock, &flags);
    if (peer)
        inpcb_get_peername(inp, addr, len);
    else
        inpcb_get_sockname(inp, addr, len);
    spinlock_release_irqrestore(&inp->table->lock, flags);
    return 0;
}

int inpcb_pr_sockaddr(struct socket* so, struct sockaddr* addr, socklen_t* len)
{
    return inpcb_pr_name(so, addr, len, false);
}

int inpcb_pr_peeraddr(struct socket* so, struct sockaddr* addr, socklen_t* len)
{
    return inpcb_pr_name(so, addr, len, true);
}

uint16_t inpcb_l4_checksum(const struct netbuf* nb, const size_t offset, const size_t len, const uint8_t proto,
                           const struct in6_addr* src, const struct in6_addr* dst)
{
    const uint32_t pseudo = inpcb_is_v4_mapped(dst)
        ? inet_pseudo_checksum4(inpcb_unmap_v4(src), inpcb_unmap_v4(dst), proto, len)
        : inet_pseudo_checksum6(src, dst, proto, len);
    uint32_t sum = (uint16_t)~inet_checksum_netbuf(nb, offset, len) + pseudo;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum & 0xFFFF;
}

int inpcb_ip_output(const struct inpcb* inp, struct netbuf* nb, const uint8_t proto,
                    const struct in6_addr* src, const struct in6_addr* dst)
{
    const uint8_t ttl = inp ? inp->ttl : IPV4_DEFAULT_TTL;
    const uint8_t tos = inp ? inp->tos : 0;

    if (inpcb_is_v4_mapped(dst)) {
        const struct ipv4_options opts = { .ttl = ttl, .tos = tos };
        const uint32_t flags = inp && (inp->flags & INPCB_BROADCAST) ? IPV4_OUTPUT_ALLOW_BCAST : 0;
        return ipv4_output(nb, inpcb_unmap_v4(src), inpcb_unmap_v4(dst), proto, &opts, flags);
    }
    const struct ipv6_options opts = { .hop_limit = ttl, .traffic_class = tos };
    return ipv6_output(nb, src, dst, proto, &opts, 0);
}
