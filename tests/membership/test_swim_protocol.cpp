#include "membership/swim_protocol.h"

#include <thread>
#include <gtest/gtest.h>

using namespace pensieve;
using namespace std::chrono_literals;

namespace {

struct SwimNode {
    IoUringContext ctx{128};
    UdpSocket sock;
    MemberList members;
    Disseminator disseminator{10};
    NodeId id;
    std::unique_ptr<SwimProtocol> protocol;
    std::thread runner;

    SwimNode(const std::string& host, uint16_t port,
             SwimProtocol::Config config)
        : sock(host, port), id{host, sock.port()} {
        NodeInfo self_info;
        self_info.id = id;
        self_info.state = NodeState::Alive;
        members.add_node(self_info);

        protocol = std::make_unique<SwimProtocol>(ctx, sock, members,
                                                  disseminator, id, config);
    }

    void start() {
        protocol->run();
        runner = std::thread([this] { ctx.run(); });
    }

    void stop_and_join() {
        protocol->stop();
        ctx.stop();
        if (runner.joinable()) runner.join();
        // Destroy coroutine frames while the io_uring ring is still valid
        protocol.reset();
    }

    void add_peer(const NodeId& peer_id) {
        NodeInfo info;
        info.id = peer_id;
        info.state = NodeState::Alive;
        members.add_node(info);
    }
};

SwimProtocol::Config fast_config() {
    SwimProtocol::Config c;
    c.protocol_period = std::chrono::milliseconds(50);
    c.ping_timeout = std::chrono::milliseconds(30);
    c.suspect_timeout = std::chrono::milliseconds(300);
    c.indirect_ping_peers = 2;
    return c;
}

}  // namespace

TEST(SwimProtocol, PingAckRoundTrip) {
    auto config = fast_config();
    config.suspect_timeout = std::chrono::milliseconds(2000);

    SwimNode a("127.0.0.1", 0, config);
    SwimNode b("127.0.0.1", 0, config);

    a.add_peer(b.id);
    b.add_peer(a.id);

    a.start();
    b.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    a.stop_and_join();
    b.stop_and_join();

    auto a_view = a.members.get_node(b.id);
    ASSERT_TRUE(a_view.has_value());
    EXPECT_EQ(a_view->state, NodeState::Alive);

    auto b_view = b.members.get_node(a.id);
    ASSERT_TRUE(b_view.has_value());
    EXPECT_EQ(b_view->state, NodeState::Alive);
}

TEST(SwimProtocol, SuspectOnTimeout) {
    auto config = fast_config();
    config.suspect_timeout = std::chrono::milliseconds(2000);

    SwimNode a("127.0.0.1", 0, config);
    SwimNode b("127.0.0.1", 0, config);

    a.add_peer(b.id);

    a.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    a.stop_and_join();

    auto view = a.members.get_node(b.id);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->state, NodeState::Suspect);
}

TEST(SwimProtocol, DeadAfterSuspectTimeout) {
    auto config = fast_config();

    SwimNode a("127.0.0.1", 0, config);
    SwimNode b("127.0.0.1", 0, config);

    a.add_peer(b.id);

    a.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    a.stop_and_join();

    auto view = a.members.get_node(b.id);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->state, NodeState::Dead);
}

TEST(SwimProtocol, ThreeNodeCluster) {
    auto config = fast_config();
    config.suspect_timeout = std::chrono::milliseconds(2000);

    SwimNode a("127.0.0.1", 0, config);
    SwimNode b("127.0.0.1", 0, config);
    SwimNode c("127.0.0.1", 0, config);

    a.add_peer(b.id);
    a.add_peer(c.id);
    b.add_peer(a.id);
    b.add_peer(c.id);
    c.add_peer(a.id);
    c.add_peer(b.id);

    a.start();
    b.start();
    c.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    a.stop_and_join();
    b.stop_and_join();
    c.stop_and_join();

    EXPECT_EQ(a.members.get_node(b.id)->state, NodeState::Alive);
    EXPECT_EQ(a.members.get_node(c.id)->state, NodeState::Alive);
    EXPECT_EQ(b.members.get_node(a.id)->state, NodeState::Alive);
    EXPECT_EQ(c.members.get_node(a.id)->state, NodeState::Alive);
}

TEST(SwimProtocol, DetectNodeFailure) {
    auto config = fast_config();

    SwimNode a("127.0.0.1", 0, config);
    SwimNode b("127.0.0.1", 0, config);
    SwimNode c("127.0.0.1", 0, config);

    a.add_peer(b.id);
    a.add_peer(c.id);
    b.add_peer(a.id);
    b.add_peer(c.id);
    c.add_peer(a.id);
    c.add_peer(b.id);

    a.start();
    b.start();
    c.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    c.stop_and_join();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    a.stop_and_join();
    b.stop_and_join();

    auto a_view_c = a.members.get_node(c.id);
    ASSERT_TRUE(a_view_c.has_value());
    EXPECT_NE(a_view_c->state, NodeState::Alive);

    auto b_view_c = b.members.get_node(c.id);
    ASSERT_TRUE(b_view_c.has_value());
    EXPECT_NE(b_view_c->state, NodeState::Alive);
}
