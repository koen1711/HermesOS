#ifndef NET_CORE_INET_ADDR_H
#define NET_CORE_INET_ADDR_H

#include <os/stdint.h>

struct in_addr {
    uint32_t s_addr;
};

struct in6_addr {
    union {
        uint8_t s6_addr[16];
        uint16_t s6_addr16[8];
        uint32_t s6_addr32[4];
    };
};

#endif //NET_CORE_INET_ADDR_H
