#include "membership/ring_store.h"

#include <algorithm>

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

void RingStore::remove_node(const NodeId& node) {
    std::lock_guard lock(write_mu_);
    auto new_ring = std::make_shared<Ring>(*ring_);
    std::erase_if(*new_ring,
                  [&](const auto& pair) { return pair.second == node; });
    ring_ = std::move(new_ring);
}

size_t RingStore::size() const {
    return snapshot()->size();
}

}  // namespace pensieve
