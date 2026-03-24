#include "io/awaitable.h"
#include "io/io_uring_context.h"
#include "io/task.h"
#include "io/udp_socket.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <thread>
#include <gtest/gtest.h>

using namespace pensieve;

namespace {

sockaddr_in make_addr(const char* host, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    return addr;
}

}  // namespace

TEST(UdpSocket, CreateAndBind) {
    UdpSocket sock("127.0.0.1", 0);
    EXPECT_GE(sock.fd(), 0);
    EXPECT_GT(sock.port(), 0);
}

TEST(UdpSocket, MoveSemantics) {
    UdpSocket a("127.0.0.1", 0);
    fd_t orig_fd = a.fd();

    UdpSocket b(std::move(a));
    EXPECT_EQ(b.fd(), orig_fd);
    EXPECT_EQ(a.fd(), -1);
}

TEST(UdpSocket, LoopbackEcho) {
    IoUringContext ctx(64);
    UdpSocket sender("127.0.0.1", 0);
    UdpSocket receiver("127.0.0.1", 0);

    auto dest = make_addr("127.0.0.1", receiver.port());
    const char* msg = "hello-udp";

    bool done = false;

    auto run = [&]() -> Task<> {
        int32_t sent = co_await async_sendto(ctx, sender.fd(), msg,
                                             std::strlen(msg), dest);
        EXPECT_EQ(sent, static_cast<int32_t>(std::strlen(msg)));

        char buf[256]{};
        sockaddr_in from{};
        int32_t n = co_await async_recvfrom(ctx, receiver.fd(), buf,
                                            sizeof(buf), &from);
        EXPECT_EQ(n, static_cast<int32_t>(std::strlen(msg)));
        EXPECT_EQ(std::string(buf, n), msg);
        EXPECT_EQ(ntohs(from.sin_port), sender.port());

        done = true;
        ctx.stop();
    };

    auto task = run();
    task.start();
    ctx.run();
    EXPECT_TRUE(done);
}

TEST(UdpSocket, MultipleMessages) {
    IoUringContext ctx(64);
    UdpSocket sock_a("127.0.0.1", 0);
    UdpSocket sock_b("127.0.0.1", 0);

    auto addr_b = make_addr("127.0.0.1", sock_b.port());
    auto addr_a = make_addr("127.0.0.1", sock_a.port());
    constexpr int kRounds = 10;
    int completed = 0;

    auto run = [&]() -> Task<> {
        for (int i = 0; i < kRounds; ++i) {
            std::string msg = "msg-" + std::to_string(i);

            co_await async_sendto(ctx, sock_a.fd(), msg.data(), msg.size(),
                                  addr_b);

            char buf[256]{};
            sockaddr_in from{};
            int32_t n = co_await async_recvfrom(ctx, sock_b.fd(), buf,
                                                sizeof(buf), &from);
            EXPECT_EQ(std::string(buf, n), msg);

            co_await async_sendto(ctx, sock_b.fd(), buf,
                                  static_cast<size_t>(n), addr_a);

            char echo[256]{};
            sockaddr_in from2{};
            int32_t n2 = co_await async_recvfrom(ctx, sock_a.fd(), echo,
                                                 sizeof(echo), &from2);
            EXPECT_EQ(std::string(echo, n2), msg);
            ++completed;
        }
        ctx.stop();
    };

    auto task = run();
    task.start();
    ctx.run();
    EXPECT_EQ(completed, kRounds);
}

TEST(UdpSocket, Timeout) {
    IoUringContext ctx(64);

    bool timed_out = false;

    auto run = [&]() -> Task<> {
        int32_t res = co_await async_timeout(ctx, std::chrono::milliseconds(50));
        EXPECT_EQ(res, -ETIME);
        timed_out = true;
        ctx.stop();
    };

    auto task = run();
    task.start();
    ctx.run();
    EXPECT_TRUE(timed_out);
}

TEST(UdpSocket, LargeDatagram) {
    IoUringContext ctx(64);
    UdpSocket sender("127.0.0.1", 0);
    UdpSocket receiver("127.0.0.1", 0);

    auto dest = make_addr("127.0.0.1", receiver.port());
    std::vector<uint8_t> payload(8192, 0xAB);

    bool done = false;

    auto run = [&]() -> Task<> {
        int32_t sent = co_await async_sendto(ctx, sender.fd(), payload.data(),
                                             payload.size(), dest);
        EXPECT_EQ(sent, static_cast<int32_t>(payload.size()));

        std::vector<uint8_t> recv_buf(16384);
        sockaddr_in from{};
        int32_t n = co_await async_recvfrom(ctx, receiver.fd(),
                                            recv_buf.data(), recv_buf.size(),
                                            &from);
        EXPECT_EQ(n, static_cast<int32_t>(payload.size()));
        recv_buf.resize(n);
        EXPECT_EQ(recv_buf, payload);

        done = true;
        ctx.stop();
    };

    auto task = run();
    task.start();
    ctx.run();
    EXPECT_TRUE(done);
}
