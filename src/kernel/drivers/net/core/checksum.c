#include <drivers/net/core/checksum.h>

#include <hardware/memory/alloc.h>

static uint16_t inet_fold(uint64_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return sum;
}

static uint64_t inet_sum_raw(const uint8_t *buf, size_t len)
{
    uint64_t s0 = 0, s1 = 0;
    uint32_t w0, w1;

    while (len >= 8) {
        memcpy(&w0, buf, 4);
        memcpy(&w1, buf + 4, 4);
        s0 += w0;
        s1 += w1;
        buf += 8;
        len -= 8;
    }
    if (len >= 4) { memcpy(&w0, buf, 4); s0 += w0; buf += 4; len -= 4; }
    if (len >= 2) { uint16_t h; memcpy(&h, buf, 2); s0 += h; buf += 2; len -= 2; }
    if (len)      { uint16_t h = 0; memcpy(&h, buf, 1); s0 += h; }

    return s0 + s1;
}

static inline uint64_t csum_add(uint64_t a, const uint64_t b)
{
    a += b;
    return a + (a < b);
}

static inline uint16_t csum_fold(uint64_t s)
{
    s = (s & 0xFFFFFFFF) + (s >> 32);
    s = (s & 0xFFFFFFFF) + (s >> 32);
    s = (s & 0xFFFF) + (s >> 16);
    s = (s & 0xFFFF) + (s >> 16);
    s = (s & 0xFFFF) + (s >> 16);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    s = ((s & 0xFF) << 8) | (s >> 8);
#endif
    return (uint16_t)s;
}

static uint64_t inet_sum(const uint8_t *buf, size_t len)
{
    return csum_fold(inet_sum_raw(buf, len));
}

uint16_t inet_checksum(const void* data, const size_t len)
{
    return ~inet_fold(inet_sum(data, len)) & 0xFFFF;
}

uint16_t inet_checksum_netbuf(const struct netbuf *nb, size_t offset, size_t len)
{
    uint64_t sum = 0;
    bool odd = false;

    while (nb && offset >= nb->len) {
        offset -= nb->len;
        nb = nb->next;
    }

    for (; nb && len > 0; nb = nb->next, offset = 0) {
        const size_t avail = nb->len - offset;
        const size_t n = len < avail ? len : avail;

        uint64_t part = inet_sum_raw(nb->data + offset, n);
        if (odd)
            part = part << 8 | part >> 56;
        sum = csum_add(sum, part);

        odd ^= n & 1;
        len -= n;
    }

    return (uint16_t)~csum_fold(sum);
}

uint32_t inet_pseudo_checksum4(const struct in_addr src, const struct in_addr dst, const uint8_t proto, const uint16_t len)
{
    uint64_t sum = 0;
    sum += inet_sum((const uint8_t*)&src.s_addr, 4);
    sum += inet_sum((const uint8_t*)&dst.s_addr, 4);
    sum += proto;
    sum += len;

    return inet_fold(sum);
}

uint32_t inet_pseudo_checksum6(const struct in6_addr* src, const struct in6_addr* dst, const uint8_t next_header, const uint32_t len)
{
    uint64_t sum = 0;
    sum += inet_sum(src->s6_addr, 16);
    sum += inet_sum(dst->s6_addr, 16);
    sum += len >> 16;
    sum += len & 0xFFFF;
    sum += next_header;

    return inet_fold(sum);
}

uint16_t inet_checksum_update(const uint16_t checksum, const uint16_t old_value, const uint16_t new_value)
{
    uint64_t sum = (uint16_t)~checksum;
    sum += (uint16_t)~old_value;
    sum += new_value;

    return ~inet_fold(sum) & 0xFFFF;
}
