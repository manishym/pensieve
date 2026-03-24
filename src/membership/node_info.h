#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace pensieve {

enum class NodeState : uint8_t { Alive, Suspect, Dead, Left };

struct NodeId {
    std::string host;
    uint16_t gossip_port = 0;

    bool operator==(const NodeId&) const = default;
    auto operator<=>(const NodeId&) const = default;
};

struct NodeInfo {
    NodeId id;
    uint16_t data_port = 0;
    NodeState state = NodeState::Alive;
    uint64_t incarnation = 0;
    std::chrono::steady_clock::time_point last_state_change =
        std::chrono::steady_clock::now();
};

inline bool state_supersedes(NodeState proposed, NodeState current) {
    return static_cast<uint8_t>(proposed) > static_cast<uint8_t>(current);
}

}  // namespace pensieve

template <>
struct std::hash<pensieve::NodeId> {
    size_t operator()(const pensieve::NodeId& n) const noexcept {
        size_t h1 = std::hash<std::string>{}(n.host);
        size_t h2 = std::hash<uint16_t>{}(n.gossip_port);
        return h1 ^ (h2 << 16);
    }
};
