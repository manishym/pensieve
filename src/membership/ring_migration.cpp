#include "membership/ring_migration.h"

#include <set>

namespace pensieve {

namespace {

// Look up the owner of a hash position in a ring.
// Uses lower_bound + wrap-around, same logic as RingStore::get_node_for_key.
NodeId owner_at(const RingStore::Ring& ring, uint32_t hash) {
    auto it = ring.lower_bound(hash);
    if (it == ring.end()) it = ring.begin();
    return it->second;
}

}  // namespace

std::vector<RangeMigration> compute_migrations(
    const RingStore::Ring& old_ring,
    const RingStore::Ring& new_ring) {
    if (old_ring.empty() || new_ring.empty()) return {};

    std::set<uint32_t> boundaries;
    for (const auto& [token, _] : old_ring) boundaries.insert(token);
    for (const auto& [token, _] : new_ring) boundaries.insert(token);

    std::vector<uint32_t> sorted(boundaries.begin(), boundaries.end());
    std::vector<RangeMigration> result;

    // In consistent hashing, each token owns the range (prev_token, token].
    // Walk boundaries and for each one, check whether the ownership of the
    // range ending at that boundary changed between old and new rings.
    for (size_t i = 0; i < sorted.size(); ++i) {
        uint32_t curr = sorted[i];
        uint32_t prev = (i == 0) ? sorted.back() : sorted[i - 1];

        auto old_owner = owner_at(old_ring, curr);
        auto new_owner = owner_at(new_ring, curr);

        if (old_owner != new_owner) {
            result.push_back({prev, curr, old_owner, new_owner});
        }
    }

    return result;
}

}  // namespace pensieve
