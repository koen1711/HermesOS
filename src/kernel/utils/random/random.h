#ifndef UTILS_RANDOM_RANDOM_H
#define UTILS_RANDOM_RANDOM_H

#include <os/stdint.h>
#include <os/stddef.h>

uint64_t random_u64();
void random_fill(void* buf, size_t len);

#endif //UTILS_RANDOM_RANDOM_H
