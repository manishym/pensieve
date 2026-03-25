#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "membership/node_info.h"

namespace pensieve {

struct NodeEndpoint {
    std::string host;
    uint16_t gossip_port = 0;
    uint16_t data_port = 0;
};

class TopologyManager {
public:
    TopologyManager() = default;
    ~TopologyManager();

    TopologyManager(const TopologyManager&) = delete;
    TopologyManager& operator=(const TopologyManager&) = delete;

    bool bootstrap(const std::string& seed_host, uint16_t seed_port);
    bool refresh();

    void start_background_refresh(std::chrono::seconds interval);
    void stop_background_refresh();

    std::optional<NodeEndpoint> find_owner(std::string_view key) const;
    std::vector<NodeEndpoint> find_successors(std::string_view key,
                                              size_t n) const;

    std::vector<NodeEndpoint> all_endpoints() const;
    size_t node_count() const;

    static bool fetch_topology(const std::string& host, uint16_t port,
                               std::vector<NodeEndpoint>& out);

private:
    using Ring = std::map<uint32_t, size_t>;

    void rebuild_ring();
    void refresh_loop(std::chrono::seconds interval);

    mutable std::mutex mu_;
    std::vector<NodeEndpoint> nodes_;
    Ring ring_;

    std::atomic<bool> running_{false};
    std::thread refresh_thread_;
};

}  // namespace pensieve
