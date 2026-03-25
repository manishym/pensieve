#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pensieve {

class TopologyManager;
class ConnectionPool;
class CircuitBreaker;

class PensieveClient {
public:
    struct Config {
        std::string seed_host;
        uint16_t seed_port = 11211;
        std::chrono::seconds refresh_interval{10};
        size_t max_connections_per_node = 4;
        int max_retries = 2;
        std::chrono::milliseconds circuit_break_duration{5000};
        int circuit_break_threshold = 3;
    };

    struct Stats {
        uint64_t requests_total = 0;
        uint64_t requests_direct = 0;
        uint64_t requests_retried = 0;
        uint64_t ring_refreshes = 0;
        uint64_t circuit_breaks = 0;
        uint64_t last_latency_us = 0;
    };

    explicit PensieveClient(Config cfg);
    ~PensieveClient();

    PensieveClient(const PensieveClient&) = delete;
    PensieveClient& operator=(const PensieveClient&) = delete;

    bool connect();

    std::optional<std::string> get(std::string_view key);
    bool put(std::string_view key, std::string_view value);
    bool del(std::string_view key);

    // Fetch cluster topology information as a human-readable string.
    std::string cluster_info();

    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pensieve
