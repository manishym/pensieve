#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "membership/node_info.h"

namespace pensieve {

class RingStore {
public:
    using Ring = std::map<uint32_t, NodeId>;

    RingStore();

    // Read-side: returns an immutable snapshot. Multiple concurrent readers
    // share the same underlying map until a write creates a new version.
    std::shared_ptr<const Ring> snapshot() const;

    // Write-side: clones the current ring, applies the mutation, then
    // atomically publishes the new version (clone-and-swap / RCU).
    void add_node(const NodeId& node, std::span<const uint32_t> tokens);
    void remove_node(const NodeId& node);

    size_t size() const;

private:
    mutable std::mutex write_mu_;
    std::shared_ptr<const Ring> ring_;
};

}  // namespace pensieve
