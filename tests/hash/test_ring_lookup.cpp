#include "membership/ring_store.h"

#include <set>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

TEST(RingLookup, EmptyRingReturnsNullopt) {
    RingStore rs;
    EXPECT_FALSE(rs.get_node_for_key(42).has_value());
}

TEST(RingLookup, SingleNodeOwnsAllKeys) {
    RingStore rs;
    NodeId node{"10.0.0.1", 5000};
    rs.add_node(node, std::vector<uint32_t>{100, 500, 900});

    EXPECT_EQ(rs.get_node_for_key(0), node);
    EXPECT_EQ(rs.get_node_for_key(100), node);
    EXPECT_EQ(rs.get_node_for_key(250), node);
    EXPECT_EQ(rs.get_node_for_key(900), node);
    EXPECT_EQ(rs.get_node_for_key(UINT32_MAX), node);
}

TEST(RingLookup, TwoNodesPartitionSpace) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    // a owns token 100, b owns token 500
    rs.add_node(a, std::vector<uint32_t>{100});
    rs.add_node(b, std::vector<uint32_t>{500});

    // Keys at or after 100 but before 500 go to b (next clockwise)
    EXPECT_EQ(rs.get_node_for_key(100), a);
    EXPECT_EQ(rs.get_node_for_key(200), b);
    EXPECT_EQ(rs.get_node_for_key(499), b);
    EXPECT_EQ(rs.get_node_for_key(500), b);

    // Keys after 500 wrap around to a
    EXPECT_EQ(rs.get_node_for_key(501), a);
    EXPECT_EQ(rs.get_node_for_key(UINT32_MAX), a);

    // Key 0 wraps around to a (first token clockwise)
    EXPECT_EQ(rs.get_node_for_key(0), a);
    EXPECT_EQ(rs.get_node_for_key(99), a);
}

TEST(RingLookup, WrapAroundToBegin) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    rs.add_node(a, std::vector<uint32_t>{1000});
    rs.add_node(b, std::vector<uint32_t>{2000});

    // Key beyond all tokens wraps to begin (a at 1000)
    EXPECT_EQ(rs.get_node_for_key(3000), a);
    EXPECT_EQ(rs.get_node_for_key(UINT32_MAX), a);
}

TEST(RingLookup, ExactTokenMatch) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    rs.add_node(a, std::vector<uint32_t>{100});
    rs.add_node(b, std::vector<uint32_t>{200});

    // lower_bound(100) == token 100 => a
    EXPECT_EQ(rs.get_node_for_key(100), a);
    // lower_bound(200) == token 200 => b
    EXPECT_EQ(rs.get_node_for_key(200), b);
}

TEST(RingLookup, ThreeNodesClockwise) {
    RingStore rs;
    NodeId a{"a", 1};
    NodeId b{"b", 1};
    NodeId c{"c", 1};

    rs.add_node(a, std::vector<uint32_t>{100});
    rs.add_node(b, std::vector<uint32_t>{200});
    rs.add_node(c, std::vector<uint32_t>{300});

    EXPECT_EQ(rs.get_node_for_key(50), a);
    EXPECT_EQ(rs.get_node_for_key(100), a);
    EXPECT_EQ(rs.get_node_for_key(150), b);
    EXPECT_EQ(rs.get_node_for_key(200), b);
    EXPECT_EQ(rs.get_node_for_key(250), c);
    EXPECT_EQ(rs.get_node_for_key(300), c);
    EXPECT_EQ(rs.get_node_for_key(350), a);  // wraps
}

// --- get_n_nodes_for_key tests ---

TEST(RingLookupN, EmptyRingReturnsEmpty) {
    RingStore rs;
    auto result = rs.get_n_nodes_for_key(42, 3);
    EXPECT_TRUE(result.empty());
}

TEST(RingLookupN, SingleNodeReturnsOne) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    rs.add_node(a, std::vector<uint32_t>{100, 200, 300});

    auto result = rs.get_n_nodes_for_key(0, 3);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], a);
}

TEST(RingLookupN, TwoNodesReturnsBoth) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    rs.add_node(a, std::vector<uint32_t>{100, 300});
    rs.add_node(b, std::vector<uint32_t>{200, 400});

    auto result = rs.get_n_nodes_for_key(50, 2);
    ASSERT_EQ(result.size(), 2u);

    // First hit is a (token 100), second distinct is b (token 200)
    EXPECT_EQ(result[0], a);
    EXPECT_EQ(result[1], b);
}

TEST(RingLookupN, SkipsDuplicateVnodes) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    // a has consecutive tokens 100, 200; b has 300
    rs.add_node(a, std::vector<uint32_t>{100, 200});
    rs.add_node(b, std::vector<uint32_t>{300});

    auto result = rs.get_n_nodes_for_key(50, 2);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], a);
    EXPECT_EQ(result[1], b);
}

TEST(RingLookupN, RequestMoreThanAvailable) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    rs.add_node(a, std::vector<uint32_t>{100});
    rs.add_node(b, std::vector<uint32_t>{200});

    auto result = rs.get_n_nodes_for_key(50, 5);
    ASSERT_EQ(result.size(), 2u);
}

TEST(RingLookupN, WrapAroundCollectsAll) {
    RingStore rs;
    NodeId a{"a", 1};
    NodeId b{"b", 1};
    NodeId c{"c", 1};

    rs.add_node(a, std::vector<uint32_t>{100});
    rs.add_node(b, std::vector<uint32_t>{200});
    rs.add_node(c, std::vector<uint32_t>{300});

    // Start from 250 -> c(300) -> a(100 wrap) -> b(200 wrap)
    auto result = rs.get_n_nodes_for_key(250, 3);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], c);
    EXPECT_EQ(result[1], a);
    EXPECT_EQ(result[2], b);
}

TEST(RingLookupN, WithVirtualNodes) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};
    NodeId c{"10.0.0.3", 5000};

    rs.add_node(a, 32);
    rs.add_node(b, 32);
    rs.add_node(c, 32);

    auto result = rs.get_n_nodes_for_key(0, 3);
    ASSERT_EQ(result.size(), 3u);

    std::set<NodeId> unique(result.begin(), result.end());
    EXPECT_EQ(unique.size(), 3u);
}
