#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <netdb.h>
#include <string>

#include "common/types.h"
#include "coordinator/coordinator.h"
#include "io/awaitable.h"
#include "io/buffer_pool.h"
#include "io/io_uring_context.h"
#include "io/task.h"
#include "io/tcp_connection.h"
#include "io/tcp_listener.h"
#include "io/udp_socket.h"
#include "membership/disseminator.h"
#include "membership/member_list.h"
#include "membership/node_info.h"
#include "membership/ring_store.h"
#include "membership/swim_message.h"
#include "membership/swim_protocol.h"
#include "storage/local_store.h"

using namespace pensieve;

static IoUringContext* g_ctx = nullptr;

static void signal_handler(int) {
    if (g_ctx) g_ctx->stop();
}

static std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

static std::string resolve_host(const std::string& host) {
    sockaddr_in test{};
    if (inet_pton(AF_INET, host.c_str(), &test.sin_addr) == 1)
        return host;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
        std::cerr << "failed to resolve host: " << host << "\n";
        return host;
    }
    char buf[INET_ADDRSTRLEN]{};
    auto* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
    freeaddrinfo(result);
    return buf;
}

int main(int argc, char* argv[]) {
    std::string host     = env_or("PENSIEVE_HOST",        argc > 1 ? argv[1] : "0.0.0.0");
    uint16_t data_port   = static_cast<uint16_t>(std::stoi(env_or("PENSIEVE_DATA_PORT",   argc > 2 ? argv[2] : "11211")));
    uint16_t gossip_port = static_cast<uint16_t>(std::stoi(env_or("PENSIEVE_GOSSIP_PORT", argc > 3 ? argv[3] : "7946")));
    size_t   memory_mb   = std::stoul(env_or("PENSIEVE_MEMORY_MB", argc > 4 ? argv[4] : "64"));

    std::string seed_str = env_or("PENSIEVE_SEED", argc > 5 ? argv[5] : "");
    uint16_t seed_data_port = static_cast<uint16_t>(std::stoi(
        env_or("PENSIEVE_SEED_DATA_PORT", std::to_string(data_port).c_str())));

    std::string resolved_host = resolve_host(host);

    IoUringContext ctx(1024);
    g_ctx = &ctx;
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    BufferPool pool(ctx, 65536, 64);
    LocalStore store(memory_mb * 1024 * 1024, 16);

    NodeId self{resolved_host, gossip_port};
    MemberList members;
    RingStore ring;
    Disseminator disseminator;

    // Keep RingStore in sync with MemberList: add on Alive, remove on Dead/Left.
    members.set_on_change([&ring](const NodeId& id, const NodeInfo& info) {
        if (info.state == NodeState::Alive) {
            ring.add_node(id);
        } else if (info.state == NodeState::Dead ||
                   info.state == NodeState::Left) {
            ring.remove_node(id);
        }
    });

    NodeInfo self_info;
    self_info.id = self;
    self_info.data_port = data_port;
    self_info.state = NodeState::Alive;
    members.add_node(self_info);

    if (!seed_str.empty()) {
        std::string seed_host;
        uint16_t seed_gossip_port = gossip_port;

        auto colon = seed_str.rfind(':');
        if (colon != std::string::npos) {
            seed_host = seed_str.substr(0, colon);
            seed_gossip_port = static_cast<uint16_t>(
                std::stoi(seed_str.substr(colon + 1)));
        } else {
            seed_host = seed_str;
        }
        seed_host = resolve_host(seed_host);

        NodeId seed_id{seed_host, seed_gossip_port};
        NodeInfo seed_info;
        seed_info.id = seed_id;
        seed_info.data_port = seed_data_port;
        seed_info.state = NodeState::Alive;
        members.add_node(seed_info);

        std::cout << "seed: " << seed_host << ":"
                  << seed_gossip_port << " (data " << seed_data_port << ")\n";
    }

    // Announce ourselves so gossip propagates our identity + data_port.
    disseminator.enqueue({MembershipUpdate::Type::Join, self, 0, data_port});

    UdpSocket gossip_sock(resolved_host, gossip_port);
    SwimProtocol::Config swim_cfg;
    swim_cfg.self_data_port = data_port;
    SwimProtocol swim(ctx, gossip_sock, members, disseminator, self, swim_cfg);

    Coordinator coordinator(ctx, store, ring, members, self, &pool);

    std::vector<Task<>> connections;

    TcpListener listener(ctx, resolved_host, data_port);
    listener.start([&](TcpConnection conn) {
        auto task = coordinator.handle_connection(conn.release());
        task.start();
        connections.push_back(std::move(task));
        std::erase_if(connections, [](const Task<>& t) { return t.done(); });
    });

    swim.run();
    std::cout << "pensieve node " << resolved_host
              << " data=" << data_port
              << " gossip=" << gossip_port
              << " memory=" << memory_mb << "MB\n";
    ctx.run();

    swim.stop();
    std::cout << "pensieve shutdown complete\n";
    return 0;
}
