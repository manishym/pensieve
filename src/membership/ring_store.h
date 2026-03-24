#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "membership/node_info.h"

namespace pensieve {

class RingStore {
public:
    using Ring = std::map<uint32_t, NodeId>;

    static constexpr uint32_t kDefaultVnodes = 128;

    RingStore();

    // Read-side: returns an immutable snapshot. Multiple concurrent readers
    // share the same underlying map until a write creates a new version.
    std::shared_ptr<const Ring> snapshot() const;

    // Write-side (explicit tokens): clones the current ring, applies the
    // mutation, then atomically publishes the new version (clone-and-swap / RCU).
    void add_node(const NodeId& node, std::span<const uint32_t> tokens);

    // Write-side (virtual nodes): generates num_vnodes tokens via
    // hash_node_token() and places them on the ring.
    void add_node(const NodeId& node, uint32_t num_vnodes = kDefaultVnodes);

    void remove_node(const NodeId& node);

    // Lookup: find the node responsible for a given hash position.
    // Uses lower_bound with circular wrap-around.
    std::optional<NodeId> get_node_for_key(uint32_t hash) const;

    // Lookup: walk clockwise collecting up to n distinct physical nodes.
    std::vector<NodeId> get_n_nodes_for_key(uint32_t hash, size_t n) const;

    size_t size() const;

private:
    mutable std::mutex write_mu_;
    std::shared_ptr<const Ring> ring_;
};

}  // namespace pensieve
