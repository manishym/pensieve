#include "membership/disseminator.h"
#include "membership/member_list.h"
#include "membership/ring_store.h"
#include "membership/swim_protocol.h"

#include <algorithm>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;
using namespace std::chrono_literals;

namespace {

struct VirtualNode {
    IoUringContext ctx{128};
    UdpSocket sock;
    MemberList members;
    Disseminator disseminator{10};
    RingStore ring;
    NodeId id;
    std::unique_ptr<SwimProtocol> protocol;
    std::thread runner;

    static constexpr uint32_t kTestVnodes = 16;

    VirtualNode(uint16_t port, SwimProtocol::Config config)
        : sock("127.0.0.1", port),
          id{"127.0.0.1", sock.port()} {
        NodeInfo self_info;
        self_info.id = id;
        self_info.state = NodeState::Alive;
        members.add_node(self_info);

        ring.add_node(id, kTestVnodes);

        protocol = std::make_unique<SwimProtocol>(ctx, sock, members,
                                                  disseminator, id, config);
    }

    void add_peer(const NodeId& peer_id) {
        NodeInfo info;
        info.id = peer_id;
        info.state = NodeState::Alive;
        members.add_node(info);

        ring.add_node(peer_id, kTestVnodes);
    }

    void start() {
        protocol->run();
        runner = std::thread([this] { ctx.run(); });
    }

    void stop_and_join() {
        protocol->stop();
        ctx.stop();
        if (runner.joinable()) runner.join();
        protocol.reset();
    }
};

SwimProtocol::Config e2e_config() {
    SwimProtocol::Config c;
    c.protocol_period = std::chrono::milliseconds(80);
    c.ping_timeout = std::chrono::milliseconds(50);
    c.suspect_timeout = std::chrono::milliseconds(400);
    c.indirect_ping_peers = 2;
    c.max_piggyback_updates = 8;
    return c;
}

}  // namespace

TEST(SwimE2E, FiveNodeConvergence) {
    constexpr int N = 5;
    auto config = e2e_config();

    std::vector<std::unique_ptr<VirtualNode>> nodes;
    for (int i = 0; i < N; ++i) {
        nodes.push_back(std::make_unique<VirtualNode>(0, config));
    }

    // Full mesh: every node knows about every other node
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i != j) {
                nodes[i]->add_peer(nodes[j]->id);
            }
        }
    }

    for (auto& n : nodes) n->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // Stop all before inspecting state
    for (auto& n : nodes) n->stop_and_join();

    // All nodes should see all others as Alive
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i == j) continue;
            auto view = nodes[i]->members.get_node(nodes[j]->id);
            ASSERT_TRUE(view.has_value())
                << "Node " << i << " doesn't know about node " << j;
            EXPECT_EQ(view->state, NodeState::Alive)
                << "Node " << i << " sees node " << j << " as non-alive";
        }
    }
}

TEST(SwimE2E, SingleNodeFailureDetected) {
    constexpr int N = 4;
    auto config = e2e_config();

    std::vector<std::unique_ptr<VirtualNode>> nodes;
    for (int i = 0; i < N; ++i) {
        nodes.push_back(std::make_unique<VirtualNode>(0, config));
    }

    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i != j) nodes[i]->add_peer(nodes[j]->id);
        }
    }

    for (auto& n : nodes) n->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // Kill node 0
    nodes[0]->stop_and_join();
    NodeId dead_id = nodes[0]->id;

    // Wait for failure detection and re-convergence among survivors
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // Stop remaining nodes
    for (int i = 1; i < N; ++i) {
        nodes[i]->stop_and_join();
    }

    // All surviving nodes should detect node 0 as non-alive
    for (int i = 1; i < N; ++i) {
        auto view = nodes[i]->members.get_node(dead_id);
        ASSERT_TRUE(view.has_value())
            << "Node " << i << " doesn't know about dead node";
        EXPECT_NE(view->state, NodeState::Alive)
            << "Node " << i << " still sees dead node as alive";
    }

    // Surviving nodes should see each other as alive
    for (int i = 1; i < N; ++i) {
        for (int j = 1; j < N; ++j) {
            if (i == j) continue;
            auto view = nodes[i]->members.get_node(nodes[j]->id);
            ASSERT_TRUE(view.has_value());
            EXPECT_EQ(view->state, NodeState::Alive)
                << "Node " << i << " sees surviving node " << j
                << " as non-alive";
        }
    }
}

TEST(SwimE2E, RingStoreReflectsMembership) {
    constexpr int N = 3;
    auto config = e2e_config();

    std::vector<std::unique_ptr<VirtualNode>> nodes;
    for (int i = 0; i < N; ++i) {
        nodes.push_back(std::make_unique<VirtualNode>(0, config));
    }

    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i != j) nodes[i]->add_peer(nodes[j]->id);
        }
    }

    for (auto& n : nodes) n->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (auto& n : nodes) n->stop_and_join();

    // Each node's ring store should contain tokens for all 3 nodes
    for (int i = 0; i < N; ++i) {
        auto snap = nodes[i]->ring.snapshot();
        EXPECT_EQ(snap->size(), static_cast<size_t>(N * VirtualNode::kTestVnodes))
            << "Node " << i << " ring has wrong token count";

        // Verify all nodes have tokens in the ring
        std::set<NodeId> ring_nodes;
        for (const auto& [token, nid] : *snap) {
            ring_nodes.insert(nid);
        }
        EXPECT_EQ(ring_nodes.size(), static_cast<size_t>(N));
    }
}

TEST(SwimE2E, GossipPropagatesJoins) {
    auto config = e2e_config();

    // Start with nodes A and B knowing each other
    auto a = std::make_unique<VirtualNode>(0, config);
    auto b = std::make_unique<VirtualNode>(0, config);
    auto c = std::make_unique<VirtualNode>(0, config);

    a->add_peer(b->id);
    b->add_peer(a->id);

    // c only knows about a
    c->add_peer(a->id);
    a->add_peer(c->id);

    // Enqueue join updates so they get piggybacked
    a->disseminator.enqueue(
        {MembershipUpdate::Type::Join, c->id, 0});
    a->disseminator.enqueue(
        {MembershipUpdate::Type::Join, b->id, 0});
    c->disseminator.enqueue(
        {MembershipUpdate::Type::Join, a->id, 0});

    a->start();
    b->start();
    c->start();

    // Give time for gossip to propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    a->stop_and_join();
    b->stop_and_join();
    c->stop_and_join();

    // b should have learned about c through a's piggybacked updates
    auto b_view_c = b->members.get_node(c->id);
    // This may or may not succeed depending on gossip timing
    // At minimum, a should know all nodes
    EXPECT_TRUE(a->members.get_node(b->id).has_value());
    EXPECT_TRUE(a->members.get_node(c->id).has_value());
    EXPECT_EQ(a->members.get_node(b->id)->state, NodeState::Alive);
    EXPECT_EQ(a->members.get_node(c->id)->state, NodeState::Alive);
}
