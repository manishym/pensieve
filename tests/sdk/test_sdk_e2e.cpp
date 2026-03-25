#include "coordinator/coordinator.h"
#include "io/awaitable.h"
#include "io/buffer_pool.h"
#include "io/io_uring_context.h"
#include "io/task.h"
#include "membership/member_list.h"
#include "membership/ring_store.h"
#include "protocol/message.h"
#include "sdk/pensieve_client.h"
#include "sdk/topology_manager.h"
#include "storage/local_store.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace pensieve;

namespace {

uint16_t get_bound_port(fd_t listen_fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

fd_t make_listen_socket() {
    fd_t fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(fd, SOMAXCONN);
    return fd;
}

struct ServerFixture {
    IoUringContext ctx{128};
    LocalStore store{4 * 1024 * 1024};
    MemberList members;
    RingStore ring;
    NodeId self{"127.0.0.1", 7000};
    std::unique_ptr<Coordinator> coord;
    fd_t listen_fd = -1;
    uint16_t port = 0;
    std::thread server_thread;
    std::vector<Task<>> connections;
    Task<> accept_task;

    void start(uint16_t /*unused*/) {
        listen_fd = make_listen_socket();
        port = get_bound_port(listen_fd);

        NodeInfo info;
        info.id = self;
        info.data_port = port;
        info.state = NodeState::Alive;
        members.add_node(info);
        ring.add_node(self, 128);

        coord = std::make_unique<Coordinator>(ctx, store, ring, members,
                                              self);

        accept_task = accept_loop();
        accept_task.start();

        server_thread = std::thread([this] { ctx.run(); });
    }

    Task<> accept_loop() {
        while (true) {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int32_t client_fd =
                co_await async_accept(ctx, listen_fd, &client_addr,
                                      &addr_len);
            if (client_fd < 0) break;
            auto task = coord->handle_connection(client_fd);
            task.start();
            connections.push_back(std::move(task));
            std::erase_if(connections,
                          [](const Task<>& t) { return t.done(); });
        }
    }

    void stop() {
        ctx.stop();
        if (server_thread.joinable()) server_thread.join();
        if (listen_fd >= 0) ::close(listen_fd);
    }

    ~ServerFixture() { stop(); }
};

}  // namespace

TEST(SdkE2E, ClusterInfoReturnsNodes) {
    ServerFixture srv;
    srv.start(0);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    std::vector<NodeEndpoint> nodes;
    bool ok = TopologyManager::fetch_topology("127.0.0.1", srv.port, nodes);
    ASSERT_TRUE(ok);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].host, "127.0.0.1");
    EXPECT_EQ(nodes[0].data_port, srv.port);
}

TEST(SdkE2E, TopologyBootstrapAndRingLookup) {
    ServerFixture srv;
    srv.start(0);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    TopologyManager topo;
    ASSERT_TRUE(topo.bootstrap("127.0.0.1", srv.port));
    EXPECT_EQ(topo.node_count(), 1u);

    auto owner = topo.find_owner("test-key");
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(owner->host, "127.0.0.1");
    EXPECT_EQ(owner->data_port, srv.port);
}

TEST(SdkE2E, ClientPutGetDel) {
    ServerFixture srv;
    srv.start(0);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    PensieveClient::Config cfg;
    cfg.seed_host = "127.0.0.1";
    cfg.seed_port = srv.port;
    cfg.refresh_interval = std::chrono::seconds{0};

    PensieveClient client(cfg);
    ASSERT_TRUE(client.connect());

    EXPECT_TRUE(client.put("hello", "world"));
    auto val = client.get("hello");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "world");

    EXPECT_TRUE(client.del("hello"));
    val = client.get("hello");
    EXPECT_FALSE(val.has_value());
}

TEST(SdkE2E, ClientManyKeys) {
    ServerFixture srv;
    srv.start(0);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    PensieveClient::Config cfg;
    cfg.seed_host = "127.0.0.1";
    cfg.seed_port = srv.port;
    cfg.refresh_interval = std::chrono::seconds{0};

    PensieveClient client(cfg);
    ASSERT_TRUE(client.connect());

    for (int i = 0; i < 100; ++i) {
        std::string key = "key-" + std::to_string(i);
        std::string val = "val-" + std::to_string(i);
        EXPECT_TRUE(client.put(key, val));
    }

    for (int i = 0; i < 100; ++i) {
        std::string key = "key-" + std::to_string(i);
        std::string expected = "val-" + std::to_string(i);
        auto val = client.get(key);
        ASSERT_TRUE(val.has_value()) << "key=" << key;
        EXPECT_EQ(*val, expected);
    }
}

TEST(SdkE2E, ClusterInfoPrintable) {
    ServerFixture srv;
    srv.start(0);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    PensieveClient::Config cfg;
    cfg.seed_host = "127.0.0.1";
    cfg.seed_port = srv.port;
    cfg.refresh_interval = std::chrono::seconds{0};

    PensieveClient client(cfg);
    ASSERT_TRUE(client.connect());

    std::string info = client.cluster_info();
    EXPECT_NE(info.find("1 node"), std::string::npos);
    EXPECT_NE(info.find("127.0.0.1"), std::string::npos);
}

TEST(SdkE2E, StatsCountRequests) {
    ServerFixture srv;
    srv.start(0);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    PensieveClient::Config cfg;
    cfg.seed_host = "127.0.0.1";
    cfg.seed_port = srv.port;
    cfg.refresh_interval = std::chrono::seconds{0};

    PensieveClient client(cfg);
    ASSERT_TRUE(client.connect());

    client.put("a", "1");
    client.get("a");
    client.del("a");

    auto s = client.stats();
    EXPECT_EQ(s.requests_total, 3u);
    EXPECT_GE(s.requests_direct, 3u);
    EXPECT_GT(s.last_latency_us, 0u);
}
