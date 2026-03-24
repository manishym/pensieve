#include "membership/disseminator.h"

#include <algorithm>

namespace pensieve {

Disseminator::Disseminator(size_t max_transmissions)
    : max_transmissions_(max_transmissions) {}

void Disseminator::enqueue(MembershipUpdate update) {
    for (auto& entry : queue_) {
        if (entry.update.node == update.node) {
            if (update.incarnation > entry.update.incarnation ||
                (update.incarnation == entry.update.incarnation &&
                 static_cast<uint8_t>(update.type) >
                     static_cast<uint8_t>(entry.update.type))) {
                entry.update = std::move(update);
                entry.transmit_count = 0;
            }
            return;
        }
    }
    queue_.push_back({std::move(update), 0});
}

std::vector<MembershipUpdate> Disseminator::get_updates(size_t limit) {
    std::sort(queue_.begin(), queue_.end(),
              [](const Entry& a, const Entry& b) {
                  return a.transmit_count < b.transmit_count;
              });

    std::vector<MembershipUpdate> result;
    size_t count = std::min(limit, queue_.size());
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        result.push_back(queue_[i].update);
        ++queue_[i].transmit_count;
    }

    std::erase_if(queue_, [this](const Entry& e) {
        return e.transmit_count >= max_transmissions_;
    });

    return result;
}

}  // namespace pensieve
