#ifndef NET_CORE_RANDOM_H
#define NET_CORE_RANDOM_H

#include <os/stdint.h>
#include <os/stddef.h>

uint32_t net_random32();
void net_random_bytes(void* out, size_t len);

#endif //NET_CORE_RANDOM_H
