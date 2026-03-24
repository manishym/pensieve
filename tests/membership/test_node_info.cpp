#include "membership/node_info.h"

#include <unordered_set>
#include <gtest/gtest.h>

using namespace pensieve;

TEST(NodeId, Equality) {
    NodeId a{"127.0.0.1", 5000};
    NodeId b{"127.0.0.1", 5000};
    NodeId c{"127.0.0.1", 5001};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(NodeId, Ordering) {
    NodeId a{"127.0.0.1", 5000};
    NodeId b{"127.0.0.1", 5001};
    NodeId c{"127.0.0.2", 5000};

    EXPECT_LT(a, b);
    EXPECT_LT(a, c);
}

TEST(NodeId, HashableInUnorderedSet) {
    std::unordered_set<NodeId> set;
    set.insert({"127.0.0.1", 5000});
    set.insert({"127.0.0.1", 5001});
    set.insert({"127.0.0.1", 5000});

    EXPECT_EQ(set.size(), 2u);
}

TEST(NodeState, Supersedes) {
    EXPECT_FALSE(state_supersedes(NodeState::Alive, NodeState::Alive));
    EXPECT_TRUE(state_supersedes(NodeState::Suspect, NodeState::Alive));
    EXPECT_TRUE(state_supersedes(NodeState::Dead, NodeState::Alive));
    EXPECT_TRUE(state_supersedes(NodeState::Dead, NodeState::Suspect));
    EXPECT_FALSE(state_supersedes(NodeState::Alive, NodeState::Dead));
    EXPECT_FALSE(state_supersedes(NodeState::Suspect, NodeState::Dead));
}

TEST(NodeInfo, DefaultState) {
    NodeInfo info;
    info.id = {"10.0.0.1", 7000};
    EXPECT_EQ(info.state, NodeState::Alive);
    EXPECT_EQ(info.incarnation, 0u);
}
