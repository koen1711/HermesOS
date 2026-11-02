#ifndef STACKWALK_H
#define STACKWALK_H

#ifdef STACKWALK_HOST
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#else
#include <os/stdint.h>
#include <os/stdbool.h>
#include <os/stddef.h>
#endif

#define CTX_RBP_OFFSET 64
#define CTX_RIP_OFFSET 136
#define CTX_CS_OFFSET 144
#define CTX_SIZE 176

struct stackwalk_bounds {
    uint64_t stack_low, stack_high, text_low, text_high, trap_return;
};

size_t stackwalk(uint64_t rbp, const struct stackwalk_bounds* b, uint64_t* out, size_t max, bool* truncated);

#endif
