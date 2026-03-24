#include "membership/ring_store.h"

#include <algorithm>
#include <unordered_set>

#include "hash/hasher.h"

namespace pensieve {

RingStore::RingStore() : ring_(std::make_shared<const Ring>()) {}

auto RingStore::snapshot() const -> std::shared_ptr<const Ring> {
    std::lock_guard lock(write_mu_);
    return ring_;
}

void RingStore::add_node(const NodeId& node,
                         std::span<const uint32_t> tokens) {
    std::lock_guard lock(write_mu_);
    auto new_ring = std::make_shared<Ring>(*ring_);
    for (uint32_t token : tokens) {
        (*new_ring)[token] = node;
    }
    ring_ = std::move(new_ring);
}

void RingStore::add_node(const NodeId& node, uint32_t num_vnodes) {
    std::vector<uint32_t> tokens;
    tokens.reserve(num_vnodes);
    for (uint32_t i = 0; i < num_vnodes; ++i) {
        tokens.push_back(hash_node_token(node, i));
    }
    add_node(node, tokens);
}

void RingStore::remove_node(const NodeId& node) {
    std::lock_guard lock(write_mu_);
    auto new_ring = std::make_shared<Ring>(*ring_);
    std::erase_if(*new_ring,
                  [&](const auto& pair) { return pair.second == node; });
    ring_ = std::move(new_ring);
}

std::optional<NodeId> RingStore::get_node_for_key(uint32_t hash) const {
    auto snap = snapshot();
    if (snap->empty()) return std::nullopt;

    auto it = snap->lower_bound(hash);
    if (it == snap->end()) {
        it = snap->begin();
    }
    return it->second;
}

std::vector<NodeId> RingStore::get_n_nodes_for_key(uint32_t hash,
                                                   size_t n) const {
    auto snap = snapshot();
    if (snap->empty()) return {};

    std::vector<NodeId> result;
    std::unordered_set<NodeId> seen;
    result.reserve(n);

    auto it = snap->lower_bound(hash);
    if (it == snap->end()) {
        it = snap->begin();
    }

    auto start = it;
    do {
        if (seen.insert(it->second).second) {
            result.push_back(it->second);
            if (result.size() == n) break;
        }
        ++it;
        if (it == snap->end()) {
            it = snap->begin();
        }
    } while (it != start);

    return result;
}

size_t RingStore::size() const {
    return snapshot()->size();
}

}  // namespace pensieve
