#include "io/tcp_listener.h"
#include "io/tcp_connection.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace pensieve {

TcpListener::TcpListener(IoUringContext& ctx, const std::string& host,
                           uint16_t port)
    : ctx_(ctx) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket() failed");
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);
        throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
    }

    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        ::close(listen_fd_);
        throw std::runtime_error("listen() failed");
    }
}

TcpListener::~TcpListener() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

void TcpListener::start(AcceptCallback on_accept) {
    on_accept_ = std::move(on_accept);
    submit_accept();
}

void TcpListener::submit_accept() {
    client_addr_len_ = sizeof(client_addr_);

    accept_cb_ = [this](int32_t res) {
        if (res >= 0) {
            handle_accept(res);
        }
        submit_accept();
    };
    accept_completion_ = Completion::from_callback(&accept_cb_);

    io_uring_sqe* sqe = ctx_.get_sqe();
    io_uring_prep_accept(sqe, listen_fd_,
                         reinterpret_cast<sockaddr*>(&client_addr_),
                         &client_addr_len_, 0);
    io_uring_sqe_set_data(sqe, &accept_completion_);
    ctx_.submit();
}

void TcpListener::handle_accept(int fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    if (on_accept_) {
        on_accept_(TcpConnection(ctx_, fd));
    }
}

}  // namespace pensieve
