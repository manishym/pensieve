#include "coordinator/peer_client.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pensieve {

Task<fd_t> PeerClient::connect(const std::string& host, uint16_t port) {
    fd_t fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) co_return -1;

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
    if (!co_await write_all(peer_fd, wire.data(), wire.size())) {
        co_return Response{Status::Error, {}};
    }

    ResponseHeader hdr{};
    if (!co_await read_exact(peer_fd, &hdr, sizeof(hdr))) {
        co_return Response{Status::Error, {}};
    }

    std::string value;
    if (hdr.value_len > 0) {
        if (pool_) {
            // Zero-copy path: read into registered buffer, then copy out.
            // The buffer stays registered for the kernel transfer, avoiding
            // an extra kernel→userspace copy on large payloads.
            auto handle = pool_->acquire();
            if (handle && handle->size >= hdr.value_len) {
                if (!co_await read_exact(peer_fd, handle->data,
                                         hdr.value_len)) {
                    pool_->release(handle->index);
                    co_return Response{Status::Error, {}};
                }
                value.assign(reinterpret_cast<const char*>(handle->data),
                             hdr.value_len);
                pool_->release(handle->index);
            } else {
                if (handle) pool_->release(handle->index);
                value.resize(hdr.value_len);
                if (!co_await read_exact(peer_fd, value.data(),
                                         hdr.value_len)) {
                    co_return Response{Status::Error, {}};
                }
            }
        } else {
            value.resize(hdr.value_len);
            if (!co_await read_exact(peer_fd, value.data(), hdr.value_len)) {
                co_return Response{Status::Error, {}};
            }
        }
    }

    co_return Response{hdr.status, std::move(value)};
}

Task<bool> PeerClient::read_exact(fd_t fd, void* buf, size_t len) {
    auto* dst = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        int32_t n = co_await async_read(ctx_, fd, dst, remaining);
        if (n <= 0) co_return false;
        dst += n;
        remaining -= static_cast<size_t>(n);
    }
    co_return true;
}

Task<bool> PeerClient::write_all(fd_t fd, const void* buf, size_t len) {
    auto* src = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        int32_t n = co_await async_write(ctx_, fd, src, remaining);
        if (n <= 0) co_return false;
        src += n;
        remaining -= static_cast<size_t>(n);
    }
    co_return true;
}

}  // namespace pensieve
