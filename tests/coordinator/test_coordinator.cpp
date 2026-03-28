#include "coordinator/coordinator.h"
#include "hash/hasher.h"
#include "io/awaitable.h"
#include "io/buffer_pool.h"
#include "io/io_uring_context.h"
#include "io/task.h"
#include "membership/member_list.h"
#include "membership/ring_store.h"
#include "protocol/message.h"
#include "storage/local_store.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

int blocking_connect(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    for (int i = 0; i < 50; ++i) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    return -1;
}

void send_request(fd_t fd, const Request& req) {
    auto wire = serialize_request(req);
    size_t off = 0;
    while (off < wire.size()) {
        ssize_t n = ::write(fd, wire.data() + off, wire.size() - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

Response recv_response(fd_t fd) {
    MemHeader hdr{};
    size_t off = 0;
    while (off < sizeof(hdr)) {
        ssize_t n = ::read(fd, reinterpret_cast<char*>(&hdr) + off,
                           sizeof(hdr) - off);
        if (n <= 0) return Response{Status::Error, {}};
        off += static_cast<size_t>(n);
    }

    uint32_t body_len = be32toh(hdr.body_len);
    std::string value;
    if (body_len > 0) {
        value.resize(body_len);
        off = 0;
        while (off < body_len) {
            ssize_t n = ::read(fd, value.data() + off, body_len - off);
            if (n <= 0) return Response{Status::Error, {}};
            off += static_cast<size_t>(n);
        }
    }
    return Response{static_cast<Status>(be16toh(hdr.vbucket)), std::move(value), be32toh(hdr.opaque), be64toh(hdr.cas)};
}

}  // namespace

class CoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        self_ = NodeId{"127.0.0.1", 7000};
        members_.add_node(NodeInfo{self_, 8000});
        ring_.add_node(self_, 128);
    }

    NodeId self_;
    MemberList members_;
    RingStore ring_;
};

TEST_F(CoordinatorTest, LocalPutAndGet) {
    IoUringContext ctx(128);
    LocalStore store(4 * 1024 * 1024);
    Coordinator coord(ctx, store, ring_, members_, self_);

    fd_t listen_fd = make_listen_socket();
    uint16_t port = get_bound_port(listen_fd);
    std::atomic<bool> done{false};

    auto server = [&]() -> Task<> {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int32_t client_fd =
            co_await async_accept(ctx, listen_fd, &client_addr, &addr_len);
        if (client_fd >= 0) {
            co_await coord.handle_connection(client_fd);
        }
        ctx.stop();
    };

    auto task = server();
    task.start();

    std::thread client([port, &done] {
        int fd = blocking_connect(port);
        ASSERT_GE(fd, 0);

        // SET
        send_request(fd, {Opcode::Set, "key1", "value1"});
        auto resp = recv_response(fd);
        EXPECT_EQ(resp.status, Status::Ok);

        // GET
        send_request(fd, {Opcode::Get, "key1", ""});
        resp = recv_response(fd);
        EXPECT_EQ(resp.status, Status::Ok);
        EXPECT_EQ(resp.value, "value1");

        // GET nonexistent
        send_request(fd, {Opcode::Get, "nope", ""});
        resp = recv_response(fd);
        EXPECT_EQ(resp.status, Status::NotFound);

        done = true;
        ::close(fd);
    });

    ctx.run();
    client.join();
    ::close(listen_fd);
    EXPECT_TRUE(done.load());
}

TEST_F(CoordinatorTest, LocalDelete) {
    IoUringContext ctx(128);
    LocalStore store(4 * 1024 * 1024);
    Coordinator coord(ctx, store, ring_, members_, self_);

    fd_t listen_fd = make_listen_socket();
    uint16_t port = get_bound_port(listen_fd);
    std::atomic<bool> done{false};

    auto server = [&]() -> Task<> {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int32_t client_fd =
            co_await async_accept(ctx, listen_fd, &client_addr, &addr_len);
        if (client_fd >= 0) {
            co_await coord.handle_connection(client_fd);
        }
        ctx.stop();
    };

    auto task = server();
    task.start();

    std::thread client([port, &done] {
        int fd = blocking_connect(port);
        ASSERT_GE(fd, 0);

        send_request(fd, {Opcode::Set, "k", "v"});
        recv_response(fd);

        send_request(fd, {Opcode::Del, "k", ""});
        auto resp = recv_response(fd);
        EXPECT_EQ(resp.status, Status::Ok);

        send_request(fd, {Opcode::Get, "k", ""});
        resp = recv_response(fd);
        EXPECT_EQ(resp.status, Status::NotFound);

        done = true;
        ::close(fd);
    });

    ctx.run();
    client.join();
    ::close(listen_fd);
    EXPECT_TRUE(done.load());
}

TEST_F(CoordinatorTest, ProxyToRemotePeer) {
    // Set up two io_uring contexts: one for the "peer" (remote store) and
    // one for the "coordinator" that proxies.
    IoUringContext peer_ctx(128);
    IoUringContext coord_ctx(128);

    // Peer node
    NodeId peer_id{"127.0.0.1", 7001};
    LocalStore peer_store(4 * 1024 * 1024);
    peer_store.put("remote_key", "remote_value");

    fd_t peer_listen = make_listen_socket();
    uint16_t peer_port = get_bound_port(peer_listen);

    // Register peer in coordinator's view with correct data_port.
    MemberList coord_members;
    coord_members.add_node(NodeInfo{self_, 8000});
    coord_members.add_node(NodeInfo{peer_id, peer_port});

    // Build a ring where "remote_key" maps to peer_id.
    RingStore coord_ring;
    coord_ring.add_node(self_, 128);
    coord_ring.add_node(peer_id, 128);

    // Find a key that routes to the peer.
    std::string test_key = "remote_key";
    uint32_t hash = hash_key(test_key);
    auto owner = coord_ring.get_node_for_key(hash);

    // If this key doesn't route to peer, try a few others.
    if (!owner.has_value() || *owner == self_) {
        for (int i = 0; i < 10000; ++i) {
            test_key = "probe_key_" + std::to_string(i);
            hash = hash_key(test_key);
            owner = coord_ring.get_node_for_key(hash);
            if (owner.has_value() && *owner == peer_id) break;
        }
    }
    ASSERT_TRUE(owner.has_value());
    ASSERT_EQ(*owner, peer_id);

    // Put the value in the peer store under the found key.
    peer_store.put(test_key, "remote_value");

    // Peer server: handles one connection with the wire protocol.
    auto peer_handler = [&](fd_t client_fd) -> Task<> {
        MemHeader hdr{};
        {
            auto* dst = reinterpret_cast<uint8_t*>(&hdr);
            size_t remaining = sizeof(hdr);
            while (remaining > 0) {
                int32_t n = co_await async_read(peer_ctx, client_fd, dst,
                                                remaining);
                if (n <= 0) { co_await async_close(peer_ctx, client_fd); co_return; }
                dst += n;
                remaining -= static_cast<size_t>(n);
            }
        }

        uint32_t body_len = be32toh(hdr.body_len);
        uint16_t key_len = be16toh(hdr.key_len);
        uint8_t ext_len = hdr.ext_len;
        size_t plen = body_len;

        std::vector<uint8_t> payload(plen);
        if (plen > 0) {
            size_t remaining = plen;
            uint8_t* dst = payload.data();
            while (remaining > 0) {
                int32_t n = co_await async_read(peer_ctx, client_fd, dst,
                                                remaining);
                if (n <= 0) break;
                dst += n;
                remaining -= static_cast<size_t>(n);
            }
        }

        std::string key;
        if (key_len > 0) {
            key.assign(reinterpret_cast<const char*>(payload.data()) + ext_len, key_len);
        }
        auto val = peer_store.get(key);

        Response resp;
        if (val.has_value()) {
            resp = Response{Status::Ok, std::move(*val), be32toh(hdr.opaque), be64toh(hdr.cas)};
        } else {
            resp = Response{Status::NotFound, {}, be32toh(hdr.opaque), be64toh(hdr.cas)};
        }
        auto wire = serialize_response(resp);

        auto* src = wire.data();
        size_t remaining = wire.size();
        while (remaining > 0) {
            int32_t n = co_await async_write(peer_ctx, client_fd, src,
                                             remaining);
            if (n <= 0) break;
            src += n;
            remaining -= static_cast<size_t>(n);
        }

        co_await async_close(peer_ctx, client_fd);
        peer_ctx.stop();
    };

    auto peer_acceptor = [&]() -> Task<> {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int32_t fd = co_await async_accept(peer_ctx, peer_listen, &addr, &len);
        if (fd >= 0) co_await peer_handler(fd);
    };

    // Coordinator side
    LocalStore coord_store(4 * 1024 * 1024);
    Coordinator coord(coord_ctx, coord_store, coord_ring, coord_members,
                      self_);

    fd_t coord_listen = make_listen_socket();
    uint16_t coord_port = get_bound_port(coord_listen);

    auto coord_acceptor = [&]() -> Task<> {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int32_t fd = co_await async_accept(coord_ctx, coord_listen, &addr,
                                           &len);
        if (fd >= 0) co_await coord.handle_connection(fd);
        coord_ctx.stop();
    };

    // Start both event loops.
    auto peer_task = peer_acceptor();
    peer_task.start();
    std::thread peer_thread([&] { peer_ctx.run(); });

    auto coord_task = coord_acceptor();
    coord_task.start();
    std::thread coord_thread([&] { coord_ctx.run(); });

    // Client sends GET for the key that routes to the peer.
    int fd = blocking_connect(coord_port);
    ASSERT_GE(fd, 0);

    send_request(fd, {Opcode::Get, test_key, ""});
    auto resp = recv_response(fd);
    EXPECT_EQ(resp.status, Status::Ok);
    EXPECT_EQ(resp.value, "remote_value");

    ::close(fd);
    coord_thread.join();
    peer_thread.join();
    ::close(coord_listen);
    ::close(peer_listen);
}

TEST_F(CoordinatorTest, ZeroCopyWithBufferPool) {
    IoUringContext ctx(128);
    LocalStore store(4 * 1024 * 1024);
    BufferPool pool(ctx, 4096, 8);
    Coordinator coord(ctx, store, ring_, members_, self_, &pool);

    fd_t listen_fd = make_listen_socket();
    uint16_t port = get_bound_port(listen_fd);
    std::atomic<bool> done{false};

    auto server = [&]() -> Task<> {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int32_t client_fd = co_await async_accept(ctx, listen_fd, &addr, &len);
        if (client_fd >= 0) co_await coord.handle_connection(client_fd);
        ctx.stop();
    };

    auto task = server();
    task.start();

    std::thread client([port, &done] {
        int fd = blocking_connect(port);
        ASSERT_GE(fd, 0);

        std::string big_value(2048, 'Z');
        send_request(fd, {Opcode::Set, "bigkey", big_value});
        auto resp = recv_response(fd);
        EXPECT_EQ(resp.status, Status::Ok);

        send_request(fd, {Opcode::Get, "bigkey", ""});
        resp = recv_response(fd);
        EXPECT_EQ(resp.status, Status::Ok);
        EXPECT_EQ(resp.value, big_value);

        done = true;
        ::close(fd);
    });

    ctx.run();
    client.join();
    ::close(listen_fd);
    EXPECT_TRUE(done.load());
}

TEST_F(CoordinatorTest, MultipleRequestsOnSameConnection) {
    IoUringContext ctx(128);
    LocalStore store(4 * 1024 * 1024);
    Coordinator coord(ctx, store, ring_, members_, self_);

    fd_t listen_fd = make_listen_socket();
    uint16_t port = get_bound_port(listen_fd);
    std::atomic<bool> done{false};

    auto server = [&]() -> Task<> {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int32_t client_fd = co_await async_accept(ctx, listen_fd, &addr, &len);
        if (client_fd >= 0) co_await coord.handle_connection(client_fd);
        ctx.stop();
    };

    auto task = server();
    task.start();

    std::thread client([port, &done] {
        int fd = blocking_connect(port);
        ASSERT_GE(fd, 0);

        for (int i = 0; i < 100; ++i) {
            std::string key = "key_" + std::to_string(i);
            std::string val = "val_" + std::to_string(i);
            send_request(fd, {Opcode::Set, key, val});
            auto resp = recv_response(fd);
            EXPECT_EQ(resp.status, Status::Ok) << "SET " << key;
        }

        for (int i = 0; i < 100; ++i) {
            std::string key = "key_" + std::to_string(i);
            std::string expected = "val_" + std::to_string(i);
            send_request(fd, {Opcode::Get, key, ""});
            auto resp = recv_response(fd);
            EXPECT_EQ(resp.status, Status::Ok) << "GET " << key;
            EXPECT_EQ(resp.value, expected) << "GET " << key;
        }

        done = true;
        ::close(fd);
    });

    ctx.run();
    client.join();
    ::close(listen_fd);
    EXPECT_TRUE(done.load());
}
