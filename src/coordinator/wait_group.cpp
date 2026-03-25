#include "coordinator/wait_group.h"

namespace pensieve {

bool WaitGroup::try_join(const std::string& key) {
    std::lock_guard lock(mu_);
    auto [it, inserted] = pending_.emplace(
        key, std::make_shared<PendingFetch>());
    return inserted;
}

WaitGroup::WaitAwaitable WaitGroup::wait(const std::string& key) {
    return WaitAwaitable(*this, key);
}

void WaitGroup::complete(const std::string& key,
                         std::optional<std::string> value) {
    std::shared_ptr<PendingFetch> fetch;
    std::vector<std::coroutine_handle<>> to_resume;
    {
        std::lock_guard lock(mu_);
        auto it = pending_.find(key);
        if (it == pending_.end()) return;

        fetch = it->second;
        fetch->result = std::move(value);
        fetch->completed = true;
        to_resume.swap(fetch->waiters);
        pending_.erase(it);
    }

    for (auto h : to_resume) {
        h.resume();
    }
}

size_t WaitGroup::pending_count() const {
    std::lock_guard lock(mu_);
    return pending_.size();
}

// WaitAwaitable implementation

bool WaitGroup::WaitAwaitable::await_suspend(std::coroutine_handle<> h) {
    std::lock_guard lock(wg_.mu_);
    auto it = wg_.pending_.find(key_);
    if (it == wg_.pending_.end() || it->second->completed) {
        fetch_ = (it != wg_.pending_.end()) ? it->second : nullptr;
        return false;
    }
    fetch_ = it->second;
    fetch_->waiters.push_back(h);
    return true;
}

std::optional<std::string> WaitGroup::WaitAwaitable::await_resume() {
    if (fetch_) return fetch_->result;
    return std::nullopt;
}

}  // namespace pensieve
