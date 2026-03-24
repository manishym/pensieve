#include "membership/member_list.h"

#include <algorithm>

namespace pensieve {

bool MemberList::add_node(const NodeInfo& info) {
    auto it = nodes_.find(info.id);
    if (it != nodes_.end()) {
        if (info.incarnation > it->second.incarnation ||
            (info.incarnation == it->second.incarnation &&
             state_supersedes(info.state, it->second.state))) {
            it->second = info;
            return true;
        }
        return false;
    }
    nodes_.emplace(info.id, info);
    return true;
}

bool MemberList::remove_node(const NodeId& id) {
    return nodes_.erase(id) > 0;
}

bool MemberList::update_state(const NodeId& id, NodeState new_state,
                              uint64_t incarnation) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;

    auto& n = it->second;
    if (incarnation > n.incarnation) {
        n.state = new_state;
        n.incarnation = incarnation;
        n.last_state_change = std::chrono::steady_clock::now();
        return true;
    }
    if (incarnation == n.incarnation && state_supersedes(new_state, n.state)) {
        n.state = new_state;
        n.last_state_change = std::chrono::steady_clock::now();
        return true;
    }
    return false;
}

std::optional<NodeInfo> MemberList::get_node(const NodeId& id) const {
    auto it = nodes_.find(id);
    if (it != nodes_.end()) return it->second;
    return std::nullopt;
}

std::optional<NodeId> MemberList::random_peer(const NodeId& excluding) {
    auto peers = alive_peers(excluding);
    if (peers.empty()) return std::nullopt;
    std::uniform_int_distribution<size_t> dist(0, peers.size() - 1);
    return peers[dist(rng_)];
}

std::vector<NodeId> MemberList::random_peers(size_t k,
                                             const NodeId& excluding) {
    auto peers = alive_peers(excluding);
    if (peers.size() <= k) return peers;

    std::shuffle(peers.begin(), peers.end(), rng_);
    peers.resize(k);
    return peers;
}

std::vector<NodeId> MemberList::alive_peers(const NodeId& excluding) const {
    std::vector<NodeId> result;
    for (const auto& [id, info] : nodes_) {
        if (id != excluding && info.state == NodeState::Alive) {
            result.push_back(id);
        }
    }
    return result;
}

}  // namespace pensieve
