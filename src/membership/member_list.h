#pragma once

#include <functional>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include "membership/node_info.h"

namespace pensieve {

class MemberList {
public:
    // Fires after any successful add_node, remove_node, or update_state.
    using ChangeCallback = std::function<void(const NodeId&, const NodeInfo&)>;
    void set_on_change(ChangeCallback cb) { on_change_ = std::move(cb); }

    bool add_node(const NodeInfo& info);
    bool remove_node(const NodeId& id);

    bool update_state(const NodeId& id, NodeState new_state,
                      uint64_t incarnation);

    std::optional<NodeInfo> get_node(const NodeId& id) const;
    const std::unordered_map<NodeId, NodeInfo>& all_nodes() const { return nodes_; }
    size_t size() const { return nodes_.size(); }

    std::optional<NodeId> random_peer(const NodeId& excluding);
    std::vector<NodeId> random_peers(size_t k, const NodeId& excluding);

    std::vector<NodeId> alive_peers(const NodeId& excluding) const;

private:
    void notify(const NodeId& id);

    std::unordered_map<NodeId, NodeInfo> nodes_;
    std::mt19937 rng_{std::random_device{}()};
    ChangeCallback on_change_;
};

}  // namespace pensieve
