#include "membership/member_list.h"

#include <set>
#include <gtest/gtest.h>

using namespace pensieve;

namespace {

NodeInfo make_node(const std::string& host, uint16_t port,
                   NodeState state = NodeState::Alive, uint64_t inc = 0) {
    NodeInfo info;
    info.id = {host, port};
    info.data_port = static_cast<uint16_t>(port + 1000);
    info.state = state;
    info.incarnation = inc;
    return info;
}

}  // namespace

TEST(MemberList, AddAndGet) {
    MemberList ml;
    auto n = make_node("10.0.0.1", 5000);
    EXPECT_TRUE(ml.add_node(n));
    EXPECT_EQ(ml.size(), 1u);

    auto got = ml.get_node(n.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, n.id);
    EXPECT_EQ(got->data_port, n.data_port);
}

TEST(MemberList, AddDuplicate) {
    MemberList ml;
    auto n = make_node("10.0.0.1", 5000);
    EXPECT_TRUE(ml.add_node(n));
    EXPECT_FALSE(ml.add_node(n));
    EXPECT_EQ(ml.size(), 1u);
}

TEST(MemberList, AddDuplicateHigherIncarnation) {
    MemberList ml;
    auto n1 = make_node("10.0.0.1", 5000, NodeState::Alive, 1);
    auto n2 = make_node("10.0.0.1", 5000, NodeState::Alive, 2);
    EXPECT_TRUE(ml.add_node(n1));
    EXPECT_TRUE(ml.add_node(n2));
    EXPECT_EQ(ml.size(), 1u);
    EXPECT_EQ(ml.get_node(n1.id)->incarnation, 2u);
}

TEST(MemberList, Remove) {
    MemberList ml;
    auto n = make_node("10.0.0.1", 5000);
    ml.add_node(n);
    EXPECT_TRUE(ml.remove_node(n.id));
    EXPECT_EQ(ml.size(), 0u);
    EXPECT_FALSE(ml.get_node(n.id).has_value());
}

TEST(MemberList, RemoveNonExistent) {
    MemberList ml;
    EXPECT_FALSE(ml.remove_node({"10.0.0.99", 5000}));
}

TEST(MemberList, UpdateState) {
    MemberList ml;
    auto n = make_node("10.0.0.1", 5000, NodeState::Alive, 1);
    ml.add_node(n);

    EXPECT_TRUE(ml.update_state(n.id, NodeState::Suspect, 1));
    EXPECT_EQ(ml.get_node(n.id)->state, NodeState::Suspect);
}

TEST(MemberList, UpdateStateHigherIncarnation) {
    MemberList ml;
    auto n = make_node("10.0.0.1", 5000, NodeState::Suspect, 1);
    ml.add_node(n);

    EXPECT_TRUE(ml.update_state(n.id, NodeState::Alive, 2));
    EXPECT_EQ(ml.get_node(n.id)->state, NodeState::Alive);
    EXPECT_EQ(ml.get_node(n.id)->incarnation, 2u);
}

TEST(MemberList, UpdateStateLowerIncarnationRejected) {
    MemberList ml;
    auto n = make_node("10.0.0.1", 5000, NodeState::Alive, 5);
    ml.add_node(n);

    EXPECT_FALSE(ml.update_state(n.id, NodeState::Suspect, 3));
    EXPECT_EQ(ml.get_node(n.id)->state, NodeState::Alive);
}

TEST(MemberList, UpdateStateSameIncarnationLowerPriorityRejected) {
    MemberList ml;
    auto n = make_node("10.0.0.1", 5000, NodeState::Suspect, 1);
    ml.add_node(n);

    EXPECT_FALSE(ml.update_state(n.id, NodeState::Alive, 1));
    EXPECT_EQ(ml.get_node(n.id)->state, NodeState::Suspect);
}

TEST(MemberList, RandomPeer) {
    MemberList ml;
    NodeId self{"10.0.0.1", 5000};
    ml.add_node(make_node("10.0.0.1", 5000));
    ml.add_node(make_node("10.0.0.2", 5000));
    ml.add_node(make_node("10.0.0.3", 5000));

    auto peer = ml.random_peer(self);
    ASSERT_TRUE(peer.has_value());
    EXPECT_NE(*peer, self);
}

TEST(MemberList, RandomPeerNoneAvailable) {
    MemberList ml;
    NodeId self{"10.0.0.1", 5000};
    ml.add_node(make_node("10.0.0.1", 5000));

    auto peer = ml.random_peer(self);
    EXPECT_FALSE(peer.has_value());
}

TEST(MemberList, RandomPeersK) {
    MemberList ml;
    NodeId self{"10.0.0.1", 5000};
    for (int i = 1; i <= 10; ++i) {
        ml.add_node(make_node("10.0.0." + std::to_string(i), 5000));
    }

    auto peers = ml.random_peers(3, self);
    EXPECT_EQ(peers.size(), 3u);
    std::set<NodeId> unique(peers.begin(), peers.end());
    EXPECT_EQ(unique.size(), 3u);
    for (const auto& p : peers) {
        EXPECT_NE(p, self);
    }
}

TEST(MemberList, AlivePeersExcludesDead) {
    MemberList ml;
    NodeId self{"10.0.0.1", 5000};
    ml.add_node(make_node("10.0.0.1", 5000));
    ml.add_node(make_node("10.0.0.2", 5000));
    ml.add_node(make_node("10.0.0.3", 5000, NodeState::Dead, 0));

    auto alive = ml.alive_peers(self);
    EXPECT_EQ(alive.size(), 1u);
    EXPECT_EQ(alive[0].host, "10.0.0.2");
}
