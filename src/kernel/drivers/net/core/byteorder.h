#ifndef NET_CORE_BYTEORDER_H
#define NET_CORE_BYTEORDER_H

#include <os/stdint.h>

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#endif //NET_CORE_BYTEORDER_H
