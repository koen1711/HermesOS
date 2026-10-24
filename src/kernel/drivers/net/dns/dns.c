#include <drivers/net/dns/dns.h>

#include <drivers/net/core/byteorder.h>
#include <drivers/net/core/random.h>
#include <drivers/net/ipv4/ipv4.h>
#include <drivers/net/ipv6/ipv6.h>
#include <drivers/net/socket/socket.h>
#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <utils/sync/spinlock.h>
#include <os/errno.h>

#define DNS_HEADER_LEN   12
#define DNS_MAX_LABEL    63
#define DNS_MAX_HOPS     64
#define DNS_ROUNDS       2
#define DNS_MIN_TRY_MS   200
#define DNS_CACHE_RESULTS 4
#define DNS_MAX_TTL_S    86400

enum dns_rcode
{
    DNS_RCODE_NOERROR = 0,
    DNS_RCODE_FORMERR = 1,
    DNS_RCODE_SERVFAIL = 2,
    DNS_RCODE_NXDOMAIN = 3
};

#define DNS_FLAG_TC 0x0200

struct dns_cache_entry {
    char name[DNS_MAX_NAME + 1];
    sa_family_t family;
    struct dns_result results[DNS_CACHE_RESULTS];
    uint8_t count;
    uint64_t expires;
};

static spinlock_t dns_lock;
static struct sockaddr_storage servers[DNS_MAX_SERVERS];
static size_t server_count;
static struct dns_cache_entry cache[DNS_CACHE_SIZE];

void dns_initialize()
{
    spinlock_init(&dns_lock, "dns");
    server_count = 0;
    memset(cache, 0, sizeof(cache));
}

static char dns_lower(const char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
}

static bool dns_name_eq(const char* a, const char* b)
{
    while (*a && *b && dns_lower(*a) == dns_lower(*b)) {
        a++;
        b++;
    }
    if (*a == '.' && !a[1])
        a++;
    if (*b == '.' && !b[1])
        b++;
    return !*a && !*b;
}

static size_t dns_strlen(const char* s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void dns_strcpy(char* dst, const char* src, const size_t max)
{
    size_t i = 0;
    for (; src[i] && i + 1 < max; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static uint16_t dns_get16(const uint8_t* p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t dns_get32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int dns_set_servers(const struct sockaddr_storage* list, const size_t count)
{
    if (count > DNS_MAX_SERVERS)
        return -EINVAL;
    for (size_t i = 0; i < count; i++)
        if (list[i].ss_family != AF_INET && list[i].ss_family != AF_INET6)
            return -EAFNOSUPPORT;
    uint64_t flags;
    spinlock_acquire_irqsave(&dns_lock, &flags);
    for (size_t i = 0; i < count; i++) {
        servers[i] = list[i];
        if (servers[i].ss_family == AF_INET && !((struct sockaddr_in*)&servers[i])->sin_port)
            ((struct sockaddr_in*)&servers[i])->sin_port = htons(DNS_PORT);
        if (servers[i].ss_family == AF_INET6 && !((struct sockaddr_in6*)&servers[i])->sin6_port)
            ((struct sockaddr_in6*)&servers[i])->sin6_port = htons(DNS_PORT);
    }
    server_count = count;
    spinlock_release_irqrestore(&dns_lock, flags);
    return 0;
}

static size_t dns_encode_name(const char* name, uint8_t* out, const size_t out_len)
{
    size_t n = 0;
    const char* p = name;
    if (p[0] == '.' && !p[1])
        p++;
    while (*p) {
        const char* end = p;
        while (*end && *end != '.')
            end++;
        const size_t label = (size_t)(end - p);
        if (label == 0 || label > DNS_MAX_LABEL || n + 1 + label + 1 > out_len)
            return 0;
        out[n++] = (uint8_t)label;
        memcpy(out + n, p, label);
        n += label;
        p = *end ? end + 1 : end;
    }
    if (n + 1 > out_len || n + 1 > DNS_MAX_NAME)
        return 0;
    out[n++] = 0;
    return n;
}

size_t dns_build_query(const uint16_t id, const char* name, const uint16_t qtype, uint8_t* out, const size_t out_len)
{
    if (out_len < DNS_HEADER_LEN + 1 + 4)
        return 0;
    memset(out, 0, DNS_HEADER_LEN);
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)id;
    out[2] = DNS_FLAG_RD >> 8;
    out[5] = 1;
    const size_t nlen = dns_encode_name(name, out + DNS_HEADER_LEN, out_len - DNS_HEADER_LEN - 4);
    if (!nlen)
        return 0;
    uint8_t* q = out + DNS_HEADER_LEN + nlen;
    q[0] = (uint8_t)(qtype >> 8);
    q[1] = (uint8_t)qtype;
    q[2] = 0;
    q[3] = DNS_CLASS_IN;
    return DNS_HEADER_LEN + nlen + 4;
}

int dns_decode_name(const uint8_t* msg, const size_t len, const size_t offset, char* out, const size_t out_len)
{
    size_t pos = offset, n = 0;
    int consumed = -1;
    int hops = 0;
    for (;;) {
        if (pos >= len)
            return -EINVAL;
        const uint8_t c = msg[pos];
        if ((c & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++hops > DNS_MAX_HOPS)
                return -EINVAL;
            const size_t target = (size_t)(c & 0x3F) << 8 | msg[pos + 1];
            if (target >= pos)
                return -EINVAL;
            if (consumed < 0)
                consumed = (int)(pos + 2 - offset);
            pos = target;
            continue;
        }
        if (c & 0xC0)
            return -EINVAL;
        if (c == 0) {
            if (consumed < 0)
                consumed = (int)(pos + 1 - offset);
            break;
        }
        if (pos + 1 + c > len)
            return -EINVAL;
        if (out) {
            if (n + (n ? 1 : 0) + c + 1 > out_len)
                return -ENAMETOOLONG;
            if (n)
                out[n++] = '.';
            memcpy(out + n, msg + pos + 1, c);
        } else if (n + c + 1 > DNS_MAX_NAME) {
            return -ENAMETOOLONG;
        }
        n += c;
        pos += 1 + c;
    }
    if (out && out_len)
        out[n < out_len ? n : out_len - 1] = '\0';
    return consumed;
}

static int dns_parse(const uint8_t* msg, const size_t len, const uint16_t expected_id, struct dns_result* out,
                     const size_t max_out, char* qname_out, uint16_t* qtype_out)
{
    if (len < DNS_HEADER_LEN)
        return -EINVAL;
    const uint16_t id = dns_get16(msg), flags = dns_get16(msg + 2);
    const uint16_t qd = dns_get16(msg + 4), an = dns_get16(msg + 6);
    if (id != expected_id || !(flags & DNS_FLAG_QR))
        return -EINVAL;
    switch (flags & DNS_RCODE_MASK) {
    case DNS_RCODE_NOERROR:
        break;
    case DNS_RCODE_NXDOMAIN:
        return -ENOENT;
    default:
        return -EIO;
    }

    char current[DNS_MAX_NAME + 1] = "";
    size_t pos = DNS_HEADER_LEN;
    for (uint16_t i = 0; i < qd; i++) {
        char qname[DNS_MAX_NAME + 1];
        const int n = dns_decode_name(msg, len, pos, qname, sizeof(qname));
        if (n < 0 || pos + (size_t)n + 4 > len)
            return -EINVAL;
        if (i == 0) {
            dns_strcpy(current, qname, sizeof(current));
            if (qname_out)
                dns_strcpy(qname_out, qname, DNS_MAX_NAME + 1);
            if (qtype_out)
                *qtype_out = dns_get16(msg + pos + n);
        }
        pos += (size_t)n + 4;
    }

    size_t count = 0;
    for (uint16_t i = 0; i < an; i++) {
        char owner[DNS_MAX_NAME + 1];
        const int n = dns_decode_name(msg, len, pos, owner, sizeof(owner));
        if (n < 0 || pos + (size_t)n + 10 > len)
            return -EINVAL;
        const uint8_t* rr = msg + pos + n;
        const uint16_t type = dns_get16(rr), class = dns_get16(rr + 2);
        const uint32_t ttl = dns_get32(rr + 4);
        const uint16_t rdlen = dns_get16(rr + 8);
        const size_t rdata = pos + (size_t)n + 10;
        if (rdata + rdlen > len)
            return -EINVAL;
        pos = rdata + rdlen;
        if (class != DNS_CLASS_IN || (qd && !dns_name_eq(owner, current)))
            continue;
        if (type == DNS_TYPE_CNAME) {
            char target[DNS_MAX_NAME + 1];
            if (dns_decode_name(msg, len, rdata, target, sizeof(target)) < 0)
                return -EINVAL;
            dns_strcpy(current, target, sizeof(current));
        } else if ((type == DNS_TYPE_A && rdlen == 4) || (type == DNS_TYPE_AAAA && rdlen == 16)) {
            if (count < max_out) {
                out[count].family = type == DNS_TYPE_A ? AF_INET : AF_INET6;
                memset(out[count].addr, 0, sizeof(out[count].addr));
                memcpy(out[count].addr, msg + rdata, rdlen);
                out[count].ttl = ttl;
                count++;
            }
        }
    }
    return (int)count;
}

int dns_parse_response(const uint8_t* msg, const size_t len, const uint16_t expected_id,
                       struct dns_result* out, const size_t max_out)
{
    return dns_parse(msg, len, expected_id, out, max_out, nullptr, nullptr);
}

static struct dns_cache_entry* dns_cache_find(const char* name, const sa_family_t family, const uint64_t now)
{
    for (size_t i = 0; i < DNS_CACHE_SIZE; i++) {
        struct dns_cache_entry* e = &cache[i];
        if (e->expires && e->expires <= now)
            e->expires = 0;
        if (e->expires && e->family == family && dns_name_eq(e->name, name))
            return e;
    }
    return nullptr;
}

static struct dns_cache_entry* dns_cache_slot(const uint64_t now)
{
    struct dns_cache_entry* victim = &cache[0];
    for (size_t i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!cache[i].expires || cache[i].expires <= now)
            return &cache[i];
        if (cache[i].expires < victim->expires)
            victim = &cache[i];
    }
    return victim;
}

static uint64_t dns_ttl_ticks(const uint32_t ttl)
{
    return ticks_from_ms((uint64_t)(ttl < DNS_MAX_TTL_S ? ttl : DNS_MAX_TTL_S) * 1000);
}

static void dns_cache_store(const char* name, const sa_family_t family, const struct dns_result* results,
                            const size_t n)
{
    uint32_t ttl = DNS_MAX_TTL_S;
    for (size_t i = 0; i < n; i++)
        if (results[i].ttl < ttl)
            ttl = results[i].ttl;
    if (!n || !ttl || dns_strlen(name) > DNS_MAX_NAME)
        return;
    const uint64_t now = ticks_get();
    uint64_t flags;
    spinlock_acquire_irqsave(&dns_lock, &flags);
    struct dns_cache_entry* e = dns_cache_find(name, family, now);
    if (!e)
        e = dns_cache_slot(now);
    dns_strcpy(e->name, name, sizeof(e->name));
    e->family = family;
    e->count = (uint8_t)(n < DNS_CACHE_RESULTS ? n : DNS_CACHE_RESULTS);
    memcpy(e->results, results, e->count * sizeof(struct dns_result));
    e->expires = now + dns_ttl_ticks(ttl);
    spinlock_release_irqrestore(&dns_lock, flags);
}

static size_t dns_cache_get(const char* name, const sa_family_t family, struct dns_result* out, const size_t max)
{
    const sa_family_t order[2] = { family == AF_INET ? AF_INET : AF_INET6, AF_INET };
    const int families = family == AF_UNSPEC ? 2 : 1;
    const uint64_t now = ticks_get();
    size_t n = 0;
    uint64_t flags;
    spinlock_acquire_irqsave(&dns_lock, &flags);
    for (int f = 0; f < families; f++) {
        const struct dns_cache_entry* e = dns_cache_find(name, order[f], now);
        for (uint8_t i = 0; e && i < e->count && n < max; i++) {
            out[n] = e->results[i];
            out[n].ttl = (uint32_t)((e->expires - now) / ticks_from_ms(1000));
            n++;
        }
    }
    spinlock_release_irqrestore(&dns_lock, flags);
    return n;
}

bool dns_cache_lookup(const char* name, const sa_family_t family, struct dns_result* out)
{
    return dns_cache_get(name, family, out, 1) == 1;
}

void dns_cache_insert(const char* name, const struct dns_result* result)
{
    if (!result || (result->family != AF_INET && result->family != AF_INET6) || !result->ttl)
        return;
    struct dns_result merged[DNS_CACHE_RESULTS];
    size_t n = dns_cache_get(name, result->family, merged, DNS_CACHE_RESULTS);
    for (size_t i = 0; i < n; i++)
        if (!memcmp(merged[i].addr, (void*)result->addr, sizeof(result->addr)))
            return;
    if (n < DNS_CACHE_RESULTS)
        merged[n++] = *result;
    dns_cache_store(name, result->family, merged, n);
}

void dns_cache_flush()
{
    uint64_t flags;
    spinlock_acquire_irqsave(&dns_lock, &flags);
    memset(cache, 0, sizeof(cache));
    spinlock_release_irqrestore(&dns_lock, flags);
}

static int dns_query_server(const struct sockaddr_storage* server, const char* name, const uint16_t qtype,
                            struct dns_result* out, const size_t max_out, const uint64_t deadline)
{
    static uint8_t buf[DNS_MAX_PACKET];   // TODO: per caller once there are threads
    const uint16_t id = (uint16_t)net_random32();
    const size_t qlen = dns_build_query(id, name, qtype, buf, sizeof(buf));
    if (!qlen)
        return -EINVAL;

    struct socket* so;
    int err = sock_create(server->ss_family, SOCK_DGRAM, 0, &so);
    if (err)
        return err;
    const socklen_t slen = server->ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

    err = sock_connect(so, (const struct sockaddr*)server, slen);
    if (!err) {
        const ssize_t sent = sock_send(so, buf, qlen, MSG_DONTWAIT, nullptr, 0);
        err = sent < 0 ? (int)sent : 0;
    }
    int result = -ETIMEDOUT;
    while (!err) {
        const ssize_t n = sock_recv(so, buf, sizeof(buf), MSG_DONTWAIT, nullptr, nullptr);
        if (n == -EAGAIN) {
            const uint64_t now = ticks_get();
            if (now >= deadline)
                break;
            if (sockbuf_timedwait(&so->rcv, deadline - now) == -EINTR) {
                result = -EINTR;
                break;
            }
            continue;
        }
        if (n < 0) {
            result = (int)n;
            break;
        }
        char qname[DNS_MAX_NAME + 1];
        uint16_t got_qtype = 0;
        const int r = dns_parse(buf, (size_t)n, id, out, max_out, qname, &got_qtype);
        if (r == -EINVAL)
            continue;
        if (r >= 0 && (!dns_name_eq(qname, name) || got_qtype != qtype))
            continue;
        // TODO: retry over TCP when TC is set (RFC 7766); for now take what fit
        result = r;
        break;
    }
    sock_close(so);
    return err ? err : result;
}

static int dns_resolve_type(const char* name, const uint16_t qtype, struct dns_result* out, const size_t max_out,
                            const uint32_t timeout_ms)
{
    struct sockaddr_storage list[DNS_MAX_SERVERS];
    uint64_t flags;
    spinlock_acquire_irqsave(&dns_lock, &flags);
    const size_t count = server_count;
    memcpy(list, servers, count * sizeof(struct sockaddr_storage));
    spinlock_release_irqrestore(&dns_lock, flags);
    if (!count)
        return -EDESTADDRREQ;

    uint32_t per_try = timeout_ms / (uint32_t)(count * DNS_ROUNDS);
    if (per_try < DNS_MIN_TRY_MS)
        per_try = DNS_MIN_TRY_MS;
    const uint64_t end = ticks_get() + ticks_from_ms(timeout_ms);
    int last = -ETIMEDOUT;
    for (int round = 0; round < DNS_ROUNDS; round++) {
        for (size_t i = 0; i < count; i++) {
            const uint64_t now = ticks_get();
            if (now >= end)
                return last;
            uint64_t deadline = now + ticks_from_ms(per_try);
            if (deadline > end)
                deadline = end;
            const int r = dns_query_server(&list[i], name, qtype, out, max_out, deadline);
            if (r >= 0 || r == -ENOENT || r == -EINTR)
                return r;
            last = r;
        }
    }
    return last;
}

static int dns_literal(const char* name, const sa_family_t family, struct dns_result* out)
{
    struct in_addr v4;
    struct in6_addr v6;
    memset(out, 0, sizeof(*out));
    if (family != AF_INET6 && ipv4_pton(name, &v4) == 0) {
        out->family = AF_INET;
        memcpy(out->addr, &v4, sizeof(v4));
        return 1;
    }
    if (family != AF_INET && ipv6_pton(name, &v6) == 0) {
        out->family = AF_INET6;
        memcpy(out->addr, &v6, sizeof(v6));
        return 1;
    }
    return 0;
}

int dns_resolve(const char* name, const sa_family_t family, struct dns_result* out, const size_t max_out,
                const uint32_t timeout_ms)
{
    if (!name || !out || !max_out || (family != AF_INET && family != AF_INET6 && family != AF_UNSPEC))
        return -EINVAL;
    uint8_t wire[DNS_MAX_NAME + 1];
    if (!dns_encode_name(name, wire, sizeof(wire)))
        return -EINVAL;
    if (dns_literal(name, family, out))
        return 1;
    const size_t cached = dns_cache_get(name, family, out, max_out);
    if (cached)
        return (int)cached;

    const uint16_t types[2] = { family == AF_INET ? DNS_TYPE_A : DNS_TYPE_AAAA, DNS_TYPE_A };
    const int n_types = family == AF_UNSPEC ? 2 : 1;
    size_t total = 0;
    int err = 0;
    for (int t = 0; t < n_types && total < max_out; t++) {
        const int r = dns_resolve_type(name, types[t], out + total, max_out - total, timeout_ms);
        if (r == -ENOENT || r == -EINTR)
            return total ? (int)total : r;
        if (r < 0) {
            err = r;
            continue;
        }
        dns_cache_store(name, types[t] == DNS_TYPE_A ? AF_INET : AF_INET6, out + total, (size_t)r);
        total += (size_t)r;
    }
    return total ? (int)total : err;
}
