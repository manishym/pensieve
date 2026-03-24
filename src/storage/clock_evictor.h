#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "storage/slab_entry.h"

namespace pensieve {

class ClockEvictor {
    std::vector<SlabEntry*> ring_;
    std::unordered_map<SlabEntry*, size_t> index_;
    size_t hand_ = 0;

    void swap_remove(size_t idx);

public:
    void track(SlabEntry* entry);
    void untrack(SlabEntry* entry);
    SlabEntry* evict_one();
    size_t tracked() const { return ring_.size(); }
};

}  // namespace pensieve
