#pragma once

// MurmurHash3 -- public domain reference implementation by Austin Appleby.
// Adapted for the pensieve project: wrapped in namespace, C++20 types.

#include <cstdint>
#include <cstddef>

namespace pensieve {

void murmurhash3_x86_32(const void* key, size_t len, uint32_t seed,
                        uint32_t* out);

void murmurhash3_x86_128(const void* key, size_t len, uint32_t seed,
                         uint32_t out[4]);

void murmurhash3_x64_128(const void* key, size_t len, uint32_t seed,
                         uint64_t out[2]);

}  // namespace pensieve
