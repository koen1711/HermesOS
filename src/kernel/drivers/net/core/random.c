#include <drivers/net/core/random.h>

#include <drivers/timer/callout.h>
#include <hardware/memory/alloc.h>
#include <utils/sync/spinlock.h>

#define RANDOM_RESEED_BLOCKS 1024
#define RANDOM_HW_RETRIES    10

static spinlock_t random_lock;
static uint32_t drbg_key[8];
static uint64_t drbg_counter;
static uint32_t drbg_out[8];
static size_t drbg_avail;
static uint32_t drbg_blocks;
static bool drbg_seeded;

static uint64_t random_rdtsc()
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static uint32_t random_cpuid(const uint32_t leaf, uint32_t* ebx, uint32_t* ecx)
{
    uint32_t eax = leaf, edx;
    *ecx = 0;
    asm volatile("cpuid" : "+a"(eax), "=b"(*ebx), "+c"(*ecx), "=d"(edx));
    return eax;
}

static bool random_has_rdrand()
{
    uint32_t ebx, ecx;
    random_cpuid(1, &ebx, &ecx);
    return ecx & (1u << 30);
}

static bool random_has_rdseed()
{
    uint32_t ebx, ecx;
    if (random_cpuid(0, &ebx, &ecx) < 7)
        return false;
    random_cpuid(7, &ebx, &ecx);
    return ebx & (1u << 18);
}

static bool random_rdseed64(uint64_t* out)
{
    for (int i = 0; i < RANDOM_HW_RETRIES; i++) {
        uint8_t ok;
        asm volatile("rdseed %0; setc %1" : "=r"(*out), "=qm"(ok) :: "cc");
        if (ok)
            return true;
    }
    return false;
}

static bool random_rdrand64(uint64_t* out)
{
    for (int i = 0; i < RANDOM_HW_RETRIES; i++) {
        uint8_t ok;
        asm volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok) :: "cc");
        if (ok)
            return true;
    }
    return false;
}

static uint64_t random_jitter64()
{
    uint64_t acc = ticks_get();
    for (int i = 0; i < 64; i++) {
        const uint64_t start = random_rdtsc();
        for (volatile int j = 0; j < 64 + (int)(acc & 31); j++)
            ;
        acc = (acc << 7 | acc >> 57) ^ (random_rdtsc() - start);
    }
    return acc;
}

static uint64_t random_entropy64()
{
    static int has_rdseed = -1, has_rdrand = -1;
    if (has_rdseed < 0) {
        has_rdseed = random_has_rdseed();
        has_rdrand = random_has_rdrand();
    }
    uint64_t v;
    if (has_rdseed && random_rdseed64(&v))
        return v;
    if (has_rdrand && random_rdrand64(&v))
        return v;
    return random_jitter64();
}

static uint32_t rotl32(const uint32_t v, const int n)
{
    return v << n | v >> (32 - n);
}

#define CHACHA_QR(a, b, c, d)                      \
    a += b; d = rotl32(d ^ a, 16);                 \
    c += d; b = rotl32(b ^ c, 12);                 \
    a += b; d = rotl32(d ^ a, 8);                  \
    c += d; b = rotl32(b ^ c, 7)

static void chacha20_block(const uint32_t key[8], const uint64_t counter, uint32_t out[16])
{
    const uint32_t in[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        (uint32_t)counter, (uint32_t)(counter >> 32), 0, 0,
    };
    uint32_t x[16];
    memcpy(x, in, sizeof(x));
    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x[0], x[4], x[8], x[12]);
        CHACHA_QR(x[1], x[5], x[9], x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8], x[13]);
        CHACHA_QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++)
        out[i] = x[i] + in[i];
}

static void drbg_reseed()
{
    for (int i = 0; i < 8; i += 2) {
        const uint64_t e = random_entropy64();
        drbg_key[i] ^= (uint32_t)e;
        drbg_key[i + 1] ^= (uint32_t)(e >> 32);
    }
    drbg_blocks = 0;
    drbg_avail = 0;
    drbg_seeded = true;
}

static void drbg_refill()
{
    if (!drbg_seeded || drbg_blocks >= RANDOM_RESEED_BLOCKS)
        drbg_reseed();
    uint32_t block[16];
    chacha20_block(drbg_key, drbg_counter++, block);
    memcpy(drbg_key, block, sizeof(drbg_key));
    memcpy(drbg_out, block + 8, sizeof(drbg_out));
    memset(block, 0, sizeof(block));
    drbg_avail = 8;
    drbg_blocks++;
}

static uint32_t drbg_next()
{
    if (!drbg_avail)
        drbg_refill();
    const uint32_t v = drbg_out[--drbg_avail];
    drbg_out[drbg_avail] = 0;
    return v;
}

uint32_t net_random32()
{
    uint64_t flags;
    spinlock_acquire_irqsave(&random_lock, &flags);
    const uint32_t v = drbg_next();
    spinlock_release_irqrestore(&random_lock, flags);
    return v;
}

void net_random_bytes(void* out, size_t len)
{
    uint8_t* p = out;
    uint64_t flags;
    spinlock_acquire_irqsave(&random_lock, &flags);
    while (len) {
        const uint32_t v = drbg_next();
        const size_t n = len < sizeof(v) ? len : sizeof(v);
        memcpy(p, &v, n);
        p += n;
        len -= n;
    }
    spinlock_release_irqrestore(&random_lock, flags);
}
