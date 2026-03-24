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

    // Collect the union of all token boundaries from both rings.
    std::set<uint32_t> boundaries;
    for (const auto& [token, _] : old_ring) boundaries.insert(token);
    for (const auto& [token, _] : new_ring) boundaries.insert(token);

    std::vector<RangeMigration> result;

    // Walk every boundary. For each boundary, the range extends from this
    // boundary (inclusive) to the next boundary (exclusive). We check
    // whether the owner changed between old and new rings.
    auto it = boundaries.begin();
    while (it != boundaries.end()) {
        uint32_t start = *it;
        auto next = std::next(it);

        uint32_t end;
        if (next != boundaries.end()) {
            end = *next;
        } else {
            // Last boundary to wrap-around: the range is [start, first_boundary)
            // where first_boundary wraps around.
            end = *boundaries.begin();
        }

        auto old_owner = owner_at(old_ring, start);
        auto new_owner = owner_at(new_ring, start);

        if (old_owner != new_owner) {
            result.push_back({start, end, old_owner, new_owner});
        }

        it = next;
    }

    return result;
}

}  // namespace pensieve
