#pragma once

#include <cstdint>
#include <functional>
#include <netinet/in.h>
#include <string>

#include "common/types.h"
#include "io/io_uring_context.h"

namespace pensieve {

class TcpConnection;

class TcpListener {
public:
    using AcceptCallback = std::function<void(TcpConnection)>;

    TcpListener(IoUringContext& ctx, const std::string& host, uint16_t port);
    ~TcpListener();

    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    void start(AcceptCallback on_accept);
    fd_t fd() const { return listen_fd_; }

private:
    void submit_accept();
    void handle_accept(int fd);

    IoUringContext& ctx_;
    fd_t listen_fd_ = -1;
    AcceptCallback on_accept_;

    struct sockaddr_in client_addr_{};
    socklen_t client_addr_len_ = sizeof(client_addr_);

    Completion accept_completion_;
    std::function<void(int32_t)> accept_cb_;
};

}  // namespace pensieve
