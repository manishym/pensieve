#include "hash/murmurhash3.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

TEST(MurmurHash3, X86_32_EmptyString) {
    uint32_t out = 0;
    murmurhash3_x86_32("", 0, 0, &out);
    EXPECT_EQ(out, 0u);
}

TEST(MurmurHash3, X86_32_EmptyWithSeed) {
    uint32_t out = 0;
    murmurhash3_x86_32("", 0, 1, &out);
    EXPECT_EQ(out, 0x514e28b7u);
}

TEST(MurmurHash3, X86_32_KnownVectors) {
    uint32_t out = 0;

    // Single-block + tail: verified against Python reference implementation
    murmurhash3_x86_32("hello", 5, 0, &out);
    EXPECT_EQ(out, 0x248bfa47u);

    murmurhash3_x86_32("hello", 5, 42, &out);
    EXPECT_EQ(out, 0xe2dbd2e1u);

    // Multi-block (3 blocks + 1 tail byte)
    murmurhash3_x86_32("hello world!!", 13, 0, &out);
    EXPECT_EQ(out, 0xe2cada3fu);
}

TEST(MurmurHash3, X86_32_Deterministic) {
    const std::string key = "hello world";
    uint32_t h1 = 0, h2 = 0;
    murmurhash3_x86_32(key.data(), key.size(), 42, &h1);
    murmurhash3_x86_32(key.data(), key.size(), 42, &h2);
    EXPECT_EQ(h1, h2);
}

TEST(MurmurHash3, X86_32_SeedChangesOutput) {
    const std::string key = "test";
    uint32_t h1 = 0, h2 = 0;
    murmurhash3_x86_32(key.data(), key.size(), 0, &h1);
    murmurhash3_x86_32(key.data(), key.size(), 1, &h2);
    EXPECT_NE(h1, h2);
}

TEST(MurmurHash3, X86_32_Distribution) {
    constexpr int kNumKeys = 10000;
    constexpr int kBuckets = 16;
    std::vector<int> counts(kBuckets, 0);

    for (int i = 0; i < kNumKeys; ++i) {
        std::string key = "key-" + std::to_string(i);
        uint32_t out = 0;
        murmurhash3_x86_32(key.data(), key.size(), 0, &out);
        counts[out % kBuckets]++;
    }

    double expected = static_cast<double>(kNumKeys) / kBuckets;
    for (int c : counts) {
        EXPECT_GT(c, static_cast<int>(expected * 0.7))
            << "Bucket significantly under-represented";
        EXPECT_LT(c, static_cast<int>(expected * 1.3))
            << "Bucket significantly over-represented";
    }
}

TEST(MurmurHash3, X86_32_AllLengths) {
    // Verify no crashes or zero-output for lengths 0..32
    std::set<uint32_t> hashes;
    for (size_t len = 0; len <= 32; ++len) {
        std::string key(len, 'x');
        uint32_t out = 0;
        murmurhash3_x86_32(key.data(), key.size(), 0, &out);
        hashes.insert(out);
    }
    // 33 different lengths should produce mostly unique hashes
    EXPECT_GT(hashes.size(), 28u);
}

TEST(MurmurHash3, X64_128_Deterministic) {
    const std::string key = "hello world";
    uint64_t h1[2] = {}, h2[2] = {};
    murmurhash3_x64_128(key.data(), key.size(), 42, h1);
    murmurhash3_x64_128(key.data(), key.size(), 42, h2);
    EXPECT_EQ(h1[0], h2[0]);
    EXPECT_EQ(h1[1], h2[1]);
}

TEST(MurmurHash3, X64_128_EmptyString) {
    uint64_t out[2] = {};
    murmurhash3_x64_128("", 0, 0, out);
    EXPECT_EQ(out[0], out[1]);
}

TEST(MurmurHash3, X86_128_Deterministic) {
    const std::string key = "hello world";
    uint32_t h1[4] = {}, h2[4] = {};
    murmurhash3_x86_128(key.data(), key.size(), 42, h1);
    murmurhash3_x86_128(key.data(), key.size(), 42, h2);
    EXPECT_EQ(h1[0], h2[0]);
    EXPECT_EQ(h1[1], h2[1]);
    EXPECT_EQ(h1[2], h2[2]);
    EXPECT_EQ(h1[3], h2[3]);
}
