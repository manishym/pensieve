// MurmurHash3 -- public domain reference implementation by Austin Appleby.
// https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
//
// Adapted: wrapped in namespace pensieve, uses size_t for length,
// silenced sign-conversion warnings.

#include "hash/murmurhash3.h"

#include <algorithm>
#include <cstring>

namespace pensieve {

namespace {

inline uint32_t rotl32(uint32_t x, int8_t r) {
    return (x << r) | (x >> (32 - r));
}

inline uint64_t rotl64(uint64_t x, int8_t r) {
    return (x << r) | (x >> (64 - r));
}

inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

inline uint32_t getblock32(const uint32_t* p, size_t i) {
    uint32_t v;
    std::memcpy(&v, p + i, sizeof(v));
    return v;
}

inline uint64_t getblock64(const uint64_t* p, size_t i) {
    uint64_t v;
    std::memcpy(&v, p + i, sizeof(v));
    return v;
}

// Accumulate tail bytes [start, start+count) into a 32-bit word,
// byte at offset 0 in the least-significant position.
inline uint32_t tail_word32(const uint8_t* tail, size_t start, size_t count) {
    uint32_t k = 0;
    for (size_t i = 0; i < count; ++i)
        k ^= static_cast<uint32_t>(tail[start + i]) << (i * 8);
    return k;
}

// Same for 64-bit words.
inline uint64_t tail_word64(const uint8_t* tail, size_t start, size_t count) {
    uint64_t k = 0;
    for (size_t i = 0; i < count; ++i)
        k ^= static_cast<uint64_t>(tail[start + i]) << (i * 8);
    return k;
}

}  // namespace

void murmurhash3_x86_32(const void* key, size_t len, uint32_t seed,
                        uint32_t* out) {
    const auto* data = static_cast<const uint8_t*>(key);
    const size_t nblocks = len / 4;

    uint32_t h1 = seed;

    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    const auto* blocks =
        reinterpret_cast<const uint32_t*>(data + nblocks * 4);

    for (size_t i = 0; i < nblocks; ++i) {
        // Read blocks in reverse order (matches reference impl)
        uint32_t k1 = getblock32(blocks, static_cast<size_t>(-static_cast<ptrdiff_t>(i) - 1));

        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;

        h1 ^= k1;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xe6546b64;
    }

    const auto* tail = data + nblocks * 4;

    uint32_t k1 = 0;

    switch (len & 3) {
    case 3:
        k1 ^= static_cast<uint32_t>(tail[2]) << 16;
        [[fallthrough]];
    case 2:
        k1 ^= static_cast<uint32_t>(tail[1]) << 8;
        [[fallthrough]];
    case 1:
        k1 ^= static_cast<uint32_t>(tail[0]);
        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;
        h1 ^= k1;
    }

    h1 ^= static_cast<uint32_t>(len);
    h1 = fmix32(h1);

    *out = h1;
}

void murmurhash3_x86_128(const void* key, size_t len, uint32_t seed,
                         uint32_t out[4]) {
    const auto* data = static_cast<const uint8_t*>(key);
    const size_t nblocks = len / 16;

    uint32_t h1 = seed;
    uint32_t h2 = seed;
    uint32_t h3 = seed;
    uint32_t h4 = seed;

    const uint32_t c1 = 0x239b961b;
    const uint32_t c2 = 0xab0e9789;
    const uint32_t c3 = 0x38b34ae5;
    const uint32_t c4 = 0xa1e38b93;

    const auto* blocks =
        reinterpret_cast<const uint32_t*>(data + nblocks * 16);

    for (size_t i = 0; i < nblocks; ++i) {
        size_t base = i * 4;
        uint32_t k1 = getblock32(blocks, base + 0);
        uint32_t k2 = getblock32(blocks, base + 1);
        uint32_t k3 = getblock32(blocks, base + 2);
        uint32_t k4 = getblock32(blocks, base + 3);

        k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
        h1 = rotl32(h1, 19); h1 += h2; h1 = h1 * 5 + 0x561ccd1b;

        k2 *= c2; k2 = rotl32(k2, 16); k2 *= c3; h2 ^= k2;
        h2 = rotl32(h2, 17); h2 += h3; h2 = h2 * 5 + 0x0bcaa747;

        k3 *= c3; k3 = rotl32(k3, 17); k3 *= c4; h3 ^= k3;
        h3 = rotl32(h3, 15); h3 += h4; h3 = h3 * 5 + 0x96cd1c35;

        k4 *= c4; k4 = rotl32(k4, 18); k4 *= c1; h4 ^= k4;
        h4 = rotl32(h4, 13); h4 += h1; h4 = h4 * 5 + 0x32ac3b17;
    }

    const auto* tail = data + nblocks * 16;
    const size_t rem = len & 15;

    if (rem > 12) {
        uint32_t k4 = tail_word32(tail, 12, rem - 12);
        k4 *= c4; k4 = rotl32(k4, 18); k4 *= c1; h4 ^= k4;
    }
    if (rem > 8) {
        uint32_t k3 = tail_word32(tail, 8, std::min(rem, size_t{12}) - 8);
        k3 *= c3; k3 = rotl32(k3, 17); k3 *= c4; h3 ^= k3;
    }
    if (rem > 4) {
        uint32_t k2 = tail_word32(tail, 4, std::min(rem, size_t{8}) - 4);
        k2 *= c2; k2 = rotl32(k2, 16); k2 *= c3; h2 ^= k2;
    }
    if (rem > 0) {
        uint32_t k1 = tail_word32(tail, 0, std::min(rem, size_t{4}));
        k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
    }

    h1 ^= static_cast<uint32_t>(len);
    h2 ^= static_cast<uint32_t>(len);
    h3 ^= static_cast<uint32_t>(len);
    h4 ^= static_cast<uint32_t>(len);

    h1 += h2; h1 += h3; h1 += h4;
    h2 += h1; h3 += h1; h4 += h1;

    h1 = fmix32(h1);
    h2 = fmix32(h2);
    h3 = fmix32(h3);
    h4 = fmix32(h4);

    h1 += h2; h1 += h3; h1 += h4;
    h2 += h1; h3 += h1; h4 += h1;

    out[0] = h1;
    out[1] = h2;
    out[2] = h3;
    out[3] = h4;
}

void murmurhash3_x64_128(const void* key, size_t len, uint32_t seed,
                         uint64_t out[2]) {
    const auto* data = static_cast<const uint8_t*>(key);
    const size_t nblocks = len / 16;

    uint64_t h1 = seed;
    uint64_t h2 = seed;

    const uint64_t c1 = 0x87c37b91114253d5ULL;
    const uint64_t c2 = 0x4cf5ad432745937fULL;

    const auto* blocks =
        reinterpret_cast<const uint64_t*>(data);

    for (size_t i = 0; i < nblocks; ++i) {
        uint64_t k1 = getblock64(blocks, i * 2);
        uint64_t k2 = getblock64(blocks, i * 2 + 1);

        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
        h1 = rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;

        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
        h2 = rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
    }

    const auto* tail = data + nblocks * 16;
    const size_t rem = len & 15;

    if (rem > 8) {
        uint64_t k2 = tail_word64(tail, 8, rem - 8);
        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
    }
    if (rem > 0) {
        uint64_t k1 = tail_word64(tail, 0, std::min(rem, size_t{8}));
        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
    }

    h1 ^= static_cast<uint64_t>(len);
    h2 ^= static_cast<uint64_t>(len);

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;
    h2 += h1;

    out[0] = h1;
    out[1] = h2;
}

}  // namespace pensieve
