#pragma once

#include <cstddef>
#include <vector>

#include "membership/swim_message.h"

namespace pensieve {

class Disseminator {
public:
    explicit Disseminator(size_t max_transmissions = 10);

    void enqueue(MembershipUpdate update);

    // Returns up to `limit` updates ordered by least-transmitted first.
    // Increments transmit_count for each returned update.
    // Evicts updates that exceed max_transmissions.
    std::vector<MembershipUpdate> get_updates(size_t limit);

    size_t pending() const { return queue_.size(); }

private:
    struct Entry {
        MembershipUpdate update;
        uint32_t transmit_count = 0;
    };

    size_t max_transmissions_;
    std::vector<Entry> queue_;
};

}  // namespace pensieve
