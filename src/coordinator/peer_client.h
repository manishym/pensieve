#pragma once

#include <arpa/inet.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/types.h"
#include "io/awaitable.h"
#include "io/buffer_pool.h"
#include "io/task.h"
#include "membership/node_info.h"
#include "protocol/message.h"

namespace pensieve {

// Async outbound TCP client with a per-peer connection pool.
// Reuses idle connections to amortize the cost of TCP handshakes.
class PeerClient {
public:
    PeerClient(IoUringContext& ctx, BufferPool* pool = nullptr)
        : ctx_(ctx), buf_pool_(pool) {}

    ~PeerClient();

    PeerClient(const PeerClient&) = delete;
    PeerClient& operator=(const PeerClient&) = delete;

    // Acquire a connection to host:port.  Returns a pooled idle connection
    // if available, otherwise opens a new one via io_uring.
    Task<fd_t> acquire(const std::string& host, uint16_t port);

    // Return a connection to the pool for reuse.
    void release(const std::string& host, uint16_t port, fd_t fd);

    // Send a serialized request and read the full response.
    Task<Response> send_request(fd_t peer_fd, const Request& req);

    size_t pool_size() const;

private:
    Task<fd_t> open_connection(const std::string& host, uint16_t port);

    IoUringContext& ctx_;
    BufferPool* buf_pool_;

    struct PeerKey {
        std::string host;
        uint16_t port;
        bool operator==(const PeerKey&) const = default;
    };

    struct PeerKeyHash {
        size_t operator()(const PeerKey& k) const noexcept {
            size_t h1 = std::hash<std::string>{}(k.host);
            size_t h2 = std::hash<uint16_t>{}(k.port);
            return h1 ^ (h2 << 16);
        }
    };

    mutable std::mutex pool_mu_;
    std::unordered_map<PeerKey, std::deque<fd_t>, PeerKeyHash> conn_pool_;
};

}  // namespace pensieve
