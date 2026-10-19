#ifndef NET_CORE_CHECKSUM_H
#define NET_CORE_CHECKSUM_H

#include <os/stdint.h>
#include <os/stddef.h>
#include <drivers/net/core/netbuf.h>
#include <drivers/net/core/inet_addr.h>

uint16_t inet_checksum(const void* data, size_t len);

uint16_t inet_checksum_netbuf(const struct netbuf* nb, size_t offset, size_t len);

uint32_t inet_pseudo_checksum4(struct in_addr src, struct in_addr dst, uint8_t proto, uint16_t len);
uint32_t inet_pseudo_checksum6(const struct in6_addr* src, const struct in6_addr* dst, uint8_t next_header, uint32_t len);

uint16_t inet_checksum_update(uint16_t checksum, uint16_t old_value, uint16_t new_value);

#endif //NET_CORE_CHECKSUM_H
