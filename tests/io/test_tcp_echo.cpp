#include "io/io_uring_context.h"
#include "io/tcp_connection.h"
#include "io/tcp_listener.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <gtest/gtest.h>

using namespace pensieve;

namespace {

int connect_to(const char* host, uint16_t port) {
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

uint16_t get_bound_port(fd_t listen_fd) {
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

struct EchoSession {
    TcpConnection conn;
    char buf[4096]{};

    explicit EchoSession(TcpConnection c) : conn(std::move(c)) {}

    void start() {
        conn.async_read(buf, sizeof(buf), [this](int32_t n) {
            if (n <= 0) {
                conn.close();
                delete this;
                return;
            }
            conn.async_write(buf, static_cast<size_t>(n), [this, n](int32_t w) {
                (void)w;
                conn.close();
                delete this;
            });
        });
    }
};

}  // namespace

TEST(TcpEcho, SingleClient) {
    IoUringContext ctx(64);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    std::atomic<int> echoed{0};

    listener.start([&](TcpConnection conn) {
        auto* session = new EchoSession(std::move(conn));
        session->start();
    });

    std::thread client([port, &echoed, &ctx] {
        int fd = connect_to("127.0.0.1", port);
        ASSERT_GE(fd, 0);

        const char* msg = "hello pensieve";
        ::write(fd, msg, strlen(msg));

        char buf[256]{};
        ssize_t n = ::read(fd, buf, sizeof(buf));
        EXPECT_EQ(n, static_cast<ssize_t>(strlen(msg)));
        EXPECT_STREQ(buf, msg);
        ::close(fd);
        echoed.store(1);

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ctx.stop();
    });

    ctx.run();
    client.join();
    EXPECT_EQ(echoed.load(), 1);
}

TEST(TcpEcho, MultipleClients) {
    IoUringContext ctx(128);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    constexpr int kClients = 10;
    std::atomic<int> done{0};

    listener.start([&](TcpConnection conn) {
        auto* session = new EchoSession(std::move(conn));
        session->start();
    });

    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([port, i, &done, &ctx, kClients] {
            int fd = connect_to("127.0.0.1", port);
            ASSERT_GE(fd, 0);

            std::string msg = "msg-" + std::to_string(i);
            ::write(fd, msg.c_str(), msg.size());

            char buf[256]{};
            ssize_t n = ::read(fd, buf, sizeof(buf));
            EXPECT_EQ(n, static_cast<ssize_t>(msg.size()));
            EXPECT_EQ(std::string(buf, n), msg);
            ::close(fd);

            if (++done >= kClients) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ctx.stop();
            }
        });
    }

    ctx.run();
    for (auto& t : clients) t.join();
    EXPECT_EQ(done.load(), kClients);
}

TEST(TcpEcho, GracefulClose) {
    IoUringContext ctx(64);
    TcpListener listener(ctx, "127.0.0.1", 0);
    uint16_t port = get_bound_port(listener.fd());

    std::atomic<bool> saw_close{false};

    listener.start([&](TcpConnection conn) {
        auto c = std::make_shared<TcpConnection>(std::move(conn));
        auto buf = std::shared_ptr<char[]>(new char[128]{});
        c->async_read(buf.get(), 128,
            [c, buf, &saw_close, &ctx](int32_t n) {
                if (n == 0) {
                    saw_close.store(true);
                }
                ctx.stop();
            });
    });

    std::thread client([port] {
        int fd = connect_to("127.0.0.1", port);
        ASSERT_GE(fd, 0);
        ::close(fd);
    });

    ctx.run();
    client.join();
    EXPECT_TRUE(saw_close.load());
}
