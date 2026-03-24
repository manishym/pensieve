#pragma once

#include <cstdint>
#include <vector>

#include "membership/ring_store.h"

namespace pensieve {

struct RangeMigration {
    uint32_t range_start;  // exclusive start (previous token position)
    uint32_t range_end;    // inclusive end (token position); range wraps when end <= start
    NodeId from_node;
    NodeId to_node;
};

// Given two ring snapshots (before and after a membership change),
// compute the minimal set of hash-range transfers needed.
// Each entry describes a contiguous range whose owner changed.
std::vector<RangeMigration> compute_migrations(
    const RingStore::Ring& old_ring,
    const RingStore::Ring& new_ring);

}  // namespace pensieve
