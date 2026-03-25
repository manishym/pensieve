#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "sdk/topology_manager.h"

namespace pensieve {

class CircuitBreaker {
public:
    struct Config {
        int failure_threshold = 3;
        std::chrono::milliseconds open_duration{5000};
    };

    CircuitBreaker();
    explicit CircuitBreaker(Config cfg);

    bool allow(const NodeEndpoint& ep);
    void record_success(const NodeEndpoint& ep);
    void record_failure(const NodeEndpoint& ep);

    size_t open_count() const;

private:
    enum class State { Closed, Open, HalfOpen };

    struct Entry {
        State state = State::Closed;
        int consecutive_failures = 0;
        std::chrono::steady_clock::time_point opened_at;
    };

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

    Config cfg_;
    mutable std::mutex mu_;
    std::unordered_map<PeerKey, Entry, PeerKeyHash> entries_;
};

}  // namespace pensieve
