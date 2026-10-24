#include "random.h"

#include <os/stdbool.h>
#include <hardware/memory/alloc.h>

static bool cpu_has(const uint32_t leaf, const int reg, const uint32_t bit)
{
    uint32_t r[4] = { leaf, 0, 0, 0 };
    asm volatile("cpuid" : "+a"(r[0]), "=b"(r[1]), "+c"(r[2]), "=d"(r[3]));
    return r[reg] & bit;
}

static uint64_t tsc()
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static uint64_t fallback()
{
    static uint64_t state;
    state += 0x9E3779B97F4A7C15ULL ^ tsc();
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t random_u64()
{
    static int rdseed = -1;
    static int rdrand = -1;
    if (rdrand < 0) {
        rdrand = cpu_has(1, 2, 1u << 30);
        rdseed = cpu_has(7, 1, 1u << 18);
    }
    uint64_t v;
    uint8_t ok;
    for (int tries = 0; rdseed && tries < 32; tries++) {
        asm volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok)
            return v;
    }
    for (int tries = 0; rdrand && tries < 32; tries++) {
        asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok)
            return v;
    }
    return fallback();
}

void random_fill(void* buf, const size_t len)
{
    for (size_t done = 0; done < len;) {
        const uint64_t r = random_u64();
        const size_t n = len - done < sizeof(r) ? len - done : sizeof(r);
        memcpy((uint8_t*)buf + done, &r, n);
        done += n;
    }
}
