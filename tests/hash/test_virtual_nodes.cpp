#include "hash/hasher.h"
#include "membership/ring_store.h"

#include <cmath>
#include <set>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

TEST(VirtualNodes, CorrectTokenCount) {
    RingStore rs;
    NodeId node{"10.0.0.1", 5000};
    rs.add_node(node, 128);
    EXPECT_EQ(rs.size(), 128u);
}

TEST(VirtualNodes, DeterministicPlacement) {
    RingStore rs1, rs2;
    NodeId node{"10.0.0.1", 5000};
    rs1.add_node(node, 64);
    rs2.add_node(node, 64);

    auto s1 = rs1.snapshot();
    auto s2 = rs2.snapshot();
    EXPECT_EQ(*s1, *s2);
}

TEST(VirtualNodes, DifferentNodesGetDifferentTokens) {
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    std::set<uint32_t> tokens_a, tokens_b;
    for (uint32_t i = 0; i < 128; ++i) {
        tokens_a.insert(hash_node_token(a, i));
        tokens_b.insert(hash_node_token(b, i));
    }

    // Tokens for different nodes should have minimal overlap
    size_t overlap = 0;
    for (auto t : tokens_a) {
        if (tokens_b.count(t)) ++overlap;
    }
    EXPECT_LT(overlap, 3u);
}

TEST(VirtualNodes, MultipleNodesOnRing) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};
    NodeId c{"10.0.0.3", 5000};

    rs.add_node(a, 128);
    rs.add_node(b, 128);
    rs.add_node(c, 128);

    auto snap = rs.snapshot();
    EXPECT_EQ(snap->size(), 384u);

    std::set<NodeId> present;
    for (const auto& [token, nid] : *snap) {
        present.insert(nid);
    }
    EXPECT_EQ(present.size(), 3u);
}

TEST(VirtualNodes, UniformDistribution) {
    RingStore rs;
    constexpr int kNodes = 5;
    constexpr uint32_t kVnodes = 256;

    std::vector<NodeId> nodes;
    for (int i = 0; i < kNodes; ++i) {
        NodeId nid{"10.0.0." + std::to_string(i + 1), 5000};
        nodes.push_back(nid);
        rs.add_node(nid, kVnodes);
    }

    // Assign 100k keys and count distribution
    constexpr int kKeys = 100000;
    std::vector<int> counts(kNodes, 0);

    auto snap = rs.snapshot();
    for (int i = 0; i < kKeys; ++i) {
        std::string key = "key-" + std::to_string(i);
        uint32_t h = hash_key(key);

        auto it = snap->lower_bound(h);
        if (it == snap->end()) it = snap->begin();
        const auto& owner = it->second;

        for (int n = 0; n < kNodes; ++n) {
            if (nodes[n] == owner) {
                counts[n]++;
                break;
            }
        }
    }

    double expected = static_cast<double>(kKeys) / kNodes;
    for (int n = 0; n < kNodes; ++n) {
        double ratio = counts[n] / expected;
        EXPECT_GT(ratio, 0.5) << "Node " << n << " has too few keys";
        EXPECT_LT(ratio, 1.5) << "Node " << n << " has too many keys";
    }
}

TEST(VirtualNodes, RemoveNodeCleansAllVnodes) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    rs.add_node(a, 128);
    rs.add_node(b, 128);
    EXPECT_EQ(rs.size(), 256u);

    rs.remove_node(a);
    EXPECT_EQ(rs.size(), 128u);

    auto snap = rs.snapshot();
    for (const auto& [token, nid] : *snap) {
        EXPECT_EQ(nid, b);
    }
}

TEST(VirtualNodes, DefaultVnodeCount) {
    RingStore rs;
    NodeId node{"10.0.0.1", 5000};
    rs.add_node(node);
    EXPECT_EQ(rs.size(), RingStore::kDefaultVnodes);
}
