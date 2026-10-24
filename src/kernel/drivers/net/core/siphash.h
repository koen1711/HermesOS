#ifndef NET_CORE_SIPHASH_H
#define NET_CORE_SIPHASH_H

#include <os/stdint.h>
#include <os/stddef.h>

#define SIPHASH_KEY_LEN 16

uint64_t siphash24(const uint8_t key[SIPHASH_KEY_LEN], const void* data, size_t len);

#endif //NET_CORE_SIPHASH_H
