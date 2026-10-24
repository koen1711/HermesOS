#include <drivers/net/core/siphash.h>

static uint64_t rotl64(const uint64_t v, const int n)
{
    return v << n | v >> (64 - n);
}

static uint64_t load_le64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static void sip_round(uint64_t v[4])
{
    v[0] += v[1]; v[1] = rotl64(v[1], 13); v[1] ^= v[0]; v[0] = rotl64(v[0], 32);
    v[2] += v[3]; v[3] = rotl64(v[3], 16); v[3] ^= v[2];
    v[0] += v[3]; v[3] = rotl64(v[3], 21); v[3] ^= v[0];
    v[2] += v[1]; v[1] = rotl64(v[1], 17); v[1] ^= v[2]; v[2] = rotl64(v[2], 32);
}

static void sip_compress(uint64_t v[4], const uint64_t m)
{
    v[3] ^= m;
    sip_round(v);
    sip_round(v);
    v[0] ^= m;
}

uint64_t siphash24(const uint8_t key[SIPHASH_KEY_LEN], const void* data, const size_t len)
{
    const uint64_t k0 = load_le64(key);
    const uint64_t k1 = load_le64(key + 8);
    uint64_t v[4] = {
        k0 ^ 0x736f6d6570736575ull,
        k1 ^ 0x646f72616e646f6dull,
        k0 ^ 0x6c7967656e657261ull,
        k1 ^ 0x7465646279746573ull,
    };

    const uint8_t* p = data;
    size_t left = len;
    for (; left >= 8; p += 8, left -= 8)
        sip_compress(v, load_le64(p));

    uint64_t last = (uint64_t)len << 56;
    for (size_t i = 0; i < left; i++)
        last |= (uint64_t)p[i] << (8 * i);
    sip_compress(v, last);

    v[2] ^= 0xFF;
    for (int i = 0; i < 4; i++)
        sip_round(v);
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}
