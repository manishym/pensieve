#pragma once

#include <optional>
#include <random>
#include <vector>

#include "membership/node_info.h"

namespace pensieve {

class MemberList {
public:
    bool add_node(const NodeInfo& info);
    bool remove_node(const NodeId& id);

    bool update_state(const NodeId& id, NodeState new_state,
                      uint64_t incarnation);

    std::optional<NodeInfo> get_node(const NodeId& id) const;
    const std::vector<NodeInfo>& all_nodes() const { return nodes_; }
    size_t size() const { return nodes_.size(); }

    std::optional<NodeId> random_peer(const NodeId& excluding);
    std::vector<NodeId> random_peers(size_t k, const NodeId& excluding);

    std::vector<NodeId> alive_peers(const NodeId& excluding) const;

private:
    std::vector<NodeInfo> nodes_;
    std::mt19937 rng_{std::random_device{}()};
};

}  // namespace pensieve
