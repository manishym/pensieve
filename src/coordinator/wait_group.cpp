#include "coordinator/wait_group.h"

namespace pensieve {

WaitGroup::JoinResult WaitGroup::try_join(const std::string& key) {
    std::lock_guard lock(mu_);
    auto [it, inserted] = pending_.emplace(
        key, std::make_shared<PendingFetch>());
    return {inserted, it->second};
}

WaitGroup::WaitAwaitable WaitGroup::wait(FetchHandle fetch) {
    return WaitAwaitable(std::move(fetch));
}

void WaitGroup::complete(const std::string& key,
                         std::optional<std::string> value) {
    FetchHandle fetch;
    std::vector<std::coroutine_handle<>> to_resume;
    {
        std::lock_guard lock(mu_);
        auto it = pending_.find(key);
        if (it == pending_.end()) return;
        fetch = it->second;
        pending_.erase(it);
    }
    {
        std::lock_guard lock(fetch->mu);
        fetch->result = std::move(value);
        fetch->completed = true;
        to_resume.swap(fetch->waiters);
    }

    for (auto h : to_resume) {
        h.resume();
    }
}

size_t WaitGroup::pending_count() const {
    std::lock_guard lock(mu_);
    return pending_.size();
}

bool WaitGroup::WaitAwaitable::await_ready() const noexcept {
    return fetch_->completed;
}

bool WaitGroup::WaitAwaitable::await_suspend(std::coroutine_handle<> h) {
    std::lock_guard lock(fetch_->mu);
    if (fetch_->completed) return false;
    fetch_->waiters.push_back(h);
    return true;
}

std::optional<std::string> WaitGroup::WaitAwaitable::await_resume() {
    return fetch_->result;
}

}  // namespace pensieve
