#include "membership/member_list.h"

#include <algorithm>

namespace pensieve {

bool MemberList::add_node(const NodeInfo& info) {
    for (auto& n : nodes_) {
        if (n.id == info.id) {
            if (info.incarnation > n.incarnation ||
                (info.incarnation == n.incarnation &&
                 state_supersedes(info.state, n.state))) {
                n = info;
                return true;
            }
            return false;
        }
    }
    nodes_.push_back(info);
    return true;
}

bool MemberList::remove_node(const NodeId& id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [&](const NodeInfo& n) { return n.id == id; });
    if (it == nodes_.end()) return false;
    nodes_.erase(it);
    return true;
}

bool MemberList::update_state(const NodeId& id, NodeState new_state,
                              uint64_t incarnation) {
    for (auto& n : nodes_) {
        if (n.id != id) continue;

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
    return false;
}

std::optional<NodeInfo> MemberList::get_node(const NodeId& id) const {
    for (const auto& n : nodes_) {
        if (n.id == id) return n;
    }
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
    for (const auto& n : nodes_) {
        if (n.id != excluding && n.state == NodeState::Alive) {
            result.push_back(n.id);
        }
    }
    return result;
}

}  // namespace pensieve
