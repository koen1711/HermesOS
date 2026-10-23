#include <drivers/net/core/checksum.h>

static uint16_t inet_fold(uint64_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return sum;
}

static uint64_t inet_sum(const uint8_t* buf, size_t len)
{
    uint64_t sum = 0;

    while (len > 1) {
        sum += (buf[0] << 8) | buf[1];
        buf += 2;
        len -= 2;
    }

    if (len > 0) {
        sum += buf[0] << 8;
    }

    return sum;
}

uint16_t inet_checksum(const void* data, const size_t len)
{
    return ~inet_fold(inet_sum(data, len)) & 0xFFFF;
}

uint16_t inet_checksum_netbuf(const struct netbuf* nb, size_t offset, size_t len)
{
    uint64_t sum = 0;
    bool odd = false;   // an odd number of bytes came before this piece

    while (nb && len > 0) {
        if (offset >= nb->len) {
            offset -= nb->len;
            nb = nb->next;
            continue;
        }

        const size_t avail = nb->len - offset;
        const size_t n = len < avail ? len : avail;

        uint16_t part = inet_fold(inet_sum(nb->data + offset, n));
        if (odd)
            part = (uint16_t)(part << 8 | part >> 8);
        sum += part;
        odd ^= n & 1;

        len -= n;
        offset = 0;
        nb = nb->next;
    }

    return ~inet_fold(sum) & 0xFFFF;
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
