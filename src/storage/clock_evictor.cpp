#include "storage/clock_evictor.h"

namespace pensieve {

void ClockEvictor::swap_remove(size_t idx) {
    SlabEntry* removed = ring_[idx];
    SlabEntry* back = ring_.back();

    index_.erase(removed);
    if (idx != ring_.size() - 1) {
        ring_[idx] = back;
        index_[back] = idx;
    }
    ring_.pop_back();
}

void ClockEvictor::track(SlabEntry* entry) {
    index_[entry] = ring_.size();
    ring_.push_back(entry);
}

void ClockEvictor::untrack(SlabEntry* entry) {
    auto it = index_.find(entry);
    if (it == index_.end()) return;

    size_t idx = it->second;
    swap_remove(idx);

    if (ring_.empty()) {
        hand_ = 0;
    } else if (hand_ >= ring_.size()) {
        hand_ = 0;
    }
}

SlabEntry* ClockEvictor::evict_one() {
    if (ring_.empty()) return nullptr;

    size_t limit = ring_.size() * 2;
    for (size_t i = 0; i < limit; ++i) {
        if (hand_ >= ring_.size()) hand_ = 0;

        SlabEntry* entry = ring_[hand_];
        if (entry->access_bit == 0) {
            swap_remove(hand_);
            if (hand_ >= ring_.size() && !ring_.empty()) hand_ = 0;
            return entry;
        }

        entry->access_bit = 0;
        hand_ = (hand_ + 1) % ring_.size();
    }

    return nullptr;
}

}  // namespace pensieve
