#include "sdk/circuit_breaker.h"

namespace pensieve {

CircuitBreaker::CircuitBreaker() : cfg_{} {}
CircuitBreaker::CircuitBreaker(Config cfg) : cfg_(cfg) {}

bool CircuitBreaker::allow(const NodeEndpoint& ep) {
    PeerKey key{ep.host, ep.data_port};
    std::lock_guard lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return true;

    auto& e = it->second;
    if (e.state == State::Closed) return true;

    auto now = std::chrono::steady_clock::now();
    if (now - e.opened_at >= cfg_.open_duration) {
        e.state = State::HalfOpen;
        return true;
    }
    return false;
}

void CircuitBreaker::record_success(const NodeEndpoint& ep) {
    PeerKey key{ep.host, ep.data_port};
    std::lock_guard lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    it->second.state = State::Closed;
    it->second.consecutive_failures = 0;
}

void CircuitBreaker::record_failure(const NodeEndpoint& ep) {
    PeerKey key{ep.host, ep.data_port};
    std::lock_guard lock(mu_);
    auto& e = entries_[key];
    ++e.consecutive_failures;
    if (e.consecutive_failures >= cfg_.failure_threshold) {
        e.state = State::Open;
        e.opened_at = std::chrono::steady_clock::now();
    }
}

size_t CircuitBreaker::open_count() const {
    std::lock_guard lock(mu_);
    size_t count = 0;
    for (const auto& [k, e] : entries_) {
        if (e.state == State::Open) ++count;
    }
    return count;
}

}  // namespace pensieve
