#include "io/awaitable.h"
#include "io/buffer_pool.h"
#include "io/io_uring_context.h"
#include "io/task.h"
#include "io/tcp_connection.h"
#include "io/tcp_listener.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <numeric>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace {

uint16_t get_bound_port(fd_t listen_fd) {
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

int blocking_connect(const char* host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    for (int attempt = 0; attempt < 100; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            return fd;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::close(fd);
    return -1;
}

void send_all(int fd, const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, p + sent, len - sent);
        ASSERT_GT(n, 0) << "write failed with errno=" << errno;
        sent += n;
    }
}

void recv_all(int fd, void* data, size_t len) {
    auto* p = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::read(fd, p + got, len - got);
        ASSERT_GT(n, 0) << "read failed/eof with errno=" << errno;
        got += n;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 1: Full-Stack Echo (TcpListener + BufferPool + coroutines)
// ---------------------------------------------------------------------------

TEST(E2E, FullStackEcho) {
    IoUringContext ctx(128);
    BufferPool pool(ctx, 4096, 8);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    std::vector<Task<>> sessions;
    std::atomic<int> done{0};

    auto handle_client = [&](fd_t client_fd) -> Task<> {
        auto buf = pool.acquire();
        EXPECT_TRUE(buf.has_value());

        int32_t n = co_await async_read_fixed(ctx, client_fd, buf->data,
                                              buf->size, buf->index);
        if (n > 0) {
            int32_t w = co_await async_write_fixed(ctx, client_fd, buf->data,
                                                   static_cast<size_t>(n),
                                                   buf->index);
            EXPECT_EQ(w, n);
        }

        pool.release(buf->index);
        co_await async_close(ctx, client_fd);

        if (++done >= 1) ctx.stop();
    };

    listener.start([&](TcpConnection conn) {
        fd_t fd = conn.fd();
        sessions.push_back(handle_client(fd));
        sessions.back().start();
    });

    std::thread client([port] {
        int fd = blocking_connect("127.0.0.1", port);
        ASSERT_GE(fd, 0);

        const char* msg = "full-stack-echo";
        send_all(fd, msg, strlen(msg));

        char buf[256]{};
        recv_all(fd, buf, strlen(msg));
        EXPECT_EQ(std::string(buf, strlen(msg)), msg);
        ::close(fd);
    });

    ctx.run();
    client.join();
    EXPECT_EQ(done.load(), 1);
    EXPECT_EQ(pool.available(), pool.count());
}

// ---------------------------------------------------------------------------
// Test 2: Multi-Message Conversation
// ---------------------------------------------------------------------------

TEST(E2E, MultiMessageConversation) {
    IoUringContext ctx(128);
    BufferPool pool(ctx, 4096, 8);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    std::vector<Task<>> sessions;
    std::atomic<int> done{0};
    constexpr int kRounds = 5;

    auto handle_client = [&](fd_t client_fd) -> Task<> {
        for (int i = 0; i < kRounds; ++i) {
            auto buf = pool.acquire();
            EXPECT_TRUE(buf.has_value());

            int32_t n = co_await async_read_fixed(ctx, client_fd, buf->data,
                                                  buf->size, buf->index);
            if (n <= 0) {
                pool.release(buf->index);
                break;
            }

            size_t written = 0;
            while (written < static_cast<size_t>(n)) {
                int32_t w = co_await async_write_fixed(
                    ctx, client_fd, buf->data + written,
                    static_cast<size_t>(n) - written, buf->index);
                if (w <= 0) break;
                written += w;
            }
            pool.release(buf->index);
        }

        co_await async_close(ctx, client_fd);
        if (++done >= 1) ctx.stop();
    };

    listener.start([&](TcpConnection conn) {
        fd_t fd = conn.fd();
        sessions.push_back(handle_client(fd));
        sessions.back().start();
    });

    std::thread client([port] {
        int fd = blocking_connect("127.0.0.1", port);
        ASSERT_GE(fd, 0);

        for (int i = 0; i < kRounds; ++i) {
            std::string msg = "round-" + std::to_string(i);
            send_all(fd, msg.c_str(), msg.size());

            char buf[256]{};
            recv_all(fd, buf, msg.size());
            EXPECT_EQ(std::string(buf, msg.size()), msg);
        }
        ::close(fd);
    });

    ctx.run();
    client.join();
    EXPECT_EQ(done.load(), 1);
}

// ---------------------------------------------------------------------------
// Test 3: Large Payload Transfer (1 MB)
// ---------------------------------------------------------------------------

TEST(E2E, LargePayload) {
    constexpr size_t kBufSize = 4096;
    constexpr size_t kBufCount = 16;
    constexpr size_t kPayloadSize = 1024 * 1024;

    IoUringContext ctx(256);
    BufferPool pool(ctx, kBufSize, kBufCount);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    std::vector<Task<>> sessions;
    std::atomic<int> done{0};

    auto handle_client = [&](fd_t client_fd) -> Task<> {
        while (true) {
            auto buf = pool.acquire();
            if (!buf.has_value()) {
                // Fallback to non-fixed read when pool exhausted
                char tmp[4096];
                int32_t n = co_await async_read(ctx, client_fd, tmp, sizeof(tmp));
                if (n <= 0) break;
                size_t written = 0;
                while (written < static_cast<size_t>(n)) {
                    int32_t w = co_await async_write(ctx, client_fd,
                                                     tmp + written, n - written);
                    if (w <= 0) break;
                    written += w;
                }
                continue;
            }

            int32_t n = co_await async_read_fixed(ctx, client_fd, buf->data,
                                                  buf->size, buf->index);
            if (n <= 0) {
                pool.release(buf->index);
                break;
            }

            size_t written = 0;
            while (written < static_cast<size_t>(n)) {
                int32_t w = co_await async_write_fixed(
                    ctx, client_fd, buf->data + written,
                    static_cast<size_t>(n) - written, buf->index);
                if (w <= 0) break;
                written += w;
            }
            pool.release(buf->index);
        }

        co_await async_close(ctx, client_fd);
        if (++done >= 1) ctx.stop();
    };

    listener.start([&](TcpConnection conn) {
        fd_t fd = conn.fd();
        sessions.push_back(handle_client(fd));
        sessions.back().start();
    });

    std::thread client([port] {
        int fd = blocking_connect("127.0.0.1", port);
        ASSERT_GE(fd, 0);

        std::vector<uint8_t> payload(kPayloadSize);
        std::iota(payload.begin(), payload.end(), static_cast<uint8_t>(0));

        std::thread writer([fd, &payload] {
            send_all(fd, payload.data(), payload.size());
        });

        std::vector<uint8_t> received(kPayloadSize);
        recv_all(fd, received.data(), received.size());

        writer.join();
        EXPECT_EQ(payload, received);
        ::close(fd);
    });

    ctx.run();
    client.join();
    EXPECT_EQ(done.load(), 1);
}

// ---------------------------------------------------------------------------
// Test 4: Buffer Pool Exhaustion Under Concurrency
// ---------------------------------------------------------------------------

TEST(E2E, BufferPoolExhaustion) {
    constexpr int kClients = 8;
    constexpr size_t kSmallPoolSize = 4;

    IoUringContext ctx(256);
    BufferPool pool(ctx, 4096, kSmallPoolSize);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    std::vector<Task<>> sessions;
    std::atomic<int> done{0};

    auto handle_client = [&](fd_t client_fd) -> Task<> {
        auto buf = pool.acquire();
        if (buf.has_value()) {
            int32_t n = co_await async_read_fixed(ctx, client_fd, buf->data,
                                                  buf->size, buf->index);
            if (n > 0) {
                co_await async_write_fixed(ctx, client_fd, buf->data,
                                           static_cast<size_t>(n), buf->index);
            }
            pool.release(buf->index);
        } else {
            char tmp[4096]{};
            int32_t n = co_await async_read(ctx, client_fd, tmp, sizeof(tmp));
            if (n > 0) {
                co_await async_write(ctx, client_fd, tmp,
                                     static_cast<size_t>(n));
            }
        }

        co_await async_close(ctx, client_fd);
        if (++done >= kClients) ctx.stop();
    };

    listener.start([&](TcpConnection conn) {
        fd_t fd = conn.fd();
        sessions.push_back(handle_client(fd));
        sessions.back().start();
    });

    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([port, i] {
            int fd = blocking_connect("127.0.0.1", port);
            ASSERT_GE(fd, 0);

            std::string msg = "exhaust-" + std::to_string(i);
            send_all(fd, msg.c_str(), msg.size());

            char buf[256]{};
            recv_all(fd, buf, msg.size());
            EXPECT_EQ(std::string(buf, msg.size()), msg);
            ::close(fd);
        });
    }

    ctx.run();
    for (auto& t : clients) t.join();
    EXPECT_EQ(done.load(), kClients);
    EXPECT_EQ(pool.available(), pool.count());
}

// ---------------------------------------------------------------------------
// Test 5: Connect-Close-Reconnect Lifecycle
// ---------------------------------------------------------------------------

TEST(E2E, ReconnectLifecycle) {
    constexpr int kCycles = 5;

    IoUringContext ctx(128);
    BufferPool pool(ctx, 4096, 8);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    std::vector<Task<>> sessions;
    std::atomic<int> done{0};

    auto handle_client = [&](fd_t client_fd) -> Task<> {
        auto buf = pool.acquire();
        EXPECT_TRUE(buf.has_value());

        int32_t n = co_await async_read_fixed(ctx, client_fd, buf->data,
                                              buf->size, buf->index);
        if (n > 0) {
            co_await async_write_fixed(ctx, client_fd, buf->data,
                                       static_cast<size_t>(n), buf->index);
        }
        pool.release(buf->index);
        co_await async_close(ctx, client_fd);

        if (++done >= kCycles) ctx.stop();
    };

    listener.start([&](TcpConnection conn) {
        fd_t fd = conn.fd();
        sessions.push_back(handle_client(fd));
        sessions.back().start();
    });

    std::thread client([port] {
        for (int i = 0; i < kCycles; ++i) {
            int fd = blocking_connect("127.0.0.1", port);
            ASSERT_GE(fd, 0) << "reconnect failed at cycle " << i;

            std::string msg = "cycle-" + std::to_string(i);
            send_all(fd, msg.c_str(), msg.size());

            char buf[256]{};
            recv_all(fd, buf, msg.size());
            EXPECT_EQ(std::string(buf, msg.size()), msg);
            ::close(fd);

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    ctx.run();
    client.join();
    EXPECT_EQ(done.load(), kCycles);
    EXPECT_EQ(pool.available(), pool.count());
}

// ---------------------------------------------------------------------------
// Test 6: Concurrent High-Load Stress (100 clients)
// ---------------------------------------------------------------------------

TEST(E2E, StressTest) {
    constexpr int kClients = 100;

    IoUringContext ctx(512);
    BufferPool pool(ctx, 4096, 32);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    std::vector<Task<>> sessions;
    std::atomic<int> done{0};

    auto handle_client = [&](fd_t client_fd) -> Task<> {
        auto buf = pool.acquire();
        if (buf.has_value()) {
            int32_t n = co_await async_read_fixed(ctx, client_fd, buf->data,
                                                  buf->size, buf->index);
            if (n > 0) {
                size_t written = 0;
                while (written < static_cast<size_t>(n)) {
                    int32_t w = co_await async_write_fixed(
                        ctx, client_fd, buf->data + written,
                        static_cast<size_t>(n) - written, buf->index);
                    if (w <= 0) break;
                    written += w;
                }
            }
            pool.release(buf->index);
        } else {
            char tmp[4096]{};
            int32_t n = co_await async_read(ctx, client_fd, tmp, sizeof(tmp));
            if (n > 0) {
                size_t written = 0;
                while (written < static_cast<size_t>(n)) {
                    int32_t w = co_await async_write(ctx, client_fd,
                                                     tmp + written,
                                                     n - written);
                    if (w <= 0) break;
                    written += w;
                }
            }
        }

        co_await async_close(ctx, client_fd);
        if (++done >= kClients) ctx.stop();
    };

    listener.start([&](TcpConnection conn) {
        fd_t fd = conn.fd();
        sessions.push_back(handle_client(fd));
        sessions.back().start();
    });

    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([port, i] {
            int fd = blocking_connect("127.0.0.1", port);
            ASSERT_GE(fd, 0) << "connect failed for client " << i;

            std::string msg = "stress-" + std::to_string(i);
            send_all(fd, msg.c_str(), msg.size());

            char buf[256]{};
            recv_all(fd, buf, msg.size());
            EXPECT_EQ(std::string(buf, msg.size()), msg);
            ::close(fd);
        });
    }

    ctx.run();
    for (auto& t : clients) t.join();
    EXPECT_EQ(done.load(), kClients);
    EXPECT_EQ(pool.available(), pool.count());
}
