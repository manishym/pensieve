#include "hash/hasher.h"

#include <set>
#include <string>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

TEST(Hasher, HashKeyDeterministic) {
    auto h1 = hash_key("my-cache-key");
    auto h2 = hash_key("my-cache-key");
    EXPECT_EQ(h1, h2);
}

TEST(Hasher, HashKeyDifferentInputs) {
    auto h1 = hash_key("key-alpha");
    auto h2 = hash_key("key-beta");
    EXPECT_NE(h1, h2);
}

TEST(Hasher, HashKeyEmptyString) {
    auto h = hash_key("");
    // Should not crash; any value is valid
    (void)h;
}

TEST(Hasher, HashNodeTokenDeterministic) {
    NodeId node{"10.0.0.1", 5000};
    auto h1 = hash_node_token(node, 0);
    auto h2 = hash_node_token(node, 0);
    EXPECT_EQ(h1, h2);
}

TEST(Hasher, HashNodeTokenDifferentIndices) {
    NodeId node{"10.0.0.1", 5000};
    std::set<uint32_t> tokens;
    for (uint32_t i = 0; i < 256; ++i) {
        tokens.insert(hash_node_token(node, i));
    }
    // 256 tokens should produce at least 250 unique values
    EXPECT_GE(tokens.size(), 250u);
}

TEST(Hasher, HashNodeTokenDifferentNodes) {
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};
    auto ha = hash_node_token(a, 0);
    auto hb = hash_node_token(b, 0);
    EXPECT_NE(ha, hb);
}

TEST(Hasher, HashNodeTokenDifferentPorts) {
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.1", 5001};
    auto ha = hash_node_token(a, 0);
    auto hb = hash_node_token(b, 0);
    EXPECT_NE(ha, hb);
}

TEST(Hasher, HashKeyDistribution) {
    constexpr int kNumKeys = 10000;
    constexpr int kBuckets = 16;
    std::vector<int> counts(kBuckets, 0);

    for (int i = 0; i < kNumKeys; ++i) {
        auto h = hash_key("user:" + std::to_string(i));
        counts[h % kBuckets]++;
    }

    double expected = static_cast<double>(kNumKeys) / kBuckets;
    for (int c : counts) {
        EXPECT_GT(c, static_cast<int>(expected * 0.7));
        EXPECT_LT(c, static_cast<int>(expected * 1.3));
    }
}

TEST(Hasher, HashNodeTokenDistribution) {
    NodeId node{"192.168.1.100", 7000};
    constexpr int kTokens = 1024;
    constexpr int kQuadrants = 4;
    std::vector<int> counts(kQuadrants, 0);
    uint32_t quadrant_size = UINT32_MAX / kQuadrants;

    for (uint32_t i = 0; i < kTokens; ++i) {
        auto h = hash_node_token(node, i);
        counts[h / quadrant_size]++;
    }

    double expected = static_cast<double>(kTokens) / kQuadrants;
    for (int c : counts) {
        EXPECT_GT(c, static_cast<int>(expected * 0.5));
        EXPECT_LT(c, static_cast<int>(expected * 1.5));
    }
}
