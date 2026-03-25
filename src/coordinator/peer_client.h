#pragma once

#include <arpa/inet.h>
#include <cstdint>
#include <string>

#include "common/types.h"
#include "io/awaitable.h"
#include "io/buffer_pool.h"
#include "io/task.h"
#include "protocol/message.h"

namespace pensieve {

// Async outbound TCP client that connects to a peer node, sends a cache
// request, and receives the response.  When a BufferPool is available the
// value payload is transferred through registered buffers (zero-copy path).
class PeerClient {
public:
    PeerClient(IoUringContext& ctx, BufferPool* pool = nullptr)
        : ctx_(ctx), pool_(pool) {}

    // Open a non-blocking TCP connection to host:port via io_uring.
    Task<fd_t> connect(const std::string& host, uint16_t port);

    // Send a serialized request and read the full response.  Uses registered
    // buffers for the value payload when pool_ is set.
    Task<Response> send_request(fd_t peer_fd, const Request& req);

private:
    // Read exactly `len` bytes from `fd` into `buf`.
    Task<bool> read_exact(fd_t fd, void* buf, size_t len);

    // Write exactly `len` bytes from `buf` to `fd`.
    Task<bool> write_all(fd_t fd, const void* buf, size_t len);

    IoUringContext& ctx_;
    BufferPool* pool_;
};

}  // namespace pensieve
