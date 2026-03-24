#include "io/awaitable.h"
#include "io/io_uring_context.h"
#include "io/task.h"

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

TEST(Coroutine, NopAwaitable) {
    IoUringContext ctx(64);
    bool completed = false;

    auto coro = [&]() -> Task<> {
        int32_t res = co_await async_nop(ctx);
        EXPECT_EQ(res, 0);
        completed = true;
        ctx.stop();
    };

    auto task = coro();
    task.start();
    ctx.run();

    EXPECT_TRUE(completed);
}

TEST(Coroutine, TaskReturnsValue) {
    IoUringContext ctx(64);

    auto compute = [&]() -> Task<int> {
        co_await async_nop(ctx);
        co_return 42;
    };

    auto outer = [&]() -> Task<> {
        int val = co_await compute();
        EXPECT_EQ(val, 42);
        ctx.stop();
    };

    auto task = outer();
    task.start();
    ctx.run();
}

TEST(Coroutine, TaskChaining) {
    IoUringContext ctx(64);
    std::vector<int> order;

    auto step = [&](int id) -> Task<int> {
        co_await async_nop(ctx);
        order.push_back(id);
        co_return id * 10;
    };

    auto outer = [&]() -> Task<> {
        int a = co_await step(1);
        int b = co_await step(2);
        int c = co_await step(3);
        EXPECT_EQ(a, 10);
        EXPECT_EQ(b, 20);
        EXPECT_EQ(c, 30);
        ctx.stop();
    };

    auto task = outer();
    task.start();
    ctx.run();

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

namespace {

uint16_t get_bound_port(fd_t listen_fd) {
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

int blocking_connect(const char* host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    return -1;
}

}  // namespace

TEST(Coroutine, EchoServer) {
    IoUringContext ctx(128);

    fd_t listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_GE(listen_fd, 0);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = 0;
    srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&srv_addr),
                     sizeof(srv_addr)), 0);
    ASSERT_EQ(::listen(listen_fd, SOMAXCONN), 0);

    uint16_t port = get_bound_port(listen_fd);

    constexpr int kClients = 10;
    std::atomic<int> done{0};

    auto handle_client = [&](fd_t client_fd) -> Task<> {
        char buf[4096];
        while (true) {
            int32_t n = co_await async_read(ctx, client_fd, buf, sizeof(buf));
            if (n <= 0) break;

            size_t written = 0;
            while (written < static_cast<size_t>(n)) {
                int32_t w = co_await async_write(
                    ctx, client_fd, buf + written, n - written);
                if (w <= 0) break;
                written += w;
            }
        }
        co_await async_close(ctx, client_fd);

        if (++done >= kClients) {
            ctx.stop();
        }
    };

    std::vector<Task<>> sessions;

    auto accept_loop2 = [&]() -> Task<> {
        for (int i = 0; i < kClients; ++i) {
            struct sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int32_t client_fd = co_await async_accept(ctx, listen_fd,
                                                       &client_addr, &addr_len);
            if (client_fd < 0) break;
            sessions.push_back(handle_client(client_fd));
            sessions.back().start();
        }
    };

    auto acceptor = accept_loop2();
    acceptor.start();

    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([port, i] {
            int fd = blocking_connect("127.0.0.1", port);
            ASSERT_GE(fd, 0);

            std::string msg = "coroutine-echo-" + std::to_string(i);
            ::write(fd, msg.c_str(), msg.size());

            char buf[256]{};
            size_t total = 0;
            while (total < msg.size()) {
                ssize_t n = ::read(fd, buf + total, sizeof(buf) - total);
                if (n <= 0) break;
                total += n;
            }

            EXPECT_EQ(std::string(buf, total), msg);
            ::close(fd);
        });
    }

    ctx.run();

    for (auto& t : clients) t.join();
    ::close(listen_fd);

    EXPECT_EQ(done.load(), kClients);
}

TEST(Coroutine, ExceptionPropagation) {
    IoUringContext ctx(64);
    bool caught = false;

    auto failing = [&]() -> Task<int> {
        co_await async_nop(ctx);
        throw std::runtime_error("test error");
        co_return 0;  // unreachable
    };

    auto outer = [&]() -> Task<> {
        try {
            co_await failing();
        } catch (const std::runtime_error& e) {
            EXPECT_STREQ(e.what(), "test error");
            caught = true;
        }
        ctx.stop();
    };

    auto task = outer();
    task.start();
    ctx.run();

    EXPECT_TRUE(caught);
}
