#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "protocol/message.h"
#include "sdk/topology_manager.h"

namespace pensieve {

class ConnectionPool {
public:
    explicit ConnectionPool(size_t max_idle_per_node = 4);
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    int acquire(const NodeEndpoint& ep);
    void release(const NodeEndpoint& ep, int fd);
    void discard(int fd);

    static bool send_request(int fd, const Request& req);
    static std::optional<Response> recv_response(int fd);

    size_t idle_count() const;

private:
    struct PeerKey {
        std::string host;
        uint16_t port;
        bool operator==(const PeerKey&) const = default;
    };
    struct PeerKeyHash {
        size_t operator()(const PeerKey& k) const noexcept {
            return std::hash<std::string>{}(k.host) ^
                   (std::hash<uint16_t>{}(k.port) << 16);
        }
    };

    mutable std::mutex mu_;
    size_t max_idle_;
    std::unordered_map<PeerKey, std::deque<int>, PeerKeyHash> pool_;
};

}  // namespace pensieve
