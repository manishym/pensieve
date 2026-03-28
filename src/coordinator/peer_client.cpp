#include "coordinator/peer_client.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "io/stream_utils.h"

namespace pensieve {

PeerClient::~PeerClient() {
    std::lock_guard lock(pool_mu_);
    for (auto& [key, fds] : conn_pool_) {
        for (fd_t fd : fds) ::close(fd);
    }
}

Task<fd_t> PeerClient::acquire(const std::string& host, uint16_t port) {
    {
        std::lock_guard lock(pool_mu_);
        auto it = conn_pool_.find({host, port});
        if (it != conn_pool_.end() && !it->second.empty()) {
            fd_t fd = it->second.front();
            it->second.pop_front();
            co_return fd;
        }
    }
    co_return co_await open_connection(host, port);
}

void PeerClient::release(const std::string& host, uint16_t port, fd_t fd) {
    std::lock_guard lock(pool_mu_);
    conn_pool_[{host, port}].push_back(fd);
}

size_t PeerClient::pool_size() const {
    std::lock_guard lock(pool_mu_);
    size_t total = 0;
    for (auto& [key, fds] : conn_pool_) total += fds.size();
    return total;
}

Task<fd_t> PeerClient::open_connection(const std::string& host,
                                       uint16_t port) {
    fd_t fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) co_return -1;

    int flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        co_return -1;
    }

    int32_t res = co_await async_connect(ctx_, fd, addr);
    if (res < 0) {
        ::close(fd);
        co_return -1;
    }
    co_return fd;
}

Task<Response> PeerClient::send_request(fd_t peer_fd, const Request& req) {
    auto wire = serialize_request(req);
    if (!co_await write_all(ctx_, peer_fd, wire.data(), wire.size())) {
        co_return Response{Status::Error, {}};
    }

    MemHeader hdr{};
    if (!co_await read_exact(ctx_, peer_fd, &hdr, sizeof(hdr))) {
        co_return Response{Status::Error, {}};
    }

    if (hdr.magic != 0x81) {
        co_return Response{Status::Error, {}};
    }

    uint32_t body_len = be32toh(hdr.body_len);
    std::string value;
    if (body_len > 0) {
        if (buf_pool_) {
            auto handle = buf_pool_->acquire();
            if (handle && handle->size >= body_len) {
                if (!co_await read_exact(ctx_, peer_fd, handle->data,
                                         body_len)) {
                    buf_pool_->release(handle->index);
                    co_return Response{Status::Error, {}};
                }
                value.assign(reinterpret_cast<const char*>(handle->data),
                             body_len);
                buf_pool_->release(handle->index);
            } else {
                if (handle) buf_pool_->release(handle->index);
                value.resize(body_len);
                if (!co_await read_exact(ctx_, peer_fd, value.data(),
                                         body_len)) {
                    co_return Response{Status::Error, {}};
                }
            }
        } else {
            value.resize(body_len);
            if (!co_await read_exact(ctx_, peer_fd, value.data(),
                                     body_len)) {
                co_return Response{Status::Error, {}};
            }
        }
    }

    co_return Response{static_cast<Status>(be16toh(hdr.vbucket)), std::move(value), be32toh(hdr.opaque), be64toh(hdr.cas)};
}

}  // namespace pensieve
